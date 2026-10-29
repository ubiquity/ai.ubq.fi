//! One finite native sideband connection. The trusted connector is fixed to the
//! approved destination; no URL or credential is accepted from the upgrade.
pub mod authority;
mod relay;
pub mod socket;
use authority::NativeLiveAuthority;
use gateway_auth_native_proof::native_live::AuthenticatedNativeLive;
use gateway_auth_primitives::crypto::base64url;
use gateway_native_live_buffer::upgrade::{
    NativeLiveHandler, NativeLiveOutcome, NativeLiveUpgradeRequest,
};
use gateway_native_live_buffer::{
    error::failure,
    limits::MAX_PAYLOAD_BYTES,
    queue::{PendingSocket, PreopenQueue, blocked},
};
use gateway_source::error::GatewayError;
use socket::NativeLiveConnector;
use std::{
    io::Write,
    net::TcpStream,
    sync::Arc,
    time::{Duration, Instant},
};
use tungstenite::{
    Message,
    handshake::server::{create_response, write_response},
    protocol::frame::CloseFrame,
};

pub const HANDSHAKE_TIMEOUT_MS: u64 = 10_000;

pub struct NativeLiveService {
    authority: Arc<dyn NativeLiveAuthority>,
    connector: Arc<dyn NativeLiveConnector>,
    tls: Arc<rustls::ClientConfig>,
    server_name: rustls::pki_types::ServerName<'static>,
}
impl NativeLiveService {
    #[cfg(test)]
    pub(crate) fn with_test_tls(mut self, tls: Arc<rustls::ClientConfig>) -> Self {
        self.tls = tls;
        self
    }
    /// Constructor-only fixed-destination connector. No unrestricted connector
    /// is installed by default. Runtime configuration must prove OS enforcement.
    pub fn configured(
        authority: Arc<dyn NativeLiveAuthority>,
        connector: Arc<dyn NativeLiveConnector>,
    ) -> Result<Self, GatewayError> {
        let roots =
            rustls::RootCertStore::from_iter(webpki_roots::TLS_SERVER_ROOTS.iter().cloned());
        Ok(Self {
            authority,
            connector,
            tls: Arc::new(
                rustls::ClientConfig::builder()
                    .with_root_certificates(roots)
                    .with_no_client_auth(),
            ),
            server_name: rustls::pki_types::ServerName::try_from("api.openai.com")
                .map_err(|_| unavailable())?,
        })
    }
}
impl NativeLiveHandler for NativeLiveService {
    fn handle(
        &self,
        stream: &mut TcpStream,
        request: NativeLiveUpgradeRequest,
        proof: AuthenticatedNativeLive,
    ) -> Result<NativeLiveOutcome, GatewayError> {
        proof.require_upgrade(
            &request.action_ref,
            gateway_auth_native_proof::native_context::now_ms()?,
        )?;
        let mut random = [0; 32];
        getrandom::fill(&mut random).map_err(|_| unavailable())?;
        let connection_id = base64url(&random);
        let authorization = self.authority.authorize(
            &request.action_ref,
            &connection_id,
            Instant::now() + Duration::from_secs(10),
        )?;
        if authorization.fence.owner_id != proof.owner_id()
            || authorization.fence.owner_epoch != proof.owner_epoch()
        {
            return Err(unavailable());
        }
        let response = sideband_response(&request.headers)?;
        // Once a101 write begins, the HTTP dispatcher must never receive an
        // error that it could render as a second JSON/HTTP response.
        let outcome = response_committed(stream, |stream| {
            write_response(&mut *stream, &response).map_err(|_| unavailable())?;
            stream.flush().map_err(|_| unavailable())?;
            relay::bridge_after_commit(self, stream, request, authorization)
        });
        Ok(outcome)
    }
}

fn response_committed(
    stream: &mut TcpStream,
    operation: impl FnOnce(&mut TcpStream) -> Result<NativeLiveOutcome, GatewayError>,
) -> NativeLiveOutcome {
    match operation(stream) {
        Ok(outcome) => outcome,
        Err(_) => {
            let _ = stream.shutdown(std::net::Shutdown::Both);
            NativeLiveOutcome::TransportUnknown
        }
    }
}

fn read_downstream<S: std::io::Read + Write>(
    down: &mut PendingSocket<TcpStream>,
    mut up: Option<&mut PendingSocket<S>>,
    preopen: &mut PreopenQueue,
) -> Result<Option<(u16, String)>, GatewayError> {
    loop {
        let text = match down.read() {
            Ok(Message::Text(text)) => Some(text.to_string()),
            Ok(Message::Binary(bytes)) => Some(decode_text(&bytes)),
            Ok(Message::Close(frame)) => return Ok(Some(close_value(frame))),
            Ok(_) => None,
            Err(error) if blocked(&error) => return Ok(None),
            Err(_) => return Err(failure("realtime sideband frame exceeds payload limit")),
        };
        if let Some(text) = text {
            if text.len() > MAX_PAYLOAD_BYTES {
                return Err(failure("realtime sideband frame exceeds payload limit"));
            }
            if let Some(peer) = up.as_mut() {
                peer.send(text)?;
            } else {
                preopen.push(text)?;
            }
        }
    }
}
fn decode_text(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes.strip_prefix(&[0xef, 0xbb, 0xbf]).unwrap_or(bytes)).into_owned()
}
fn close_value(frame: Option<CloseFrame>) -> (u16, String) {
    frame.map_or((1005, String::new()), |frame| {
        (u16::from(frame.code), frame.reason.to_string())
    })
}
pub(super) fn unavailable() -> GatewayError {
    failure("Native live transport is unavailable")
}

fn sideband_response(
    headers: &[(String, String)],
) -> Result<tungstenite::handshake::server::Response, GatewayError> {
    let mut builder = tungstenite::http::Request::builder()
        .method("GET")
        .uri(gateway_auth_native_contracts::native_live::NATIVE_LIVE_PATH);
    let mut version_seen = false;
    let mut key_seen = false;
    for (name, value) in headers {
        if name.eq_ignore_ascii_case("sec-websocket-version") {
            if version_seen || !(value == "8" || value == "13") {
                return Err(unavailable());
            }
            version_seen = true;
            // RFC6455 framing is shared. The source ws server accepts both
            // 8 and 13; tungstenite's response builder accepts 13 only.
            builder = builder.header(name, "13");
            continue;
        }
        if name.eq_ignore_ascii_case("sec-websocket-key") {
            if key_seen
                || value.len() != 24
                || !value.ends_with("==")
                || !value.as_bytes()[..22]
                    .iter()
                    .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'+' | b'/'))
            {
                return Err(unavailable());
            }
            key_seen = true;
        }
        builder = builder.header(name, value);
    }
    let upgrade = builder.body(()).map_err(|_| unavailable())?;
    let response = create_response(&upgrade).map_err(|_| unavailable())?;
    Ok(response)
}

#[cfg(test)]
mod tests;
