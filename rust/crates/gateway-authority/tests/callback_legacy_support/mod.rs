use gateway_auth_primitives::crypto::Secret;
use gateway_authority::native_control::actor::authenticate_client_actor;
use gateway_authority::store::Authority;
use gateway_authority_contracts::TokenSource;
use gateway_authority_keys as keys;
use gateway_authority_test_support::native_actor::{
    Host, Store, config, context, request, run, session_setup,
};
use gateway_core::state::StoredRecord;
use gateway_kv_codec::key::RecordKey;
use gateway_source::error::GatewayError;
use serde_json::{Value, json};
use std::sync::{
    Mutex,
    atomic::{AtomicUsize, Ordering},
};
type TestResult = Result<(), Box<dyn std::error::Error>>;
use gateway_auth_credentials::provider_credentials::{
    CodexNativeOwner, CodexSourceAccount, CodexSourcePool, MANAGED_NATIVE_SOURCE_VERSION,
    ManagedNativeSource, codex_source_pool_key,
};
use gateway_auth_native_contracts::nonce::NativeNonceAuthority;
use gateway_auth_native_contracts::transport::{NativeAuthHeaders, NativeCapability};
use gateway_auth_native_proof::native_auth::{NativeAuthenticator, NativeTransportIdentity};
use gateway_auth_native_wire::{
    control::{NativeControlEnvelope, NativeControlRequest},
    native_read::NATIVE_CONTROL_PATH,
};
use gateway_authority::native_control::NativeRenewalControl;
use gateway_authority::native_control::owners::RegisteredRenewalOwners;
use gateway_authority::native_control::receiver::NativeRenewalAuthority;
use gateway_authority_contracts::NativeRenewalCancellation;
use gateway_authority_native_custody::renewal::validate_native_control_custody;
use gateway_authority_native_records::managed::NativeManagedPublicationDisposition;
use gateway_authority_native_records::{owner::RegisteredRenewalOwner, rows};
use gateway_authority_paid_control_records::{
    PaidControlConfig, PaidControlOwner, PaidSourceCheck,
};
use gateway_authority_routing_records::types::{PoolAccount, SubscriptionPool};
use gateway_authority_routing_selection::{SelectionOperations, types::LiveRouteSelection};
use gateway_contracts::{identity::PoolVersionstamp, model::ModelId};
use gateway_core::source_control::{decode_source_control_record, encode_source_control_record};
use gateway_kv_codec::{
    tuple_key::{ScalarKeyPart, TupleKey},
    value::ValueEncoding,
};
use gateway_native_authority_wire::{context::NativeRenewalContext, renewal::NativeRenewalMode};
use gateway_source::{js_string::JsString16, source_json::SourceJson};
use std::collections::BTreeSet;

pub(super) struct Sequence(pub(super) AtomicUsize);
impl TokenSource for Sequence {
    fn next_token(&self) -> Result<String, GatewayError> {
        Ok(format!(
            "renewal-v2-fixture-{}",
            self.0.fetch_add(1, Ordering::SeqCst)
        ))
    }
}
pub(super) struct Alive;
impl NativeRenewalCancellation for Alive {
    fn cancelled(&self) -> bool {
        false
    }
}
pub(super) fn identity() -> NativeTransportIdentity {
    NativeTransportIdentity::new(
        "renewalfixture".into(),
        "renew-owner".into(),
        "renew-epoch".into(),
        [44; 32],
        BTreeSet::from([
            NativeCapability::OwnerRenewal,
            NativeCapability::AccessPublish,
        ]),
    )
    .unwrap()
}
pub(super) fn runtime_key() -> RecordKey {
    TupleKey::from_scalar_parts(&[
        ScalarKeyPart::String("uos_ai"),
        ScalarKeyPart::String("runtime_config"),
        ScalarKeyPart::String("v2"),
    ])
    .to_record_key()
}
pub(super) fn mapping_key() -> RecordKey {
    TupleKey::from_scalar_parts(&[
        ScalarKeyPart::String("uos_ai"),
        ScalarKeyPart::String("codex_live_calls"),
        ScalarKeyPart::String("v1"),
        ScalarKeyPart::String("renew-call"),
    ])
    .to_record_key()
}
pub(super) fn seed_owned(store: &Store, id: &NativeTransportIdentity) {
    let access = format!(
        "e30.{}.synthetic",
        gateway_auth_primitives::crypto::base64url(br#"{"exp":1030}"#)
    );
    let mut account = CodexSourceAccount {
        account_id: "renew-account".into(),
        access_token: Secret::new(access),
        refresh_token: Secret::new(String::new()),
        updated_at_ms: 1_000_000.0,
        native_owner: None,
    };
    let leaf = account.identity();
    account.native_owner = Some(CodexNativeOwner {
        codex_home: "/tmp/ai-rust-wasm-20261018/synthetic-home".into(),
        generation_hash: leaf.native_generation_hash,
        native_refreshed_at: None,
        external_user_id: None,
        managed_source: Some(ManagedNativeSource {
            v: MANAGED_NATIVE_SOURCE_VERSION,
            generation_hash: "d".repeat(64),
        }),
    });
    let binding = account.identity();
    store.seed(
        codex_source_pool_key(),
        "renew-source",
        CodexSourcePool {
            accounts: vec![account],
            updated_at_ms: 1_000_000.0,
        }
        .private_source_value(),
    );
    store.seed(
        keys::pool(),
        "renew-normalized",
        SubscriptionPool {
            v: 1,
            pool_versionstamp: PoolVersionstamp("renew-source".into()),
            accounts: vec![PoolAccount {
                account_id_hash: binding.routing_account_hash,
                admin_account_hash: binding.administrative_account_hash,
                credential_version: binding.credential_version,
                enabled: true,
            }],
        },
    );
    store.seed(runtime_key(), "runtime-version", json!({"v":2}));
    store.seed(
        keys::paid_control_owner(),
        "owner-version",
        PaidControlOwner {
            v: 1,
            owner_id: id.owner_id().into(),
            owner_epoch: id.owner_epoch().into(),
            key_fingerprint: id.key_fingerprint().into(),
        },
    );
    store.seed(
        keys::paid_control_config(),
        "config-version",
        PaidControlConfig {
            v: 2,
            owner_id: id.owner_id().into(),
            owner_epoch: id.owner_epoch().into(),
            generation: "config-generation".into(),
            configuration_fingerprint: "e".repeat(64),
            surplus_binding: None,
            openlux_discovery_binding: None,
            openlux_binding: None,
            source_checks: vec![PaidSourceCheck {
                key: runtime_key().0,
                expected_revision: Some("runtime-version".into()),
            }],
        },
    );
    store.seed(mapping_key(),"mapping-version",json!({"account_id":"renew-account","principal_id":"passkey-user:user-fixture","created_at_ms":1_000_000}));
}
#[derive(Default)]
pub(super) struct Nonces(Mutex<BTreeSet<String>>);
impl NativeNonceAuthority for Nonces {
    async fn consume_nonce(&self, _: &str, n: &str, _: i64, _: i64) -> Result<(), GatewayError> {
        if self.0.lock().unwrap().insert(n.into()) {
            Ok(())
        } else {
            Err(fail())
        }
    }
}
pub(super) async fn owner_proof(
    identity: &NativeTransportIdentity,
    body: &[u8],
    n: u8,
) -> gateway_auth_native_proof::native_auth::AuthenticatedNativeOwner {
    let nonce = gateway_auth_primitives::crypto::base64url(&[n; 32]);
    let mac = identity
        .sign(NATIVE_CONTROL_PATH, 1_000_000, &nonce, body)
        .unwrap();
    NativeAuthenticator::new(identity, &Nonces::default())
        .authenticate(
            NATIVE_CONTROL_PATH,
            NativeAuthHeaders {
                version: "1",
                key_id: "renewalfixture",
                time_ms: "1000000",
                nonce: &nonce,
                mac: &mac,
            },
            body,
            1_000_000,
        )
        .await
        .unwrap()
}

pub(super) async fn publication_proof(
    identity: &NativeTransportIdentity,
    body: &[u8],
    n: u8,
) -> gateway_auth_native_proof::native_auth::AuthenticatedNativeOwner {
    let path = gateway_auth_native_wire::native_read::NATIVE_PUBLICATION_PATH;
    let nonce = gateway_auth_primitives::crypto::base64url(&[n; 32]);
    let mac = identity.sign(path, 1_000_000, &nonce, body).unwrap();
    NativeAuthenticator::new(identity, &Nonces::default())
        .authenticate(
            path,
            NativeAuthHeaders {
                version: "1",
                key_id: "renewalfixture",
                time_ms: "1000000",
                nonce: &nonce,
                mac: &mac,
            },
            body,
            1_000_000,
        )
        .await
        .unwrap()
}
pub(super) fn body(reference: &str, operation: &str) -> Vec<u8> {
    serde_json::to_vec(&NativeControlEnvelope {
        v: 1,
        operation_id: operation.into(),
        request: NativeControlRequest::CodexRenewAccess {
            action_ref: reference.into(),
        },
    })
    .unwrap()
}
