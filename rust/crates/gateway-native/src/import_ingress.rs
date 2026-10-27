//! Closed HTTP import adapter. Actual shared authority mints every proof; no effect implementation.
use crate::{
    native_import::{NativeImportHandler, dispatch_import},
    owner_control::NativeControlContext,
};
use gateway_auth_contracts::types::AuthConfig;
use gateway_auth_native_contracts::native_import::{
    NATIVE_IMPORT_ACTION_HEADER, NATIVE_IMPORT_BODY_HEADER, NATIVE_IMPORT_OPERATION_HEADER,
    NativeImportKind, NativeImportMetadata,
};
use gateway_auth_native_proof::{
    native_auth::{NativeAuthHeaders, NativeAuthenticator, NativeTransportIdentity},
    native_import::NativeImportAuthHeaders,
};
use gateway_auth_native_protocol::native_replay::StoreNonceAuthority;
use gateway_auth_native_wire::{
    control::NATIVE_RENEWAL_MAX_MS,
    native_read::{
        NATIVE_KEY_HEADER, NATIVE_MAC_HEADER, NATIVE_NONCE_HEADER, NATIVE_TIME_HEADER,
        NATIVE_VERSION_HEADER,
    },
};
use gateway_authority::native_control::{
    NativeRenewalCancellation,
    receiver::{NativeRenewalAuthority, RegisteredRenewalOwner},
};
use gateway_authority::store::{Authority, TokenSource};
use gateway_core::state::AuthorityStore;
use gateway_native_http_framing::{
    import_body::{RawImportDocument, read_import_body},
    import_head::ImportHead,
    response::{disconnected, now_ms, write_import_response},
};
use gateway_native_replay::nonce_sqlite::SqliteNonceStore;
use gateway_source::error::GatewayError;
use std::{
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
    ) -> Result<gateway_native_import_source::result::NativeImportResult, GatewayError> {
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
        let raw = read_import_body(stream, &head, &cancellation)?;
        original.check()?;
        let proof = futures_executor::block_on(authority.finish_import(admitted, raw.as_bytes()))?;
        original.check()?;
        let result = futures_executor::block_on(dispatch_import(
            self.handler,
            &authority,
            &proof,
            &metadata,
            raw.as_bytes(),
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
) -> Result<gateway_native_import_source::result::NativeImportResult, GatewayError>
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
) -> Result<gateway_auth_native_proof::native_import::NativeImportAdmission, GatewayError> {
    original.check()?;
    let replay = StoreNonceAuthority::new(nonces);
    futures_executor::block_on(NativeAuthenticator::new(identity, &replay).admit_import(
        head.kind,
        NativeImportAuthHeaders {
            auth: original.original_auth_headers(),
            body_sha256: one(head, NATIVE_IMPORT_BODY_HEADER)?,
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
    raw: RawImportDocument,
}
impl RemoteImportDocument {
    pub(crate) fn body(&self) -> &[u8] {
        self.raw.as_bytes()
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
    let metadata = metadata(head)?;
    let header = admit_headers(head, identity, nonces, &metadata, original)?;
    let admitted = authority.admit_import(header)?;
    original.check()?;
    if admitted.native_owner_id() != native_owner_id
        || admitted.account_id_hash() != account_id_hash
        || !matches!(admitted.target(), gateway_auth_native_contracts::native_import::NativeImportTarget::ExistingAccount { account_id_hash: actual } if actual == account_id_hash)
    {
        return Err(unavailable());
    }
    if head.kind != NativeImportKind::CodexAuth || head.path != head.kind.path() {
        return Err(unavailable());
    }
    let raw = read_import_body(stream, head, &OriginalCancellation(original))?;
    original.check()?;
    let proof = authority.finish_import(admitted, raw.as_bytes())?;
    authority.recheck_import(&proof)?;
    original.check()?;
    Ok(RemoteImportDocument {
        proof,
        metadata,
        raw,
    })
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
