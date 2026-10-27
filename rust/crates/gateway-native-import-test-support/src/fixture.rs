//! Synthetic DATA and real public store/host components only; no context/actor/Start/durability/custody factory.
//! Real proof/authority fixtures for native typed AUTH context only.
use gateway_auth_contracts::{
    passkeys::{PasskeySession, PasskeyUser},
    types::{AuthConfig, AuthHost, AuthRequest},
    upstream::{AuthUpstreamRequest, AuthUpstreamResponse},
};
use gateway_auth_credentials::provider_credentials::{
    CodexNativeOwner, CodexSourceAccount, CodexSourcePool, MANAGED_NATIVE_SOURCE_VERSION,
    ManagedNativeSource, codex_source_pool_key,
};
use gateway_auth_native_contracts::native_import::NativeImportTarget;
use gateway_auth_native_proof::native_auth::{NativeCapability, NativeTransportIdentity};
use gateway_auth_passkeys::{passkeys, session_index};
use gateway_auth_primitives::crypto::{Secret, sha256_hex};
use gateway_authority::native_control::{
    NativeRenewalCancellation, actor, receiver::RegisteredRenewalOwner,
};
use gateway_authority::store::Authority;
use gateway_authority_contracts::{AuthorityClock, TokenSource};
use gateway_authority_keys as keys;
use gateway_authority_paid_control_records::{
    PAID_CONTROL_CONFIG_VERSION, PaidControlConfig, PaidControlOwner, PaidSourceCheck,
};
use gateway_authority_routing_records::{types::PoolAccount, types::SubscriptionPool};
use gateway_core::identity::{PoolVersionstamp, PrincipalId, RequestContext, RequestId};
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, RecordMutation, StoredRecord,
};
use gateway_source::error::GatewayError;
use serde::Serialize;
use serde_json::json;
use std::{
    collections::{BTreeMap, BTreeSet},
    sync::{
        Arc, Condvar, Mutex,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
};
pub fn now_ms() -> Result<i64, GatewayError> {
    i64::try_from(
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map_err(|_| fail())?
            .as_millis(),
    )
    .map_err(|_| fail())
}
#[derive(Clone, Default)]
pub struct Store {
    pub rows: Arc<Mutex<BTreeMap<String, StoredRecord>>>,
    pub captures: Arc<Mutex<Vec<String>>>,
    pub action_key: Arc<Mutex<Option<RecordKey>>>,
    admitted: Arc<(Mutex<usize>, Condvar)>,
}
impl Store {
    pub fn seed(
        &self,
        key: RecordKey,
        revision: &str,
        value: impl Serialize,
    ) -> Result<(), GatewayError> {
        self.rows.lock().map_err(|_| fail())?.insert(
            key.0,
            StoredRecord::json(
                revision,
                serde_json::to_value(value).map_err(|_| fail())?,
                None,
            ),
        );
        Ok(())
    }
    pub fn wait_header_admitted(&self) -> Result<(), GatewayError> {
        let (count, ready) = &*self.admitted;
        let (count, timeout) = ready
            .wait_timeout_while(
                count.lock().map_err(|_| fail())?,
                std::time::Duration::from_secs(2),
                |count| *count == 0,
            )
            .map_err(|_| fail())?;
        assert!(
            !timeout.timed_out() && *count > 0,
            "real current-action CAS did not occur before raw read"
        );
        Ok(())
    }
    pub fn captured(&self) -> Result<String, GatewayError> {
        Ok(self.captures.lock().map_err(|_| fail())?.join("\n"))
    }
}
impl AuthorityClock for Store {
    fn now_ms(&self) -> Result<i64, GatewayError> {
        now_ms()
    }
}
impl AuthorityStore for Store {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        self.captures
            .lock()
            .map_err(|_| fail())?
            .push(format!("read:{}", key.0));
        Ok(self.rows.lock().map_err(|_| fail())?.get(&key.0).cloned())
    }
    async fn commit(&self, tx: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        self.captures
            .lock()
            .map_err(|_|fail())?
             .push(json!({
                "checks": tx.checks.iter().map(|check| json!({"key":check.key.0,"expected_revision":check.expected_revision})).collect::<Vec<_>>(),
                "mutations": tx.mutations.iter().map(|mutation| match mutation {
                    RecordMutation::Put{key,record} => json!({"put":{"key":key.0,"record":record}}),
                    RecordMutation::Delete{key} => json!({"delete":{"key":key.0}}),
                }).collect::<Vec<_>>()
            }).to_string());
        let mut rows = self.rows.lock().map_err(|_| fail())?;
        if tx
            .checks
            .iter()
            .any(|c| rows.get(&c.key.0).map(|r| &r.revision) != c.expected_revision.as_ref())
        {
            return Ok(CommitOutcome::Conflict);
        }
        let actual_action_observation = tx.mutations.is_empty()
            && self
                .action_key
                .lock()
                .map_err(|_| fail())?
                .as_ref()
                .is_some_and(|key| tx.checks.iter().any(|check| &check.key == key));
        for m in tx.mutations {
            match m {
                RecordMutation::Put { key, record } => {
                    rows.insert(key.0, record);
                }
                RecordMutation::Delete { key } => {
                    rows.remove(&key.0);
                }
            }
        }
        drop(rows);
        if actual_action_observation {
            let (count, ready) = &*self.admitted;
            *count.lock().map_err(|_| fail())? += 1;
            ready.notify_all();
        }
        Ok(CommitOutcome::Committed)
    }
}
#[derive(Clone, Default)]
pub struct Tokens(Arc<AtomicU64>);
impl TokenSource for Tokens {
    fn next_token(&self) -> Result<String, GatewayError> {
        Ok(format!(
            "native-client-token-{}",
            self.0.fetch_add(1, Ordering::SeqCst)
        ))
    }
}
#[derive(Default)]
pub struct Cancel(pub AtomicBool);
impl NativeRenewalCancellation for Cancel {
    fn cancelled(&self) -> bool {
        self.0.load(Ordering::SeqCst)
    }
}
pub struct Host;
impl gateway_auth_contracts::webauthn::WebAuthnCrypto for Host {
    async fn validate_key(
        &self,
        _: &gateway_auth_contracts::webauthn::CosePublicKey,
    ) -> Result<(), GatewayError> {
        Err(fail())
    }
    async fn verify_signature(
        &self,
        _: gateway_auth_contracts::webauthn::SignatureInput,
    ) -> Result<bool, GatewayError> {
        Err(fail())
    }
}
impl AuthHost for Host {
    fn now_ms(&self) -> i64 {
        // AuthHost has no fallible clock channel; an unusable fixture clock must fail the test.
        match now_ms() {
            Ok(value) => value,
            Err(error) => panic!("Synthetic AuthHost clock failed: {error}"),
        }
    }
    fn random_bytes(&self, length: usize) -> Result<Vec<u8>, GatewayError> {
        Ok(vec![1; length])
    }
    async fn fetch_auth(
        &self,
        _: AuthUpstreamRequest,
    ) -> Result<AuthUpstreamResponse, GatewayError> {
        Err(fail())
    }
    async fn kernel_principal(
        &self,
        _: &str,
        _: &str,
        _: &str,
        _: &str,
    ) -> Result<String, GatewayError> {
        Err(fail())
    }
}
pub fn fail() -> GatewayError {
    GatewayError::unavailable("Synthetic native client refusal", "fixture_refusal")
}
pub fn run<F: std::future::Future>(future: F) -> F::Output {
    let mut future = std::pin::pin!(future);
    let mut cx = std::task::Context::from_waker(std::task::Waker::noop());
    loop {
        if let std::task::Poll::Ready(v) = future.as_mut().poll(&mut cx) {
            return v;
        }
        std::thread::yield_now();
    }
}
pub struct Fixture {
    pub store: Store,
    pub identity: NativeTransportIdentity,
    pub owner: RegisteredRenewalOwner,
    pub config: AuthConfig,
    pub cancel: Cancel,
    pub account: String,
    pub token: String,
}
impl Fixture {
    pub fn new() -> Result<Self, GatewayError> {
        let store = Store::default();
        let identity = NativeTransportIdentity::new(
            "import-client".into(),
            "owner".into(),
            "epoch".into(),
            [7; 32],
            BTreeSet::from([NativeCapability::CodexAuthImport]),
        )?;
        store.seed(
            keys::paid_control_owner(),
            "owner-1",
            PaidControlOwner {
                v: 1,
                owner_id: "owner".into(),
                owner_epoch: "epoch".into(),
                key_fingerprint: identity.key_fingerprint().into(),
            },
        )?;
        store.seed(
            keys::paid_control_config(),
            "config-1",
            PaidControlConfig {
                v: PAID_CONTROL_CONFIG_VERSION,
                owner_id: "owner".into(),
                owner_epoch: "epoch".into(),
                generation: "cfg-generation".into(),
                configuration_fingerprint: "fixture-cfg".into(),
                surplus_binding: None,
                openlux_discovery_binding: None,
                openlux_binding: None,
                source_checks: vec![PaidSourceCheck {
                    key: gateway_auth_primitives::store::key(json!([
                        "uos_ai",
                        "runtime_config",
                        "v2"
                    ]))
                    .0,
                    expected_revision: None,
                }],
            },
        )?;
        let hash = seed_account(&store)?;
        let token = seed_admin(&store)?;
        Ok(Self {
            store,
            identity,
            owner: RegisteredRenewalOwner::configured(
                "native-owner".into(),
                "/tmp/native-import-synthetic-owner".into(),
            )?,
            config: AuthConfig {
                production: true,
                hosted: true,
                git_sha_known: true,
                ..Default::default()
            },
            cancel: Cancel::default(),
            account: hash,
            token,
        })
    }
    pub fn authority(&self) -> Authority<Store, Tokens> {
        Authority::with_clock(self.store.clone(), Tokens::default(), self.store.clone())
    }
    pub async fn actor(
        &self,
        authority: &Authority<Store, Tokens>,
    ) -> Result<actor::NativeRenewalActor, GatewayError> {
        actor::authenticate_admin_actor(
            authority,
            &Host,
            &self.config,
            &AuthRequest {
                method: "POST".into(),
                url: "https://synthetic-admin.invalid/native-client-preparation".into(),
                headers: BTreeMap::from([(
                    "authorization".into(),
                    format!("Bearer {}", self.token),
                )]),
                ..Default::default()
            },
            &RequestContext {
                request_id: RequestId("native-client-original".into()),
                principal: PrincipalId("passkey-user:user-fixture".into()),
                key_id: None,
                route: "native-client-preparation".into(),
                started_at_ms: now_ms()?,
            },
        )
        .await
    }
    pub fn target(&self) -> NativeImportTarget {
        NativeImportTarget::ExistingAccount {
            account_id_hash: self.account.clone(),
        }
    }
}

fn seed_account(store: &Store) -> Result<String, GatewayError> {
    let mut account = CodexSourceAccount {
        account_id: "synthetic-account".into(),
        access_token: Secret::new("SYNTHETIC_ACCESS_PROJECTION".into()),
        refresh_token: Secret::new(String::new()),
        updated_at_ms: now_ms()? as f64,
        native_owner: Some(CodexNativeOwner {
            codex_home: "/tmp/native-import-synthetic-owner".into(),
            generation_hash: sha256_hex(b"native-generation"),
            native_refreshed_at: None,
            external_user_id: None,
            managed_source: Some(ManagedNativeSource {
                v: MANAGED_NATIVE_SOURCE_VERSION,
                generation_hash: sha256_hex(b"physical-native-generation"),
            }),
        }),
    };
    let source = account.identity();
    account
        .native_owner
        .as_mut()
        .ok_or_else(fail)?
        .generation_hash = source.native_generation_hash.clone();
    let hash = source.routing_account_hash.0.clone();
    store.seed(
        codex_source_pool_key(),
        "source-1",
        CodexSourcePool {
            accounts: vec![account],
            updated_at_ms: now_ms()? as f64,
        }
        .private_source_value(),
    )?;
    store.seed(
        keys::pool(),
        "pool-1",
        SubscriptionPool {
            v: 1,
            pool_versionstamp: PoolVersionstamp("source-1".into()),
            accounts: vec![PoolAccount {
                account_id_hash: source.routing_account_hash,
                admin_account_hash: source.administrative_account_hash,
                credential_version: source.credential_version,
                enabled: true,
            }],
        },
    )?;
    Ok(hash)
}
fn seed_admin(store: &Store) -> Result<String, GatewayError> {
    let token = "uos_ai_session_SYNTHETIC_CLIENT_ADMIN".to_owned();
    let session = PasskeySession {
        token: token.clone(),
        user_id: "user-fixture".into(),
        created_at_ms: 1,
        expires_at_ms: now_ms()? + 100_000,
        audience_origin: None,
    };
    store.seed(
        passkeys::key("users", "user-fixture"),
        "user-1",
        PasskeyUser {
            id: "user-fixture".into(),
            handle: "operator".into(),
            is_admin: true,
            credential_ids: vec!["credential".into()],
            created_at_ms: 1,
            updated_at_ms: 1,
        },
    )?;
    store.seed(passkeys::key("sessions", &token), "session-1", &session)?;
    store.rows.lock().map_err(|_| fail())?.insert(
        session_index::index_key(&session_index::digest(&token))?.0,
        session_index::record_for_session(&session, "session-1", None)?,
    );
    Ok(token)
}

impl gateway_core::query::QueryStore for Store {
    async fn query(
        &self,
        request: gateway_core::query::QueryRequest,
    ) -> Result<gateway_core::query::QueryPage, GatewayError> {
        use gateway_core::{
            query::{QueryEntry, QueryPage},
            state::RecordKey,
            tuple_key::TupleKey,
        };
        let checked = request.validate()?;
        let rows = self.rows.lock().map_err(|_| fail())?;
        let mut matching = Vec::new();
        for (key, record) in rows.iter() {
            let key = RecordKey(key.clone());
            if checked.accepts_record(&key, record)? {
                matching.push((
                    TupleKey::from_record_key(&key)?.sort_bytes().to_vec(),
                    QueryEntry {
                        key,
                        record: record.clone(),
                    },
                ));
            }
        }
        matching.sort_by(|a, b| a.0.cmp(&b.0));
        if checked.reverse {
            matching.reverse();
        }
        let more = matching.len() > checked.limit as usize;
        matching.truncate(checked.limit as usize);
        let entries = matching
            .into_iter()
            .map(|(_, entry)| entry)
            .collect::<Vec<_>>();
        let next_cursor = if more {
            entries
                .last()
                .map(|entry| checked.cursor_after(&entry.key))
                .transpose()?
        } else {
            None
        };
        Ok(QueryPage {
            entries,
            next_cursor,
        })
    }
}

pub fn config() -> AuthConfig {
    AuthConfig {
        production: true,
        hosted: true,
        git_sha_known: true,
        ..Default::default()
    }
}
pub fn authority(store: &Store) -> Authority<Store, Tokens> {
    Authority::with_clock(store.clone(), Tokens::default(), store.clone())
}
