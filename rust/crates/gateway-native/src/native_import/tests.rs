//! Private unreachable-wire binding/order invariants. Reachable behavior uses real public TCP targets.
use super::{
    AuthenticatedNativeImport, NativeControlContext, NativeImportContext, decode_context,
    dispatch_import,
};
use gateway_auth_native_contracts::native_import::NativeImportKind;
use gateway_auth_native_contracts::transport::{NativeAuthHeaders, NativeImportAuthHeaders};
use gateway_auth_native_proof::native_auth::NativeAuthenticator;
use gateway_auth_native_protocol::native_replay::StoreNonceAuthority;
use gateway_auth_native_wire::native_read::{
    NATIVE_KEY_HEADER, NATIVE_MAC_HEADER, NATIVE_NONCE_HEADER, NATIVE_TIME_HEADER,
    NATIVE_VERSION_HEADER,
};
use gateway_auth_primitives::crypto::{base64url, sha256_hex};
use gateway_authority::{native_control::receiver::NativeRenewalAuthority, store::Authority};
use gateway_native_import_test_support::{
    fixture::{Fixture, Store, Tokens, fail, now_ms, run},
    socket::SocketRequest,
};
use gateway_source::error::GatewayError;
use std::{
    collections::BTreeMap,
    time::{Duration, Instant},
};
const AUTH: &[u8] = br#"{"tokens":{"access_token":"SYNTHETIC_NATIVE_ACCESS","refresh_token":"SYNTHETIC_NATIVE_REFRESH","account_id":"synthetic-account"}}"#;
fn headers(h: &BTreeMap<String, String>) -> NativeAuthHeaders<'_> {
    NativeAuthHeaders {
        version: &h[NATIVE_VERSION_HEADER],
        key_id: &h[NATIVE_KEY_HEADER],
        time_ms: &h[NATIVE_TIME_HEADER],
        nonce: &h[NATIVE_NONCE_HEADER],
        mac: &h[NATIVE_MAC_HEADER],
    }
}
fn proof(
    f: &Fixture,
    a: &Authority<Store, Tokens>,
    socket: &SocketRequest,
    body: &[u8],
    n: u8,
) -> Result<(AuthenticatedNativeImport, BTreeMap<String, String>), GatewayError> {
    let mut h = socket.headers.clone();
    let digest = sha256_hex(body);
    let at = now_ms()?;
    let nonce = base64url(&[n; 32]);
    h.insert(NATIVE_NONCE_HEADER.into(), nonce.clone());
    h.insert(NATIVE_TIME_HEADER.into(), at.to_string());
    h.insert(
        NATIVE_MAC_HEADER.into(),
        f.identity.sign_import(
            NativeImportKind::CodexAuth,
            &socket.metadata,
            &digest,
            at,
            &nonce,
        )?,
    );
    let replay = StoreNonceAuthority::new(&socket.nonces);
    let admission = run(NativeAuthenticator::new(&f.identity, &replay).admit_import(
        NativeImportKind::CodexAuth,
        NativeImportAuthHeaders {
            auth: headers(&h),
            body_sha256: &digest,
        },
        &socket.metadata,
        now_ms()?,
    ))?;
    let control =
        NativeRenewalAuthority::configured(a, &f.identity, &f.owner, &f.config, &f.cancel);
    let admitted = run(control.admit_import(admission))?;
    Ok((run(control.finish_import(admitted, body))?, h))
}
#[test]
fn a_different_actual_proof_cannot_observe_the_retained_auth_context() -> Result<(), GatewayError> {
    let f = Fixture::new()?;
    let a = f.authority();
    let socket = SocketRequest::prepared(&f, &a, AUTH)?;
    let (first, h) = proof(&f, &a, &socket, AUTH, 13)?;
    let (other, _) = proof(&f, &a, &socket, AUTH, 14)?;
    assert_eq!(first.metadata(), other.metadata());
    let connected = || false;
    let original = NativeControlContext::admitted(
        Instant::now() + Duration::from_secs(20),
        &connected,
        headers(&h),
    );
    let context = decode_context(&first, first.metadata(), AUTH, &original)?;
    context.require_original(&original)?;
    context.check_document(&first, first.metadata(), AUTH)?;
    assert!(
        context
            .check_document(&other, other.metadata(), AUTH)
            .is_err()
    );
    let effects = std::sync::atomic::AtomicUsize::new(0);
    context.check_document(&first, first.metadata(), AUTH)?;
    effects.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    assert_eq!(effects.load(std::sync::atomic::Ordering::SeqCst), 1);
    Ok(())
}
#[test]
fn a_different_genuine_original_context_cannot_extend_or_rebind_the_decoded_context()
-> Result<(), GatewayError> {
    let f = Fixture::new()?;
    let a = f.authority();
    let socket = SocketRequest::prepared(&f, &a, AUTH)?;
    let (p, h) = proof(&f, &a, &socket, AUTH, 13)?;
    let connected = || false;
    let original = NativeControlContext::admitted(
        Instant::now() + Duration::from_secs(20),
        &connected,
        headers(&h),
    );
    let other = NativeControlContext::admitted(
        Instant::now() + Duration::from_secs(21),
        &connected,
        headers(&h),
    );
    let context = decode_context(&p, p.metadata(), AUTH, &original)?;
    context.check_document(&p, p.metadata(), AUTH)?;
    context.require_original(&original)?;
    assert_eq!(
        context
            .require_original(&other)
            .err()
            .ok_or_else(fail)?
            .status,
        403
    );
    Ok(())
}
#[test]
fn original_cancellation_metadata_and_raw_binding_precede_any_decode_error()
-> Result<(), GatewayError> {
    let f = Fixture::new()?;
    let a = f.authority();
    let body = b"{";
    let socket = SocketRequest::prepared(&f, &a, body)?;
    let (p, h) = proof(&f, &a, &socket, body, 13)?;
    let control =
        NativeRenewalAuthority::configured(&a, &f.identity, &f.owner, &f.config, &f.cancel);
    struct Refuse;
    impl super::NativeImportHandler for Refuse {
        fn import_document(
            &self,
            _: &AuthenticatedNativeImport,
            _: &gateway_auth_native_contracts::native_import::NativeImportMetadata,
            _: &[u8],
            _: &NativeImportContext<'_>,
            _: &NativeControlContext<'_>,
        ) -> Result<gateway_native_import_source::result::NativeImportResult, GatewayError>
        {
            panic!("binding/cancellation must precede decoder/handler");
        }
    }
    let disconnected = || true;
    let cancelled = NativeControlContext::admitted(
        Instant::now() + Duration::from_secs(20),
        &disconnected,
        headers(&h),
    );
    let error = run(dispatch_import(
        &Refuse,
        &control,
        &p,
        p.metadata(),
        body,
        &cancelled,
    ))
    .err()
    .ok_or_else(fail)?;
    assert_eq!(
        error.envelope.error.code.as_deref(),
        Some("native_control_outcome_unknown")
    );
    let connected = || false;
    let original = NativeControlContext::admitted(
        Instant::now() + Duration::from_secs(20),
        &connected,
        headers(&h),
    );
    let mut wrong = p.metadata().clone();
    wrong.action_ref = "a".repeat(64);
    assert_eq!(
        run(dispatch_import(
            &Refuse, &control, &p, &wrong, body, &original
        ))
        .err()
        .ok_or_else(fail)?
        .status,
        403
    );
    assert!(
        run(dispatch_import(
            &Refuse,
            &control,
            &p,
            p.metadata(),
            b"OTHER",
            &original
        ))
        .is_err()
    );
    Ok(())
}
