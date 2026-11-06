//! Private synthetic producer for the actual Worker receiver conformance. This
//! target uses public constructors; it never serializes an invented ActionRow.
#![cfg(all(unix, not(target_arch = "wasm32")))]

use gateway_auth::crypto::{Secret, base64url};
use gateway_auth::native::NATIVE_CONTROL_PATH;
use gateway_auth::native_auth::{
    NativeAuthHeaders, NativeAuthenticator, NativeCapability, NativeTransportIdentity,
};
use gateway_auth::native_control::{NativeControlEnvelope, NativeControlRequest};
use gateway_auth::native_replay::StoreNonceAuthority;
use gateway_auth::passkeys::{self, PasskeySession, PasskeyUser};
use gateway_auth::provider_credentials::{
    CodexNativeOwner, CodexSourceAccount, CodexSourcePool, MANAGED_NATIVE_SOURCE_VERSION,
    ManagedNativeSource, codex_credential_identity, codex_source_pool_key,
};
use gateway_auth::session_index;
use gateway_auth::types::{AuthConfig, AuthHost, AuthRequest};
use gateway_auth::upstream::{AuthUpstreamRequest, AuthUpstreamResponse};
use gateway_authority::keys;
use gateway_authority::native_control::actor::authenticate_client_actor;
use gateway_authority::native_control::receiver::{NativeRenewalAuthority, RegisteredRenewalOwner};
use gateway_authority::native_control::{NativeRenewalCancellation, NativeRenewalControl};
use gateway_authority::paid_control::{PaidControlConfig, PaidControlOwner, PaidSourceCheck};
use gateway_authority::routing_types::{PoolAccount, SubscriptionPool};
use gateway_authority::store::{Authority, AuthorityClock, TokenSource};
use gateway_core::error::GatewayError;
use gateway_core::identity::{PoolVersionstamp, PrincipalId, RequestContext, RequestId};
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, RecordMutation, StoredRecord,
};
use gateway_core::tuple_key::{ScalarKeyPart, TupleKey};
use gateway_routing_runtime_test_support::exec::run;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::collections::{BTreeMap, BTreeSet};
use std::io::{Read, Write};
use std::sync::{
    Arc, Mutex,
    atomic::{AtomicU64, Ordering},
};

type Outcome<T> = Result<T, Box<dyn std::error::Error>>;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Transport {
    key_id: String,
    owner_id: String,
    owner_epoch: String,
    key_base64url: String,
    capabilities: Vec<String>,
    codex_home: String,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Input {
    v: u8,
    now_ms: i64,
    artifact_sha: String,
    artifact_tree: String,
    transport: Transport,
    auth_values: BTreeMap<String, String>,
}
#[derive(Clone)]
struct Store {
    rows: Arc<Mutex<BTreeMap<String, StoredRecord>>>,
    sequence: Arc<AtomicU64>,
    now: i64,
}
impl Store {
    fn seed(&self, key: RecordKey, revision: &str, value: impl Serialize) -> Outcome<()> {
        self.rows.lock().map_err(|e| e.to_string())?.insert(
            key.0,
            StoredRecord::json(revision, serde_json::to_value(value)?, None),
        );
        Ok(())
    }
}
impl AuthorityClock for Store {
    fn now_ms(&self) -> Result<i64, GatewayError> {
        Ok(self.now)
    }
}
impl TokenSource for Store {
    fn next_token(&self) -> Result<String, GatewayError> {
        Ok(format!(
            "freeze-constructor-{}-{}",
            self.now,
            self.sequence.fetch_add(1, Ordering::SeqCst)
        ))
    }
}
impl AuthorityStore for Store {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        Ok(self
            .rows
            .lock()
            .map_err(|_| unavailable())?
            .get(&key.0)
            .filter(|row| row.expires_at_ms.is_none_or(|expiry| expiry > self.now))
            .cloned())
    }
    async fn commit(&self, transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        let mut rows = self.rows.lock().map_err(|_| unavailable())?;
        for check in transition.checks {
            if rows.get(&check.key.0).map(|row| &row.revision) != check.expected_revision.as_ref() {
                return Ok(CommitOutcome::Conflict);
            }
        }
        for mutation in transition.mutations {
            match mutation {
                RecordMutation::Put { key, record } => {
                    rows.insert(key.0, record);
                }
                RecordMutation::Delete { key } => {
                    rows.remove(&key.0);
                }
            }
        }
        Ok(CommitOutcome::Committed)
    }
}
fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Synthetic fixture refused upstream or invalid state",
        "freeze_fixture_refused",
    )
}
struct Host(i64);
impl gateway_auth::webauthn::WebAuthnCrypto for Host {
    async fn validate_key(
        &self,
        _: &gateway_auth::webauthn::CosePublicKey,
    ) -> Result<(), GatewayError> {
        Err(unavailable())
    }
    async fn verify_signature(
        &self,
        _: gateway_auth::webauthn::SignatureInput,
    ) -> Result<bool, GatewayError> {
        Err(unavailable())
    }
}
impl AuthHost for Host {
    fn now_ms(&self) -> i64 {
        self.0
    }
    fn random_bytes(&self, length: usize) -> Result<Vec<u8>, GatewayError> {
        Ok(vec![9; length])
    }
    async fn fetch_auth(
        &self,
        _: AuthUpstreamRequest,
    ) -> Result<AuthUpstreamResponse, GatewayError> {
        Err(unavailable())
    }
    async fn kernel_principal(
        &self,
        _: &str,
        _: &str,
        _: &str,
        _: &str,
    ) -> Result<String, GatewayError> {
        Err(unavailable())
    }
}
struct Alive;
impl NativeRenewalCancellation for Alive {
    fn cancelled(&self) -> bool {
        false
    }
}
fn key(parts: &[&str]) -> RecordKey {
    TupleKey::from_scalar_parts(
        &parts
            .iter()
            .map(|part| ScalarKeyPart::String(part))
            .collect::<Vec<_>>(),
    )
    .to_record_key()
}

fn seed(store: &Store, identity: &NativeTransportIdentity, home: &str) -> Outcome<String> {
    let mut account = CodexSourceAccount {
        account_id: "renew-account".into(),
        access_token: Secret::new("OPAQUE_SYNTHETIC_ACCESS".into()),
        refresh_token: Secret::new(String::new()),
        updated_at_ms: 0.0,
        native_owner: None,
    };
    let old = account.identity();
    account.native_owner = Some(CodexNativeOwner {
        codex_home: home.into(),
        generation_hash: old.native_generation_hash,
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
        "opaque-source",
        CodexSourcePool {
            accounts: vec![account],
            updated_at_ms: 0.0,
        }
        .private_source_value(),
    )?;
    store.seed(
        keys::pool(),
        "opaque-normalized",
        SubscriptionPool {
            v: 1,
            pool_versionstamp: PoolVersionstamp("opaque-source".into()),
            accounts: vec![PoolAccount {
                account_id_hash: binding.routing_account_hash,
                admin_account_hash: binding.administrative_account_hash,
                credential_version: binding.credential_version,
                enabled: true,
            }],
        },
    )?;
    let runtime = key(&["uos_ai", "runtime_config", "v2"]);
    store.seed(runtime.clone(), "runtime-version", json!({"v":2}))?;
    store.seed(
        keys::paid_control_owner(),
        "owner-version",
        PaidControlOwner {
            v: 1,
            owner_id: identity.owner_id().into(),
            owner_epoch: identity.owner_epoch().into(),
            key_fingerprint: identity.key_fingerprint().into(),
        },
    )?;
    store.seed(
        keys::paid_control_config(),
        "config-version",
        PaidControlConfig {
            v: 2,
            owner_id: identity.owner_id().into(),
            owner_epoch: identity.owner_epoch().into(),
            generation: "config-generation".into(),
            configuration_fingerprint: "e".repeat(64),
            surplus_binding: None,
            openlux_discovery_binding: None,
            openlux_binding: None,
            source_checks: vec![PaidSourceCheck {
                key: runtime.0,
                expected_revision: Some("runtime-version".into()),
            }],
        },
    )?;
    store.seed(key(&["uos_ai", "codex_live_calls", "v1", "renew-call"]), "mapping-version", json!({"account_id":"renew-account","principal_id":"passkey-user:user-fixture","created_at_ms":store.now}))?;
    let token = "uos_ai_session_SYNTHETIC_FREEZE_ACTOR".to_owned();
    store.seed(
        passkeys::key("users", "user-fixture"),
        "user-1",
        PasskeyUser {
            id: "user-fixture".into(),
            handle: "fixture".into(),
            is_admin: true,
            credential_ids: vec!["fixture-credential".into()],
            created_at_ms: store.now,
            updated_at_ms: store.now,
        },
    )?;
    let session = PasskeySession {
        token: token.clone(),
        user_id: "user-fixture".into(),
        created_at_ms: store.now,
        expires_at_ms: store.now + 3_600_000,
        audience_origin: None,
    };
    store.seed(passkeys::key("sessions", &token), "session-1", &session)?;
    store.rows.lock().map_err(|e| e.to_string())?.insert(
        session_index::index_key(&session_index::digest(&token))?.0,
        session_index::record_for_session(&session, "session-1", None)?,
    );
    Ok(token)
}

async fn produce(input: Input) -> Outcome<Value> {
    assert_eq!(input.v, 1);
    assert!(input.now_ms > 0 && input.artifact_sha.len() == 40 && input.artifact_tree.len() == 40);
    assert_eq!(input.transport.key_id, "freeze-key");
    assert_eq!(input.transport.owner_id, "freeze-owner");
    assert_eq!(input.transport.owner_epoch, "freeze-epoch");
    assert_eq!(input.transport.key_base64url, base64url(&[31; 32]));
    assert_eq!(input.transport.codex_home, "/fixture/native-owner");
    assert_eq!(
        input.transport.capabilities,
        vec!["supervisor_read", "owner_renewal", "access_publish"]
    );
    let identity = NativeTransportIdentity::new(
        input.transport.key_id,
        input.transport.owner_id.clone(),
        input.transport.owner_epoch,
        [31; 32],
        BTreeSet::from([
            NativeCapability::SupervisorRead,
            NativeCapability::OwnerRenewal,
            NativeCapability::AccessPublish,
        ]),
    )?;
    let store = Store {
        rows: Arc::default(),
        sequence: Arc::new(AtomicU64::new(1)),
        now: input.now_ms,
    };
    let token = seed(&store, &identity, &input.transport.codex_home)?;
    let authority = Authority::with_clock(store.clone(), store.clone(), store.clone());
    let mut config = AuthConfig::from_source_values(&input.auth_values, true);
    config.candidate_passkeys = true;
    config
        .extra_relay_origins
        .insert("https://rust-candidate.ai.ubq.fi".into());
    let request = AuthRequest {
        method: "POST".into(),
        url: "https://gateway.internal/v1/responses".into(),
        headers: BTreeMap::from([("authorization".into(), format!("Bearer {token}"))]),
        ..Default::default()
    };
    let context = RequestContext {
        request_id: RequestId("freeze-original-request".into()),
        principal: PrincipalId("passkey-user:user-fixture".into()),
        key_id: None,
        route: "responses".into(),
        started_at_ms: input.now_ms,
    };
    let actor =
        authenticate_client_actor(&authority, &Host(input.now_ms), &config, &request, &context)
            .await?;
    let owner =
        RegisteredRenewalOwner::configured(input.transport.owner_id, input.transport.codex_home)?;
    let receiver =
        NativeRenewalAuthority::configured(&authority, &identity, &owner, &config, &Alive);
    let action = receiver
        .prepare_live_mapped_renewal(&actor, "renew-call")
        .await?;
    let operation = format!("freeze-positive-operation-{}", input.now_ms);
    let original = serde_json::to_vec(&NativeControlEnvelope {
        v: 1,
        operation_id: operation.clone(),
        request: NativeControlRequest::CodexRenewAccess {
            action_ref: action.action_ref().into(),
        },
    })?;
    let nonce = base64url(&[19; 32]);
    let mac = identity.sign(NATIVE_CONTROL_PATH, input.now_ms, &nonce, &original)?;
    let replay = StoreNonceAuthority::new(&store);
    let time = input.now_ms.to_string();
    let proof = NativeAuthenticator::new(&identity, &replay)
        .authenticate(
            NATIVE_CONTROL_PATH,
            NativeAuthHeaders {
                version: "1",
                key_id: identity.key_id(),
                time_ms: &time,
                nonce: &nonce,
                mac: &mac,
            },
            &original,
            input.now_ms,
        )
        .await?;
    let mut claim = receiver
        .claim(&proof, action.action_ref(), &operation, &original)
        .await?;
    receiver.start(&mut claim).await?;
    let access = format!(
        "e30.{}.synthetic",
        base64url(serde_json::to_string(&json!({"exp":input.now_ms/1000+3600}))?.as_bytes())
    );
    let next = codex_credential_identity(
        "renew-account",
        &Secret::new(access.clone()),
        &Secret::new(String::new()),
    );
    let fence = action.fence();
    let revision = format!("freeze-positive-publication-{}", input.now_ms);
    let publication = json!({"operation":"managed_access_publish","v":2,"action_ref":action.action_ref(),"operation_id":operation,
        "native_owner_id":fence.native_owner_id,"owner_revision":fence.owner_revision,"source_revision":fence.source_revision,"normalized_revision":fence.normalized_revision,
        "publication_revision":revision,"account_id_hash":fence.account_id_hash,"previous_native_source_generation":fence.source_native_generation,
        "native_source_generation":"c".repeat(64),"access_generation":next.native_generation_hash,
        "document":{"account_id":"renew-account","access_token":access,"native_refreshed_at":null}});
    let rows = store
        .rows
        .lock()
        .map_err(|e| e.to_string())?
        .iter()
        .map(|(key, record)| json!({"key":key,"record":record}))
        .collect::<Vec<_>>();
    Ok(
        json!({"v":1,"kind":"freeze_native_constructor_fixture","now_ms":input.now_ms,"artifact_sha":input.artifact_sha,"artifact_tree":input.artifact_tree,
        "constructors":"authenticate_client_actor/prepare_live_mapped_renewal/claim/start","rows":rows,
        "operation":{"operation_id":operation,"action_ref":action.action_ref(),"claim_ref":claim.claim_ref(),"publication_revision":revision},"publication":publication}),
    )
}

#[test]
fn produce_freeze_native_fixture() -> Outcome<()> {
    let mut bytes = Vec::new();
    std::io::stdin()
        .lock()
        .take(65_537)
        .read_to_end(&mut bytes)?;
    assert!(bytes.len() <= 65_536);
    let value = run(produce(serde_json::from_slice(&bytes)?))?;
    let mut stdout = std::io::stdout().lock();
    stdout.write_all(b"UOS_FREEZE_NATIVE_FIXTURE=")?;
    serde_json::to_writer(&mut stdout, &value)?;
    stdout.write_all(b"\n")?;
    stdout.flush()?;
    Ok(())
}
