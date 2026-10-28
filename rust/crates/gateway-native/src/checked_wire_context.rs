//! Private wire-test setup through actual MAC/nonce admission; no context constructor.
use gateway_auth_native_contracts::{
    native_import::{NativeImportKind, NativeImportMetadata},
    transport::{NativeAuthHeaders, NativeCapability, NativeImportAuthHeaders},
};
use gateway_auth_native_proof::{
    native_auth::{NativeAuthenticator, NativeTransportIdentity},
    native_context::{NativeControlContext, NativeRequestLifetime, now_ms},
};
use gateway_auth_native_protocol::native_replay::StoreNonceAuthority;
use gateway_auth_primitives::crypto::{base64url, sha256_hex};
use gateway_native_import_test_support::fixture::Store;
use gateway_source::error::GatewayError;
use std::{collections::BTreeSet, time::Instant};

pub(crate) fn wire_context<'a>(
    deadline: Instant,
    disconnected: &'a dyn Fn() -> bool,
) -> Result<NativeControlContext<'a>, GatewayError> {
    let identity = NativeTransportIdentity::new(
        "context-fixture".into(),
        "fixture-owner".into(),
        "fixture-epoch".into(),
        [61; 32],
        BTreeSet::from([NativeCapability::CodexAuthImport]),
    )?;
    let metadata = NativeImportMetadata {
        v: 1,
        operation_id: "context-operation".into(),
        action_ref: "context-action".into(),
    };
    let time = now_ms()?;
    let nonce = base64url(&[61; 32]);
    let digest = sha256_hex(b"context-only");
    let mac = identity.sign_import(
        NativeImportKind::CodexAuth,
        &metadata,
        &digest,
        time,
        &nonce,
    )?;
    let time = time.to_string();
    let store = Store::default();
    let replay = StoreNonceAuthority::new(&store);
    let (_, context) = futures_executor::block_on(
        NativeAuthenticator::new(&identity, &replay).admit_import_with_context(
            NativeImportKind::CodexAuth,
            Ok(NativeImportAuthHeaders {
                auth: NativeAuthHeaders {
                    version: "1",
                    key_id: "context-fixture",
                    time_ms: &time,
                    nonce: &nonce,
                    mac: &mac,
                },
                body_sha256: &digest,
            }),
            &metadata,
            NativeRequestLifetime::until(deadline, disconnected),
        ),
    )?;
    Ok(context)
}
