use super::support::{Host, NOW, TestResult};
use gateway_auth_native_contracts::transport::NativeAuthHeaders;
use gateway_auth_native_proof::native_auth::{
    AuthenticatedNativeOwner, NativeAuthenticator, NativeTransportIdentity,
};
use gateway_auth_native_protocol::native_replay::StoreNonceAuthority;
use gateway_auth_native_wire::native_read::{NATIVE_CONTROL_PATH, NATIVE_RENEWAL_AUTHORITY_PATH};
use gateway_auth_primitives::crypto::base64url;
use gateway_authority_contracts::{AuthorityClock, NativeRenewalCancellation, TokenSource};
use gateway_authority_native_lifecycle::SourceNativeLifecycleService;
use gateway_core::state::AuthorityStore;
use gateway_native_authority_request::{
    NativeRenewalAuthorityRequest as Request, OriginalControlHeaders,
};
use gateway_native_authority_wire::source_response::SourceNativeRenewalAuthorityResponseV2 as Response;
use gateway_source::error::GatewayError;

pub(super) fn claim(
    id: &NativeTransportIdentity,
    reference: &str,
    operation: &str,
    n: u8,
) -> Result<Request, GatewayError> {
    let original = super::support::control_body(reference, operation)?;
    let nonce = base64url(&[n; 32]);
    Ok(Request::Claim {
        v: 1,
        original_control_body_base64url: base64url(&original),
        original_auth_headers: OriginalControlHeaders {
            version: "1".into(),
            key_id: id.key_id().into(),
            time_ms: NOW.to_string(),
            nonce: nonce.clone(),
            mac: id.sign(NATIVE_CONTROL_PATH, NOW, &nonce, &original)?,
        },
    })
}
pub(super) async fn peer<S: AuthorityStore>(
    identity: &NativeTransportIdentity,
    replay: &StoreNonceAuthority<'_, S>,
    path: &str,
    bytes: &[u8],
    now: i64,
    n: u8,
) -> Result<AuthenticatedNativeOwner, GatewayError> {
    let nonce = base64url(&[n; 32]);
    let mac = identity.sign(path, now, &nonce, bytes)?;
    NativeAuthenticator::new(identity, replay)
        .authenticate(
            path,
            NativeAuthHeaders {
                version: "1",
                key_id: identity.key_id(),
                time_ms: &now.to_string(),
                nonce: &nonce,
                mac: &mac,
            },
            bytes,
            now,
        )
        .await
}
pub(super) async fn execute<S: AuthorityStore, I: TokenSource, C: NativeRenewalCancellation>(
    service: &SourceNativeLifecycleService<'_, S, I, Host, C>,
    store: &S,
    host: &Host,
    identity: &NativeTransportIdentity,
    request: &Request,
    n: u8,
) -> Result<Response, GatewayError> {
    let bytes = serde_json::to_vec(request).map_err(|_| super::support::unavailable())?;
    let replay = StoreNonceAuthority::new(store);
    let proof = peer(
        identity,
        &replay,
        NATIVE_RENEWAL_AUTHORITY_PATH,
        &bytes,
        AuthorityClock::now_ms(&host.now)?,
        n,
    )
    .await?;
    gateway_authority_native_callback::execute_source(
        service, &proof, identity, &replay, &host.now, &bytes,
    )
    .await
}
pub(super) fn decoded_claim(response: Response) -> TestResult<(String, String)> {
    match response {
        Response::Claim {
            v: 2,
            claim_ref,
            start_permit: Some(permit),
            ..
        } => Ok((claim_ref, permit)),
        _ => Err("genuine source claim with permit required".into()),
    }
}
