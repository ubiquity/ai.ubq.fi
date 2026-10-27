//! Closed HTTP import adapter. Actual shared authority mints every proof; no effect implementation.
use crate::{
    native_import::{NativeImportHandler, auth_body_error, dispatch_import},
    owner_control::NativeControlContext,
};
use gateway_auth::native_auth::{NativeAuthHeaders, NativeAuthenticator, NativeTransportIdentity};
use gateway_auth::native_import::{
    NATIVE_IMPORT_ACTION_HEADER, NATIVE_IMPORT_BODY_HEADER, NATIVE_IMPORT_OPERATION_HEADER,
    NativeImportAuthHeaders, NativeImportKind, NativeImportMetadata,
};
use gateway_auth::native_replay::StoreNonceAuthority;
use gateway_auth::types::AuthConfig;
use gateway_auth_native_wire::control::NATIVE_RENEWAL_MAX_MS;
use gateway_auth_native_wire::native_read::{
    NATIVE_KEY_HEADER, NATIVE_MAC_HEADER, NATIVE_NONCE_HEADER, NATIVE_TIME_HEADER,
    NATIVE_VERSION_HEADER,
};
use gateway_authority::native_control::{
    NativeRenewalCancellation,
    receiver::{NativeRenewalAuthority, RegisteredRenewalOwner},
};
use gateway_authority::store::{Authority, TokenSource};
use gateway_core::{error::GatewayError, state::AuthorityStore};
use gateway_native_http_framing::{
    import_head::ImportHead,
    response::{disconnected, now_ms, write_import_response},
};
use gateway_native_replay::nonce_sqlite::SqliteNonceStore;
use std::{
    io::{Read, Write},
    net::TcpStream,
    sync::Arc,
    time::{Duration, Instant},
};

pub(crate) fn auth_headers(head: &ImportHead) -> Result<NativeAuthHeaders<'_>, GatewayError> {
    Ok(NativeAuthHeaders {
        version: one(head, NATIVE_VERSION_HEADER)?,
        key_id: one(head, NATIVE_KEY_HEADER)?,
        time_ms: one(head, NATIVE_TIME_HEADER)?,
        nonce: one(head, NATIVE_NONCE_HEADER)?,
        mac: one(head, NATIVE_MAC_HEADER)?,
    })
}
fn metadata(head: &ImportHead) -> Result<NativeImportMetadata, GatewayError> {
    Ok(NativeImportMetadata {
        v: 1,
        operation_id: one(head, NATIVE_IMPORT_OPERATION_HEADER)?.into(),
        action_ref: one(head, NATIVE_IMPORT_ACTION_HEADER)?.into(),
    })
}
fn optional<'a>(head: &'a ImportHead, name: &str) -> Result<Option<&'a str>, GatewayError> {
    let mut values = head
        .headers
        .iter()
        .filter(|(key, _)| key.eq_ignore_ascii_case(name));
    let first = values.next().map(|(_, value)| value.as_str());
    if values.next().is_some() {
        return Err(invalid());
    }
    Ok(first)
}
fn one<'a>(head: &'a ImportHead, name: &str) -> Result<&'a str, GatewayError> {
    optional(head, name)
        .map_err(|_| unauthorized())?
        .filter(|value| !value.is_empty())
        .ok_or_else(unauthorized)
}

// Crate-private and implemented only by this fixed real-authority adapter, not an auth callback API.
pub(crate) trait ImportReceiver: Send + Sync {
    fn receive(
        &self,
        stream: &mut TcpStream,
        head: ImportHead,
        identity: &NativeTransportIdentity,
        nonces: &SqliteNonceStore,
    ) -> Result<(), GatewayError>;
}
struct Configured<S, I, H> {
    authority: Authority<S, I>,
    owner: RegisteredRenewalOwner,
    config: AuthConfig,
    handler: H,
}
pub(crate) fn configured<S, I, H>(
    authority: Authority<S, I>,
    owner: RegisteredRenewalOwner,
    config: AuthConfig,
    handler: H,
) -> Arc<dyn ImportReceiver>
where
    S: AuthorityStore + Send + Sync + 'static,
    I: TokenSource + Send + Sync + 'static,
    H: NativeImportHandler + 'static,
{
    Arc::new(Configured {
        authority,
        owner,
        config,
        handler,
    })
}
struct OriginalCancellation<'a, 'b>(&'a NativeControlContext<'b>);
impl NativeRenewalCancellation for OriginalCancellation<'_, '_> {
    fn cancelled(&self) -> bool {
        self.0.cancelled()
    }
}
impl<S, I, H> ImportReceiver for Configured<S, I, H>
where
    S: AuthorityStore + Send + Sync,
    I: TokenSource + Send + Sync,
    H: NativeImportHandler,
{
    fn receive(
        &self,
        stream: &mut TcpStream,
        head: ImportHead,
        identity: &NativeTransportIdentity,
        nonces: &SqliteNonceStore,
    ) -> Result<(), GatewayError> {
        let result = Admission {
            authority: &self.authority,
            owner: &self.owner,
            config: &self.config,
            handler: &self.handler,
        }
        .dispatch(stream, head, identity, nonces)?;
        write_import_response(stream, result.status, result.headers, result.body)
    }
}
struct Admission<'a, S, I, H: ?Sized> {
    authority: &'a Authority<S, I>,
    owner: &'a RegisteredRenewalOwner,
    config: &'a AuthConfig,
    handler: &'a H,
}
impl<S, I, H> Admission<'_, S, I, H>
where
    S: AuthorityStore + Send + Sync,
    I: TokenSource + Send + Sync,
    H: NativeImportHandler + ?Sized,
{
    fn dispatch(
        &self,
        stream: &mut TcpStream,
        head: ImportHead,
        identity: &NativeTransportIdentity,
        nonces: &SqliteNonceStore,
    ) -> Result<crate::native_import::NativeImportResult, GatewayError> {
        let auth = auth_headers(&head)?;
        let metadata = metadata(&head)?;
        let watch = stream.try_clone().map_err(|_| unavailable())?;
        let cancelled = || disconnected(&watch);
        let original = NativeControlContext::admitted(
            Instant::now() + Duration::from_millis(NATIVE_RENEWAL_MAX_MS as u64),
            &cancelled,
            auth,
        );
        original.check()?;
        let header_proof = admit_headers(&head, identity, nonces, &metadata, &original)?;
        let cancellation = OriginalCancellation(&original);
        let authority = NativeRenewalAuthority::configured(
            self.authority,
            identity,
            self.owner,
            self.config,
            &cancellation,
        );
        let admitted = futures_executor::block_on(authority.admit_import(header_proof))?;
        original.check()?;
        // The landed foundation cannot prepare PROFILE or bind its query/options.
        // Do not turn a future kind addition into permission to use caller options.
        if head.kind != NativeImportKind::CodexAuth || head.path != head.kind.path() {
            return Err(unavailable());
        }
        let raw = read_body(stream, &head, &original)?;
        original.check()?;
        let proof = futures_executor::block_on(authority.finish_import(admitted, &raw.0))?;
        original.check()?;
        let result = futures_executor::block_on(dispatch_import(
            self.handler,
            &authority,
            &proof,
            &metadata,
            &raw.0,
            &original,
        ))?;
        original.check()?;
        Ok(result)
    }
}
/// A fixed checked accepted-socket operation. It returns DATA/error, never an admitted context or proof.
pub fn receive_import_connection<S, I, H>(
    stream: &mut TcpStream,
    authority: &Authority<S, I>,
    identity: &NativeTransportIdentity,
    owner: &RegisteredRenewalOwner,
    config: &AuthConfig,
    nonces: &SqliteNonceStore,
    handler: &H,
) -> Result<crate::native_import::NativeImportResult, GatewayError>
where
    S: AuthorityStore + Send + Sync,
    I: TokenSource + Send + Sync,
    H: NativeImportHandler + ?Sized,
{
    gateway_native_connector::artifact::require_production_artifact()?;
    if !stream
        .peer_addr()
        .map_err(|_| unavailable())?
        .ip()
        .is_loopback()
    {
        return Err(unauthorized());
    }
    stream
        .set_read_timeout(Some(Duration::from_secs(5)))
        .map_err(|_| unavailable())?;
    stream
        .set_write_timeout(Some(Duration::from_secs(5)))
        .map_err(|_| unavailable())?;
    let head = gateway_native_http_framing::request::read_import_head(stream)?;
    Admission {
        authority,
        owner,
        config,
        handler,
    }
    .dispatch(stream, head, identity, nonces)
}
fn admit_headers(
    head: &ImportHead,
    identity: &NativeTransportIdentity,
    nonces: &SqliteNonceStore,
    metadata: &NativeImportMetadata,
    original: &NativeControlContext<'_>,
) -> Result<gateway_auth::native_import::NativeImportAdmission, GatewayError> {
    original.check()?;
    let replay = StoreNonceAuthority::new(nonces);
    futures_executor::block_on(NativeAuthenticator::new(identity, &replay).admit_import(
        head.kind,
        NativeImportAuthHeaders {
            auth: original.original_auth_headers(),
            body_sha256: one(&head, NATIVE_IMPORT_BODY_HEADER)?,
        },
        metadata,
        now_ms()?,
    ))
}
/// Original accepted request DATA and genuine actual-remote proof. No decoder or
/// second nonce store is introduced for the dedicated actor.
pub(crate) struct RemoteImportDocument {
    pub(crate) proof: gateway_authority::native_control::import::AuthenticatedNativeImport,
    pub(crate) metadata: NativeImportMetadata,
    raw: NativeDocument,
}
impl RemoteImportDocument {
    pub(crate) fn body(&self) -> &[u8] {
        &self.raw.0
    }
}
pub(crate) fn read_remote_import<C: NativeRenewalCancellation>(
    stream: &mut TcpStream,
    head: &ImportHead,
    identity: &NativeTransportIdentity,
    nonces: &SqliteNonceStore,
    authority: &gateway_authority::native_control::import::remote::RemoteImportAuthority<'_, C>,
    original: &NativeControlContext<'_>,
    registered_target: (&str, &str),
) -> Result<RemoteImportDocument, GatewayError> {
    let (native_owner_id, account_id_hash) = registered_target;
    let metadata = metadata(&head)?;
    let header = admit_headers(head, identity, nonces, &metadata, original)?;
    let admitted = authority.admit_import(header)?;
    original.check()?;
    if admitted.native_owner_id() != native_owner_id
        || admitted.account_id_hash() != account_id_hash
        || !matches!(admitted.target(), gateway_auth::native_import::NativeImportTarget::ExistingAccount { account_id_hash: actual } if actual == account_id_hash)
    {
        return Err(unavailable());
    }
    if head.kind != NativeImportKind::CodexAuth || head.path != head.kind.path() {
        return Err(unavailable());
    }
    let raw = read_body(stream, head, original)?;
    original.check()?;
    let proof = authority.finish_import(admitted, &raw.0)?;
    authority.recheck_import(&proof)?;
    original.check()?;
    Ok(RemoteImportDocument {
        proof,
        metadata,
        raw,
    })
}
struct NativeDocument(Vec<u8>);
impl Drop for NativeDocument {
    fn drop(&mut self) {
        self.0.fill(0);
    }
}
fn read_body(
    stream: &mut TcpStream,
    head: &ImportHead,
    original: &NativeControlContext<'_>,
) -> Result<NativeDocument, GatewayError> {
    let length = optional(&head, "content-length")?;
    let encoding = optional(&head, "transfer-encoding")?;
    if length.is_some() && encoding.is_some() {
        return Err(invalid());
    }
    let maximum = head.kind.body_limit_bytes();
    let mut body = NativeDocument(Vec::new());
    if let Some(length) = length {
        gateway_native_import_source::body::check_auth_content_length(Some(length))
            .map_err(|_| auth_body_error())?;
        let length = length.parse::<usize>().map_err(|_| auth_body_error())?;
        append(stream, &mut body.0, length, head.framing_deadline, original)?;
    } else if encoding.is_some_and(|encoding| encoding.eq_ignore_ascii_case("chunked")) {
        loop {
            let mut line = Vec::new();
            while !line.ends_with(b"\r\n") {
                if line.len() >= 1024 {
                    return Err(invalid());
                }
                append(stream, &mut line, 1, head.framing_deadline, original)?;
            }
            let size = match httparse::parse_chunk_size(&line).map_err(|_| invalid())? {
                httparse::Status::Complete((used, size)) if used == line.len() => {
                    usize::try_from(size).map_err(|_| auth_body_error())?
                }
                _ => return Err(invalid()),
            };
            if size > maximum - body.0.len() {
                return Err(auth_body_error());
            }
            append(stream, &mut body.0, size, head.framing_deadline, original)?;
            let mut end = Vec::new();
            append(stream, &mut end, 2, head.framing_deadline, original)?;
            if end != b"\r\n" {
                return Err(invalid());
            }
            if size == 0 {
                break;
            }
        }
    } else {
        return Err(invalid());
    }
    original.check()?;
    // Preserve the closed native framing contract: no buffered trailers or second request.
    stream.set_nonblocking(true).map_err(|_| invalid())?;
    let extra = match stream.peek(&mut [0u8; 1]) {
        Ok(count) => count > 0,
        Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => false,
        Err(_) => true,
    };
    stream.set_nonblocking(false).map_err(|_| invalid())?;
    if extra {
        return Err(invalid());
    }
    Ok(body)
}
fn append(
    stream: &mut TcpStream,
    output: &mut Vec<u8>,
    length: usize,
    deadline: Instant,
    original: &NativeControlContext<'_>,
) -> Result<(), GatewayError> {
    let mut remaining = length;
    let mut chunk = [0u8; 8192];
    while remaining > 0 {
        original.check()?;
        let time = deadline
            .checked_duration_since(Instant::now())
            .filter(|duration| !duration.is_zero())
            .ok_or_else(auth_body_error)?;
        stream
            .set_read_timeout(Some(time))
            .map_err(|_| auth_body_error())?;
        let take = remaining.min(chunk.len());
        let count = match stream.read(&mut chunk[..take]) {
            Ok(count) => count,
            Err(_) => {
                chunk.fill(0);
                return Err(auth_body_error());
            }
        };
        if count == 0 {
            chunk.fill(0);
            original.check()?;
            return Err(auth_body_error());
        }
        output.extend_from_slice(&chunk[..count]);
        chunk[..count].fill(0);
        remaining -= count;
        original.check()?;
    }
    Ok(())
}
fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Native import effect owner is not configured or unavailable",
        "native_import_unavailable",
    )
}
fn invalid() -> GatewayError {
    GatewayError::new(
        400,
        "Invalid native HTTP request",
        Some("native_request_invalid"),
    )
}
fn unauthorized() -> GatewayError {
    GatewayError::new(
        401,
        "Native request authentication failed",
        Some("native_auth_invalid"),
    )
}
