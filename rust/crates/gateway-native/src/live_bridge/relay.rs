//! Private phases of the existing native live operation; no new trust or public constructor.
use super::authority::{LiveAuthorization, NativeLiveAuthority};
use super::socket::LiveDialContext;
use super::{
    HANDSHAKE_TIMEOUT_MS, NativeLiveOutcome, NativeLiveService, NativeLiveUpgradeRequest,
    close_value, read_downstream, socket, unavailable,
};
use gateway_native_authority_wire::live::NativeLiveRetirement;
use gateway_native_live_buffer::queue::{PendingSocket, PreopenQueue, blocked, websocket_config};
use gateway_source::error::GatewayError;
use std::{
    net::TcpStream,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
        mpsc,
    },
    thread,
    time::{Duration, Instant},
};
use tungstenite::{Message, WebSocket, protocol::Role};

pub(super) fn bridge_after_commit(
    service: &NativeLiveService,
    stream: &mut TcpStream,
    request: NativeLiveUpgradeRequest,
    authorization: LiveAuthorization,
) -> Result<NativeLiveOutcome, GatewayError> {
    stream.set_nonblocking(true).map_err(|_| unavailable())?;
    let socket = WebSocket::from_partially_read(
        stream.try_clone().map_err(|_| unavailable())?,
        request.buffered,
        Role::Server,
        Some(websocket_config()),
    );
    let mut downstream = PendingSocket::new(socket, Role::Server);
    let mut preopen = PreopenQueue::default();
    // Consume already-buffered frames and close before the first upstream
    // dial. The same reader stays active while the dial thread awaits TLS.
    match read_downstream::<socket::UpstreamSocket>(&mut downstream, None, &mut preopen) {
        Ok(Some((code, reason))) => {
            downstream.close(code, &reason, false);
            return Ok(NativeLiveOutcome::CloseRequested { code });
        }
        Err(error) => {
            downstream.close(1011, &error.envelope.error.message, false);
            return Ok(NativeLiveOutcome::CloseRequested { code: 1011 });
        }
        Ok(None) => {}
    }
    let alive = Arc::new(AtomicBool::new(true));
    let context = LiveDialContext::new(
        alive.clone(),
        stream.try_clone().map_err(|_| unavailable())?,
        Instant::now() + Duration::from_millis(HANDSHAKE_TIMEOUT_MS),
    );
    let authority = service.authority.clone();
    let connector = service.connector.clone();
    let tls = service.tls.clone();
    let server_name = service.server_name.clone();
    let (sender, receiver) = mpsc::sync_channel(1);
    let dial = thread::spawn(move || {
        let result = socket::dial(
            authority.as_ref(),
            connector.as_ref(),
            tls,
            server_name,
            authorization,
            &context,
        );
        let _ = sender.send(result);
    });

    Ok(NativeRelay {
        retired: None,
        upstream: None,
        dial,
        receiver,
        alive,
        preopen,
        downstream,
    }
    .run(service.authority.as_ref()))
}
// Declaration order retains the old locals' reverse cleanup order after the explicit close/join.
struct NativeRelay {
    retired: Option<(Box<LiveAuthorization>, u16, Instant)>,
    upstream: Option<PendingSocket<socket::UpstreamSocket>>,
    dial: thread::JoinHandle<()>,
    receiver: mpsc::Receiver<socket::DialResult>,
    alive: Arc<AtomicBool>,
    preopen: PreopenQueue,
    downstream: PendingSocket<TcpStream>,
}
impl NativeRelay {
    fn run(mut self, authority: &dyn NativeLiveAuthority) -> NativeLiveOutcome {
        let finish = loop {
            match read_downstream(
                &mut self.downstream,
                self.upstream.as_mut(),
                &mut self.preopen,
            ) {
                Ok(Some(close)) => break close,
                Ok(None) => {}
                Err(error) => break (1011, error.envelope.error.message),
            }
            if self.upstream.is_none()
                && self.retired.is_none()
                && let Some(close) = self.receive_dial()
            {
                break close;
            }
            if let Some(close) = self.retire(authority) {
                break close;
            }
            if let Some(close) = self.read_upstream() {
                break close;
            }
            if self.downstream.pump().is_err() {
                break (1011, "realtime sideband is not writable".into());
            }
            thread::sleep(Duration::from_millis(2));
        };
        self.alive.store(false, Ordering::Release);
        if let Some(peer) = self.upstream.as_mut() {
            peer.close(finish.0, &finish.1, true);
        }
        self.downstream.close(finish.0, &finish.1, false);
        let _ = self.dial.join();
        NativeLiveOutcome::CloseRequested { code: finish.0 }
    }
    fn receive_dial(&mut self) -> Option<(u16, String)> {
        match self.receiver.try_recv() {
            Ok(socket::DialResult::Connected(socket)) => {
                let mut peer = PendingSocket::new(*socket, Role::Client);
                let frames = std::mem::take(&mut self.preopen);
                let mut failed = false;
                for text in frames.drain() {
                    if peer.send(text).is_err() {
                        failed = true;
                        break;
                    }
                }
                if failed {
                    return Some((1011, "realtime upstream is not draining frames".into()));
                }
                self.upstream = Some(peer);
            }
            Ok(socket::DialResult::Rejected(authorization, status)) => {
                self.retired = Some((authorization, status, Instant::now()))
            }
            Ok(socket::DialResult::Failed) | Err(mpsc::TryRecvError::Disconnected) => {
                return Some((1011, "realtime upstream unavailable".into()));
            }
            Err(mpsc::TryRecvError::Empty) => {}
        }
        None
    }
    fn retire(&mut self, authority: &dyn NativeLiveAuthority) -> Option<(u16, String)> {
        let Some((authorization, status, retry_at)) = &mut self.retired else {
            return None;
        };
        if *status != 404 && *status != 410 {
            return Some((1011, "realtime upstream rejected the sideband".into()));
        }
        if Instant::now() >= *retry_at {
            match authority.retire(
                authorization,
                *status,
                Instant::now() + Duration::from_secs(10),
            ) {
                Ok(NativeLiveRetirement::Retired) => {
                    return Some((1011, "realtime upstream rejected the sideband".into()));
                }
                Ok(NativeLiveRetirement::Pending | NativeLiveRetirement::Unknown) | Err(_) => {
                    *retry_at = Instant::now() + Duration::from_millis(250)
                }
            }
        }
        None
    }
    fn read_upstream(&mut self) -> Option<(u16, String)> {
        let peer = self.upstream.as_mut()?;
        loop {
            match peer.read() {
                Ok(Message::Text(text)) => {
                    if self.downstream.send(text.to_string()).is_err() {
                        return Some((1011, "realtime sideband is not writable".into()));
                    }
                }
                Ok(Message::Close(frame)) => return Some(close_value(frame)),
                Ok(_) => {}
                Err(error) if blocked(&error) => break,
                Err(_) => return Some((1011, "realtime upstream error".into())),
            }
        }
        if peer.pump().is_err() {
            return Some((1011, "realtime upstream is not draining frames".into()));
        }
        None
    }
}
