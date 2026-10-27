use crate::{
    error::failure,
    limits::{MAX_BUFFERED_BYTES, MAX_PAYLOAD_BYTES, PREOPEN_MAX_FRAMES},
};
use gateway_source::error::GatewayError;
use std::{
    collections::VecDeque,
    io::{Read, Write},
};
use tungstenite::{
    Error, Message, WebSocket,
    protocol::{
        Role, WebSocketConfig,
        frame::{CloseFrame, coding::CloseCode},
    },
};

pub fn websocket_config() -> WebSocketConfig {
    WebSocketConfig::default()
        .write_buffer_size(0)
        .max_write_buffer_size(MAX_BUFFERED_BYTES)
        .max_message_size(Some(MAX_PAYLOAD_BYTES))
        .max_frame_size(Some(MAX_PAYLOAD_BYTES))
}

#[derive(Default)]
pub struct PreopenQueue {
    frames: VecDeque<String>,
    bytes: usize,
}
impl PreopenQueue {
    pub fn push(&mut self, text: String) -> Result<(), GatewayError> {
        if text.len() > MAX_PAYLOAD_BYTES
            || self.frames.len() >= PREOPEN_MAX_FRAMES
            || self.bytes.saturating_add(text.len()) > MAX_BUFFERED_BYTES
        {
            return Err(failure("realtime sideband frame queue overflow"));
        }
        self.bytes += text.len();
        self.frames.push_back(text);
        Ok(())
    }
    pub fn drain(self) -> impl Iterator<Item = String> {
        self.frames.into_iter()
    }
}

/// All accepted data remains charged until BOTH the WebSocket and its actual
/// underlying TLS/socket flush complete. Only one frame enters those buffers
/// at a time. A blocked TLS write therefore cannot hide another queue from the
/// native bound; subsequent frames stay in this owned, counted queue. Like
/// ws8.18.3, the pre-send guard adds only the incoming UTF8 payload; accepted
/// frames then charge their actual plaintext framing, not TLS ciphertext.
pub struct PendingSocket<S> {
    socket: WebSocket<S>,
    queue: VecDeque<(String, usize)>,
    pending: usize,
    in_flight: Option<usize>,
    control_pending: usize,
    masked: bool,
}
impl<S: Read + Write> PendingSocket<S> {
    pub fn new(socket: WebSocket<S>, role: Role) -> Self {
        Self {
            socket,
            queue: VecDeque::new(),
            pending: 0,
            in_flight: None,
            control_pending: 0,
            masked: matches!(role, Role::Client),
        }
    }
    pub fn enqueue(&mut self, text: String) -> Result<(), GatewayError> {
        if text.len() > MAX_PAYLOAD_BYTES
            || self.pending.saturating_add(text.len()) > MAX_BUFFERED_BYTES
        {
            return Err(failure("realtime sideband is not writable"));
        }
        let size = text.len();
        let charge = size + self.header_size(size);
        self.pending += charge;
        self.queue.push_back((text, charge));
        Ok(())
    }
    fn header_size(&self, size: usize) -> usize {
        let extended = if size >= 65_536 {
            8
        } else if size > 125 {
            2
        } else {
            0
        };
        2 + extended + if self.masked { 4 } else { 0 }
    }
    pub fn send(&mut self, text: String) -> Result<(), GatewayError> {
        self.enqueue(text)?;
        self.pump()
    }
    pub fn read(&mut self) -> Result<Message, Error> {
        let message = self.socket.read()?;
        if let Message::Ping(payload) = &message {
            // Tungstenite owns one replaceable pending Pong. Include its actual
            // outgoing framing in the same counter until a successful flush.
            self.pending -= self.control_pending;
            self.control_pending = payload.len() + self.header_size(payload.len());
            self.pending += self.control_pending;
        }
        Ok(message)
    }
    pub fn pump(&mut self) -> Result<(), GatewayError> {
        loop {
            if self.in_flight.is_some() || self.control_pending != 0 {
                match self.socket.flush() {
                    Ok(()) => {
                        self.pending -= self.in_flight.take().unwrap_or(0) + self.control_pending;
                        self.control_pending = 0;
                    }
                    Err(error) if blocked(&error) => return Ok(()),
                    Err(_) => return Err(failure("realtime upstream send failed")),
                }
            }
            let Some((text, charge)) = self.queue.pop_front() else {
                return Ok(());
            };
            self.in_flight = Some(charge);
            match self.socket.write(Message::Text(text.into())) {
                Ok(()) => {}
                Err(error) if blocked(&error) => return Ok(()),
                Err(_) => return Err(failure("realtime upstream send failed")),
            }
        }
    }
    pub fn close(&mut self, code: u16, reason: &str, upstream: bool) {
        let allowed = if upstream {
            code == 1000 || (3000..=4999).contains(&code)
        } else {
            (1000..=4999).contains(&code) && ![1005, 1006, 1015].contains(&code)
        };
        let reason = String::from_utf16_lossy(&reason.encode_utf16().take(100).collect::<Vec<_>>());
        let frame = if allowed && reason.len() <= 123 {
            Some(CloseFrame {
                code: CloseCode::from(code),
                reason: reason.into(),
            })
        } else {
            None
        };
        if self.socket.close(frame).is_err() {
            let _ = self.socket.close(None);
        }
        let _ = self.socket.flush();
    }
}
pub fn blocked(error: &Error) -> bool {
    matches!(error,Error::Io(error) if error.kind()==std::io::ErrorKind::WouldBlock)
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use std::io;
    type TestResult<T> = Result<T, Box<dyn std::error::Error + Send + Sync>>;
    #[derive(Default)]
    struct Blocked;
    impl Read for Blocked {
        fn read(&mut self, _: &mut [u8]) -> io::Result<usize> {
            Err(io::ErrorKind::WouldBlock.into())
        }
    }
    impl Write for Blocked {
        fn write(&mut self, _: &[u8]) -> io::Result<usize> {
            Err(io::ErrorKind::WouldBlock.into())
        }
        fn flush(&mut self) -> io::Result<()> {
            Err(io::ErrorKind::WouldBlock.into())
        }
    }
    #[test]
    fn source_preopen_limits_preserve_order_and_exact_payload_budget() -> TestResult<()> {
        let mut queue = PreopenQueue::default();
        for index in 0..32 {
            queue.push(index.to_string())?;
        }
        assert!(queue.push("overflow".into()).is_err());
        assert_eq!(
            queue.drain().collect::<Vec<_>>(),
            (0..32).map(|i| i.to_string()).collect::<Vec<_>>()
        );
        let mut queue = PreopenQueue::default();
        queue.push("a".repeat(MAX_PAYLOAD_BYTES))?;
        queue.push("b".repeat(MAX_PAYLOAD_BYTES))?;
        assert!(queue.push("x".into()).is_err());
        Ok(())
    }
    #[test]
    fn blocked_writer_matches_pinned_ws_framed_plaintext_and_before_send_threshold()
    -> TestResult<()> {
        let socket = WebSocket::from_raw_socket(Blocked, Role::Client, Some(websocket_config()));
        let mut peer = PendingSocket::new(socket, Role::Client);
        peer.enqueue("a".repeat(MAX_PAYLOAD_BYTES))?;
        peer.pump()?;
        assert_eq!(peer.pending, MAX_PAYLOAD_BYTES + 14);
        assert_eq!(peer.in_flight, Some(MAX_PAYLOAD_BYTES + 14));
        assert!(peer.enqueue("b".repeat(MAX_PAYLOAD_BYTES)).is_err());
        peer.enqueue("b".repeat(MAX_PAYLOAD_BYTES - 14))?;
        peer.pump()?;
        assert_eq!(peer.pending, MAX_BUFFERED_BYTES + 14);
        assert!(peer.enqueue("x".into()).is_err());
        Ok(())
    }
    #[test]
    fn actual_non_draining_socket_cannot_hide_pending_frames_from_the_native_bound()
    -> TestResult<()> {
        let listener = std::net::TcpListener::bind("127.0.0.1:0")?;
        let stream = std::net::TcpStream::connect(listener.local_addr()?)?;
        let (_non_draining_peer, _) = listener.accept()?;
        socket2::SockRef::from(&stream).set_send_buffer_size(1024)?;
        stream.set_nonblocking(true)?;
        let mut peer = PendingSocket::new(
            WebSocket::from_raw_socket(stream, Role::Client, Some(websocket_config())),
            Role::Client,
        );
        let mut refused = false;
        for index in 0..64 {
            if peer.enqueue("x".repeat(MAX_PAYLOAD_BYTES)).is_err() {
                refused = true;
                break;
            }
            peer.pump()?;
            if index == 0 {
                assert_eq!(peer.pending, MAX_PAYLOAD_BYTES + 14);
            }
            assert!(peer.pending <= MAX_BUFFERED_BYTES + 14);
        }
        assert!(refused);
        assert!(peer.in_flight.is_some());
        assert!(peer.pending <= MAX_BUFFERED_BYTES + 14);
        Ok(())
    }

    #[test]
    fn actual_tls_non_draining_writer_retains_framed_plaintext_until_flush() -> TestResult<()> {
        let (server_config, client_config) =
            gateway_native_https_test_support::live_tls::configs()?;
        let listener = std::net::TcpListener::bind("127.0.0.1:0")?;
        let address = listener.local_addr()?;
        let (release, paused) = std::sync::mpsc::sync_channel(1);
        let received = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let server_received = received.clone();
        let server = std::thread::spawn(move || -> TestResult<usize> {
            let (mut socket, _) = listener.accept()?;
            socket.set_read_timeout(Some(std::time::Duration::from_secs(5)))?;
            let mut connection = rustls::ServerConnection::new(server_config)?;
            while connection.is_handshaking() {
                connection.complete_io(&mut socket)?;
            }
            paused.recv_timeout(std::time::Duration::from_secs(5))?;
            let mut tls = rustls::StreamOwned::new(connection, socket);
            let mut total = 0;
            let mut bytes = [0; 65_536];
            while total < MAX_PAYLOAD_BYTES + 14 {
                let count = tls.read(&mut bytes)?;
                assert!(count > 0);
                total += count;
                server_received.store(total, std::sync::atomic::Ordering::Relaxed);
            }
            Ok(total)
        });
        let mut socket = std::net::TcpStream::connect(address)?;
        let original_send_buffer = socket2::SockRef::from(&socket).send_buffer_size()?;
        socket2::SockRef::from(&socket).set_send_buffer_size(1024)?;
        let forced_send_buffer = socket2::SockRef::from(&socket).send_buffer_size()?;
        socket.set_read_timeout(Some(std::time::Duration::from_secs(5)))?;
        let mut connection = rustls::ClientConnection::new(
            client_config,
            rustls::pki_types::ServerName::try_from("127.0.0.1")?,
        )?;
        while connection.is_handshaking() {
            connection.complete_io(&mut socket)?;
        }
        socket.set_nonblocking(true)?;
        let tls = rustls::StreamOwned::new(connection, socket);
        let mut peer = PendingSocket::new(
            WebSocket::from_raw_socket(tls, Role::Client, Some(websocket_config())),
            Role::Client,
        );
        peer.send("x".repeat(MAX_PAYLOAD_BYTES))?;
        assert_eq!(peer.pending, MAX_PAYLOAD_BYTES + 14);
        assert_eq!(peer.in_flight, Some(MAX_PAYLOAD_BYTES + 14));
        assert!(peer.send("x".repeat(MAX_PAYLOAD_BYTES)).is_err());
        // Pressure is proved above; the independent drain phase uses the real
        // original capacity rather than the fixture's artificial 1KiB limit.
        let socket = &peer.socket.get_ref().sock;
        let restore_result =
            socket2::SockRef::from(socket).set_send_buffer_size(original_send_buffer);
        let restored_send_buffer = socket2::SockRef::from(socket).send_buffer_size();
        eprintln!(
            "SYNTHETIC_TLS_DRAIN_BUFFER original={original_send_buffer} forced={forced_send_buffer} restore={restore_result:?} restored={restored_send_buffer:?}"
        );
        restore_result?;
        restored_send_buffer?;
        release.send(())?;
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);
        while peer.pending != 0 {
            assert!(
                std::time::Instant::now() < deadline,
                "TLS drain deadline: pending={} received={} original_buffer={original_send_buffer} forced_buffer={forced_send_buffer}",
                peer.pending,
                received.load(std::sync::atomic::Ordering::Relaxed)
            );
            peer.pump()?;
            std::thread::sleep(std::time::Duration::from_millis(2));
        }
        assert_eq!(
            server
                .join()
                .map_err(|_| io::Error::other("fixture thread panicked"))??,
            MAX_PAYLOAD_BYTES + 14
        );
        assert_eq!(peer.in_flight, None);
        Ok(())
    }
}
