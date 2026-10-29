#![cfg(unix)]
//! Genuine MAC/nonce admissions only; fixture DATA never constructs a context or proof.
use gateway_auth_native_contracts::{
    native_import::{NativeImportKind, NativeImportMetadata},
    nonce::NativeNonceAuthority,
    transport::{NativeAuthHeaders, NativeCapability, NativeImportAuthHeaders},
};
use gateway_auth_native_proof::{
    native_auth::{NativeAuthenticator, NativeTransportIdentity},
    native_context::{NativeRequestLifetime, now_ms},
};
use gateway_auth_native_wire::{
    control::NATIVE_RENEWAL_MAX_MS,
    native_read::{NATIVE_CONTROL_PATH, NATIVE_RPC_PATH},
};
use gateway_auth_primitives::crypto::{base64url, sha256_hex};
use gateway_source::error::GatewayError;
use std::{
    collections::BTreeSet,
    future::Future,
    sync::{
        Mutex,
        atomic::{AtomicUsize, Ordering},
    },
    task::{Context, Poll, Waker},
    time::{Duration, Instant},
};
const CONTROL: &[u8] = br#"{"v":1,"operation_id":"fixture-operation","request":{"operation":"codex_renew_access","action_ref":"fixture-action"}}"#;
const BODY: &[u8] = b"original native document";
#[derive(Default)]
struct Nonces {
    seen: Mutex<BTreeSet<(String, String)>>,
    calls: AtomicUsize,
}
impl NativeNonceAuthority for Nonces {
    async fn consume_nonce(
        &self,
        key: &str,
        nonce: &str,
        _: i64,
        _: i64,
    ) -> Result<(), GatewayError> {
        self.calls.fetch_add(1, Ordering::SeqCst);
        if !self
            .seen
            .lock()
            .map_err(|_| setup())?
            .insert((key.into(), nonce.into()))
        {
            return Err(GatewayError::new(
                401,
                "Synthetic replay refused",
                Some("fixture_replay"),
            ));
        }
        Ok(())
    }
}
fn setup() -> GatewayError {
    GatewayError::unavailable(
        "Native context fixture setup failed",
        "fixture_setup_failed",
    )
}
fn run<T>(future: impl Future<Output = Result<T, GatewayError>>) -> Result<T, GatewayError> {
    match std::pin::pin!(future).poll(&mut Context::from_waker(Waker::noop())) {
        Poll::Ready(result) => result,
        Poll::Pending => Err(setup()),
    }
}
fn refused<T>(result: Result<T, GatewayError>) -> GatewayError {
    match result {
        Err(error) => error,
        Ok(_) => panic!("the real native API unexpectedly admitted a refused request"),
    }
}
fn transport_identity(
    capability: NativeCapability,
) -> Result<NativeTransportIdentity, GatewayError> {
    NativeTransportIdentity::new(
        "key".into(),
        "owner".into(),
        "epoch".into(),
        [73; 32],
        BTreeSet::from([capability]),
    )
}
struct HeaderData {
    time: String,
    nonce: String,
    mac: String,
}
impl HeaderData {
    fn owner(
        identity: &NativeTransportIdentity,
        byte: u8,
        path: &str,
        body: &[u8],
        time: i64,
    ) -> Result<Self, GatewayError> {
        let nonce = base64url(&[byte; 32]);
        Ok(Self {
            time: time.to_string(),
            mac: identity.sign(path, time, &nonce, body)?,
            nonce,
        })
    }
    fn import(
        identity: &NativeTransportIdentity,
        byte: u8,
        metadata: &NativeImportMetadata,
        digest: &str,
    ) -> Result<Self, GatewayError> {
        let time = now_ms()?;
        let nonce = base64url(&[byte; 32]);
        Ok(Self {
            time: time.to_string(),
            mac: identity.sign_import(
                NativeImportKind::CodexAuth,
                metadata,
                digest,
                time,
                &nonce,
            )?,
            nonce,
        })
    }
    fn borrowed(&self) -> NativeAuthHeaders<'_> {
        NativeAuthHeaders {
            version: "1",
            key_id: "key",
            time_ms: &self.time,
            nonce: &self.nonce,
            mac: &self.mac,
        }
    }
}
fn connected() -> bool {
    false
}
#[test]
fn actual_control_admission_keeps_clone_identity_and_rejects_an_equal_body_from_another_admission()
-> Result<(), GatewayError> {
    let identity = transport_identity(NativeCapability::OwnerRenewal)?;
    let replay = Nonces::default();
    let first = HeaderData::owner(&identity, 1, NATIVE_CONTROL_PATH, CONTROL, now_ms()?)?;
    let second = HeaderData::owner(&identity, 2, NATIVE_CONTROL_PATH, CONTROL, now_ms()?)?;
    let auth = NativeAuthenticator::new(&identity, &replay);
    let (proof, envelope, context) =
        run(auth.authenticate_control_request(first.borrowed(), CONTROL, &connected))?;
    let (other, _, other_context) =
        run(auth.authenticate_control_request(second.borrowed(), CONTROL, &connected))?;
    assert_eq!(envelope.operation_id, "fixture-operation");
    assert_eq!(proof.request_digest(), other.request_digest());
    context.require_owner_context(&proof)?;
    context.require_owner_context(&proof.clone())?;
    other_context.require_owner_context(&other)?;
    let error = refused(context.require_owner_context(&other));
    assert_eq!(error.status, 403);
    assert_eq!(
        error.envelope.error.code.as_deref(),
        Some("native_capability_forbidden")
    );
    assert_eq!(replay.calls.load(Ordering::SeqCst), 2);
    Ok(())
}
#[test]
fn actual_import_body_identity_refuses_cross_admission_even_with_equal_body_and_metadata()
-> Result<(), GatewayError> {
    let identity = transport_identity(NativeCapability::CodexAuthImport)?;
    let replay = Nonces::default();
    let metadata = NativeImportMetadata {
        v: 1,
        operation_id: "fixture-operation".into(),
        action_ref: "fixture-action".into(),
    };
    let digest = sha256_hex(BODY);
    let first = HeaderData::import(&identity, 3, &metadata, &digest)?;
    let second = HeaderData::import(&identity, 4, &metadata, &digest)?;
    let auth = NativeAuthenticator::new(&identity, &replay);
    let started = Instant::now();
    let lifetime = NativeRequestLifetime::until(started + Duration::from_secs(3600), &connected);
    let bounded = Instant::now();
    let (first, context) = run(auth.admit_import_with_context(
        NativeImportKind::CodexAuth,
        Ok(NativeImportAuthHeaders {
            auth: first.borrowed(),
            body_sha256: &digest,
        }),
        &metadata,
        lifetime,
    ))?;
    let (second, other_context) = run(auth.admit_import_with_context(
        NativeImportKind::CodexAuth,
        Ok(NativeImportAuthHeaders {
            auth: second.borrowed(),
            body_sha256: &digest,
        }),
        &metadata,
        NativeRequestLifetime::until(started + Duration::from_secs(20), &connected),
    ))?;
    assert_eq!(first.metadata(), second.metadata());
    assert!(context.deadline() <= bounded + Duration::from_millis(NATIVE_RENEWAL_MAX_MS as u64));
    assert!(other_context.deadline() <= started + Duration::from_secs(20));
    let verified = first.verify_body(BODY, context.now_ms()?)?;
    verified.require_context(&context)?;
    let error = refused(verified.require_context(&other_context));
    assert_eq!(error.status, 403);
    assert_eq!(
        error.envelope.error.code.as_deref(),
        Some("native_capability_forbidden")
    );
    let other = second.verify_body(BODY, other_context.now_ms()?)?;
    other.require_context(&other_context)?;
    assert_eq!(replay.calls.load(Ordering::SeqCst), 2);
    Ok(())
}
#[test]
fn real_control_path_body_and_time_fail_before_nonce_but_capability_and_parse_keep_original_nonce_order()
-> Result<(), GatewayError> {
    let identity = transport_identity(NativeCapability::OwnerRenewal)?;
    let replay = Nonces::default();
    let auth = NativeAuthenticator::new(&identity, &replay);
    for (byte, path, signed_body, time) in [
        (5, NATIVE_RPC_PATH, CONTROL, now_ms()?),
        (6, NATIVE_CONTROL_PATH, b"different".as_slice(), now_ms()?),
        (7, NATIVE_CONTROL_PATH, CONTROL, now_ms()? - 40_000),
    ] {
        let headers = HeaderData::owner(&identity, byte, path, signed_body, time)?;
        let error = refused(run(auth.authenticate_control_request(
            headers.borrowed(),
            CONTROL,
            &connected,
        )));
        assert_eq!(error.status, 401);
    }
    assert_eq!(replay.calls.load(Ordering::SeqCst), 0);
    let malformed = b"not an envelope";
    let headers = HeaderData::owner(&identity, 8, NATIVE_CONTROL_PATH, malformed, now_ms()?)?;
    assert_eq!(
        refused(run(auth.authenticate_control_request(
            headers.borrowed(),
            malformed,
            &connected
        )))
        .status,
        400
    );
    assert_eq!(replay.calls.load(Ordering::SeqCst), 1);
    let read_only = transport_identity(NativeCapability::SupervisorRead)?;
    let headers = HeaderData::owner(&read_only, 9, NATIVE_CONTROL_PATH, CONTROL, now_ms()?)?;
    assert_eq!(
        refused(run(NativeAuthenticator::new(&read_only, &replay)
            .authenticate_control_request(
                headers.borrowed(),
                CONTROL,
                &connected
            )))
        .status,
        403
    );
    assert_eq!(replay.calls.load(Ordering::SeqCst), 2);
    Ok(())
}
#[test]
fn original_import_cancellation_precedes_deferred_header_failure_without_consuming_a_nonce()
-> Result<(), GatewayError> {
    let identity = transport_identity(NativeCapability::CodexAuthImport)?;
    let replay = Nonces::default();
    let metadata = NativeImportMetadata {
        v: 1,
        operation_id: "fixture-operation".into(),
        action_ref: "fixture-action".into(),
    };
    let cancelled = || true;
    let result = run(
        NativeAuthenticator::new(&identity, &replay).admit_import_with_context(
            NativeImportKind::CodexAuth,
            Err(GatewayError::new(
                401,
                "Malformed digest header",
                Some("native_auth_invalid"),
            )),
            &metadata,
            NativeRequestLifetime::until(Instant::now() + Duration::from_secs(20), &cancelled),
        ),
    );
    let error = refused(result);
    assert_eq!(error.status, 503);
    assert_eq!(
        error.envelope.error.code.as_deref(),
        Some("native_control_outcome_unknown")
    );
    assert_eq!(replay.calls.load(Ordering::SeqCst), 0);
    let current = run(
        NativeAuthenticator::new(&identity, &replay).admit_import_with_context(
            NativeImportKind::CodexAuth,
            Err(GatewayError::new(
                401,
                "Malformed digest header",
                Some("native_auth_invalid"),
            )),
            &metadata,
            NativeRequestLifetime::until(Instant::now() + Duration::from_secs(20), &connected),
        ),
    );
    let current_error = refused(current);
    assert_eq!(current_error.status, 401);
    assert_eq!(
        current_error.envelope.error.code.as_deref(),
        Some("native_auth_invalid")
    );
    assert_eq!(replay.calls.load(Ordering::SeqCst), 0);
    Ok(())
}
