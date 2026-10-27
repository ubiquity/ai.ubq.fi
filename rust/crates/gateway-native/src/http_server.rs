//! Fixed authenticated HTTP companion. No generic URL, command, file or
//! configuration ingress. Supervisor data remains readonly and source sampled.
use crate::live_bridge::{NativeLiveHandler, NativeLiveUpgradeRequest};
use crate::owner_control::{NativeControlContext, NativeOwnerRenewalHandler};
use gateway_auth_native_contracts::native_live::{NATIVE_LIVE_ACTION_HEADER, NATIVE_LIVE_PATH};
use gateway_auth_native_proof::native_auth::{
    AuthenticatedNativeOwner, NativeAuthHeaders, NativeAuthenticator, NativeCapability,
    NativeTransportIdentity,
};
use gateway_auth_native_proof::native_live::NativeLiveAuthenticator;
use gateway_auth_native_protocol::native_replay::StoreNonceAuthority;
use gateway_auth_native_wire::control::{
    NATIVE_RENEWAL_MAX_MS, NativeControlDisposition, NativeControlEnvelope, NativeControlReason,
    NativeControlResponse,
};
use gateway_auth_native_wire::native_read::{
    NATIVE_CONTROL_PATH, NATIVE_FOLLOW_PATH, NATIVE_KEY_HEADER, NATIVE_MAC_HEADER,
    NATIVE_NONCE_HEADER, NATIVE_RESPONSE_MAC_HEADER, NATIVE_RPC_PATH, NATIVE_TIME_HEADER,
    NATIVE_VERSION_HEADER,
};
use gateway_native_http_framing::{
    request::{HttpRequest, IncomingRequest, read_request, validate_live_upgrade},
    response::{disconnected, now_ms, write_error},
};
use gateway_native_replay::nonce_sqlite::SqliteNonceStore;
use gateway_native_supervisor_data::data::SupervisorData;
use gateway_source::error::GatewayError;
use std::{
    io::Write,
    net::{TcpListener, TcpStream},
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
    time::{Duration, Instant},
};

pub struct NativeHttpService {
    identity: NativeTransportIdentity,
    nonces: SqliteNonceStore,
    data: SupervisorData,
    renewal: Option<Arc<dyn NativeOwnerRenewalHandler>>,
    live_sideband: Option<Arc<dyn NativeLiveHandler>>,
    imports: Option<Arc<dyn crate::import_ingress::ImportReceiver>>,
}
impl NativeHttpService {
    pub fn configured(
        identity: NativeTransportIdentity,
        nonces: SqliteNonceStore,
        data: SupervisorData,
    ) -> Self {
        Self {
            identity,
            nonces,
            data,
            renewal: None,
            live_sideband: None,
            imports: None,
        }
    }
    pub fn with_owner_renewal(mut self, handler: impl NativeOwnerRenewalHandler + 'static) -> Self {
        self.renewal = Some(Arc::new(handler));
        self
    }
    pub fn with_live_sideband(mut self, handler: impl NativeLiveHandler + 'static) -> Self {
        self.live_sideband = Some(Arc::new(handler));
        self
    }
    /// Fixed trusted import construction: real shared authority/owner/config, no auth callback.
    pub fn with_imports<S, I, H>(
        mut self,
        authority: gateway_authority::store::Authority<S, I>,
        owner: gateway_authority::native_control::receiver::RegisteredRenewalOwner,
        current_config: gateway_auth::types::AuthConfig,
        handler: H,
    ) -> Self
    where
        S: gateway_core::state::AuthorityStore + Send + Sync + 'static,
        I: gateway_authority::store::TokenSource + Send + Sync + 'static,
        H: crate::native_import::NativeImportHandler + 'static,
    {
        self.imports = Some(crate::import_ingress::configured(
            authority,
            owner,
            current_config,
            handler,
        ));
        self
    }
    pub fn serve(self, listener: TcpListener) -> Result<(), GatewayError> {
        gateway_native_connector::artifact::require_production_artifact()?;
        let service = Arc::new(self);
        let active = Arc::new(AtomicUsize::new(0));
        for stream in listener.incoming() {
            let stream = stream.map_err(|_| unavailable())?;
            if active.fetch_add(1, Ordering::AcqRel) >= 64 {
                active.fetch_sub(1, Ordering::AcqRel);
                let mut stream = stream;
                let _ = write_error(
                    &mut stream,
                    GatewayError::unavailable(
                        "Native listener is at capacity",
                        "native_listener_capacity",
                    ),
                );
                continue;
            }
            let service = service.clone();
            let active = active.clone();
            std::thread::spawn(move || {
                let mut stream = stream;
                if let Err(error) = service.handle(&mut stream) {
                    let _ = write_error(&mut stream, error);
                }
                active.fetch_sub(1, Ordering::AcqRel);
            });
        }
        Ok(())
    }
    pub(crate) fn serve_connection(&self, stream: &mut TcpStream) -> Result<(), GatewayError> {
        gateway_native_connector::artifact::require_production_artifact()?;
        if let Err(error) = self.handle(stream) {
            write_error(stream, error)?;
        }
        Ok(())
    }
    fn handle(&self, stream: &mut TcpStream) -> Result<(), GatewayError> {
        let peer = stream.peer_addr().map_err(|_| unavailable())?;
        if !peer.ip().is_loopback() {
            return Err(forbidden());
        }
        stream
            .set_read_timeout(Some(Duration::from_secs(5)))
            .map_err(|_| unavailable())?;
        stream
            .set_write_timeout(Some(Duration::from_secs(5)))
            .map_err(|_| unavailable())?;
        let request = match read_request(stream)? {
            IncomingRequest::Legacy(request) => request,
            IncomingRequest::Import(head) => {
                return self.imports.as_ref().ok_or_else(unavailable)?.receive(
                    stream,
                    head,
                    &self.identity,
                    &self.nonces,
                );
            }
        };
        let live_upgrade = request.method == "GET" && request.path == NATIVE_LIVE_PATH;
        if !live_upgrade
            && (request.method != "POST"
                || !matches!(
                    request.path.as_str(),
                    NATIVE_RPC_PATH | NATIVE_FOLLOW_PATH | NATIVE_CONTROL_PATH
                ))
        {
            return Err(GatewayError::new(
                404,
                "Unknown native operation",
                Some("native_route_invalid"),
            ));
        }
        let headers = NativeAuthHeaders {
            version: request_header(&request, NATIVE_VERSION_HEADER)?,
            key_id: request_header(&request, NATIVE_KEY_HEADER)?,
            time_ms: request_header(&request, NATIVE_TIME_HEADER)?,
            nonce: request_header(&request, NATIVE_NONCE_HEADER)?,
            mac: request_header(&request, NATIVE_MAC_HEADER)?,
        };
        let authority = StoreNonceAuthority::new(&self.nonces);
        if live_upgrade {
            validate_live_upgrade(&request)?;
            let action_ref = request_header(&request, NATIVE_LIVE_ACTION_HEADER)?.to_owned();
            let proof = futures_executor::block_on(
                NativeLiveAuthenticator::new(&self.identity, &authority).authenticate_upgrade(
                    headers,
                    &action_ref,
                    now_ms()?,
                ),
            )?;
            let handler = self.live_sideband.as_ref().ok_or_else(unavailable)?;
            return handler
                .handle(
                    stream,
                    NativeLiveUpgradeRequest {
                        action_ref,
                        headers: request.headers,
                        buffered: request.buffered,
                    },
                    proof,
                )
                .map(|_transport_outcome| ());
        }
        let proof = futures_executor::block_on(
            NativeAuthenticator::new(&self.identity, &authority).authenticate(
                &request.path,
                headers,
                &request.body,
                now_ms()?,
            ),
        )?;
        if request.path == NATIVE_CONTROL_PATH {
            return self.renew_control(stream, &request, &proof, headers);
        }
        gateway_native_supervisor_http::supervisor_http::receive_supervisor_request(
            stream, &request, &proof, &self.data,
        )
    }
    fn renew_control(
        &self,
        stream: &mut TcpStream,
        request: &HttpRequest,
        proof: &AuthenticatedNativeOwner,
        headers: NativeAuthHeaders<'_>,
    ) -> Result<(), GatewayError> {
        proof.require_request(
            NativeCapability::OwnerRenewal,
            NATIVE_CONTROL_PATH,
            &request.body,
            now_ms()?,
        )?;
        let envelope = NativeControlEnvelope::parse(&request.body)?;
        let disconnected = || disconnected(stream);
        let context = NativeControlContext::admitted(
            Instant::now() + Duration::from_millis(NATIVE_RENEWAL_MAX_MS as u64),
            &disconnected,
            headers,
        );
        context.check()?;
        let result = self.renewal.as_ref().map_or_else(
            || {
                Ok(NativeControlResponse {
                    v: 1,
                    operation_id: envelope.operation_id.clone(),
                    disposition: NativeControlDisposition::Unavailable,
                    publication_revision: None,
                    reason: Some(NativeControlReason::OwnerUnavailable),
                })
            },
            |handler| handler.renew(proof, &envelope, &request.body, &context),
        );
        let response = match result {
            Ok(response) => response,
            Err(error) => {
                // Never copy native/socket/provider text into a response.
                // An unclassified handler failure is unknown, even if the
                // caller disappeared before receiving a durable receipt.
                let conflict = error.status == 409;
                NativeControlResponse {
                    v: 1,
                    operation_id: envelope.operation_id.clone(),
                    disposition: if conflict {
                        NativeControlDisposition::Conflict
                    } else {
                        NativeControlDisposition::Unknown
                    },
                    publication_revision: None,
                    reason: Some(if conflict {
                        NativeControlReason::AuthorityChanged
                    } else {
                        NativeControlReason::OutcomeUnknown
                    }),
                }
            }
        };
        response.validate_for(&envelope)?;
        let body = serde_json::to_vec(&response).map_err(|_| unavailable())?;
        let mac = self.identity.sign_control_response(
            request_header(request, NATIVE_NONCE_HEADER)?,
            &request.body,
            200,
            &body,
        )?;
        stream.write_all(format!("HTTP/1.1 200 Native\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n{NATIVE_RESPONSE_MAC_HEADER}: {mac}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",body.len()).as_bytes()).map_err(|_|unavailable())?;
        stream.write_all(&body).map_err(|_| unavailable())
    }
}

fn request_header<'a>(request: &'a HttpRequest, name: &str) -> Result<&'a str, GatewayError> {
    let values: Vec<_> = request
        .headers
        .iter()
        .filter(|(key, _)| key.eq_ignore_ascii_case(name))
        .collect();
    if values.len() != 1 {
        return Err(forbidden());
    }
    Ok(values[0].1.as_str())
}
fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Native operation is unavailable",
        "native_operation_unavailable",
    )
}
fn invalid() -> GatewayError {
    GatewayError::new(
        400,
        "Invalid native HTTP request",
        Some("native_request_invalid"),
    )
}
fn forbidden() -> GatewayError {
    GatewayError::new(
        401,
        "Native request authentication failed",
        Some("native_auth_invalid"),
    )
}

#[cfg(all(test, target_os = "linux"))]
mod owner_control_tests;

#[cfg(test)]
mod live_http_tests;

#[cfg(test)]
pub(crate) mod import_http_tests;
