//! Ordinary synthetic fixtures only: no actor, phase, body, admission, Start or currentness constructor.
use gateway_auth_contracts::{
    passkeys::{PasskeySession, PasskeyUser},
    types::{AuthConfig, AuthHost, AuthRequest},
    upstream::{AuthUpstreamRequest, AuthUpstreamResponse},
};
use gateway_auth_passkeys::{passkeys, session_index};
use gateway_auth_primitives::{api_keys, crypto::sha256_base64url};
use gateway_authority::store::Authority;
use gateway_authority_contracts::TokenSource;
use gateway_authority_keys as keys;
use gateway_contracts::identity::{ApiKeyId, PrincipalId, RequestContext, RequestId};
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, StoredRecord,
};
use gateway_kv_codec::key::RecordKey;
use gateway_source::error::GatewayError;
use serde::Serialize;
use std::{
    collections::BTreeMap,
    future::Future,
    sync::{
        Arc, Mutex,
        atomic::{AtomicI64, AtomicUsize, Ordering},
    },
    task::{Context, Poll, Waker},
};
pub fn run<F: Future>(future: F) -> F::Output {
    let mut context = Context::from_waker(Waker::noop());
    let mut future = std::pin::pin!(future);
    loop {
        if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
            return value;
        }
        std::thread::yield_now();
    }
}
#[derive(Clone)]
pub struct Store {
    pub rows: Arc<Mutex<BTreeMap<String, StoredRecord>>>,
    pub now: Arc<AtomicI64>,
    pub race: Arc<Mutex<PendingRace>>,
    pub commit_time: Arc<AtomicI64>,
}
pub type PendingRace = Option<(RecordKey, Option<StoredRecord>)>;
impl Default for Store {
    fn default() -> Self {
        Self {
            rows: Arc::default(),
            now: Arc::new(AtomicI64::new(1_000_000)),
            race: Arc::default(),
            commit_time: Arc::new(AtomicI64::new(0)),
        }
    }
}
impl Store {
    pub fn seed(&self, key: RecordKey, revision: &str, value: impl Serialize) {
        self.rows.lock().unwrap().insert(
            key.0,
            StoredRecord::json(revision, serde_json::to_value(value).unwrap(), None),
        );
    }
    pub fn race(&self, key: RecordKey, value: Option<StoredRecord>) {
        *self.race.lock().unwrap() = Some((key, value));
    }
}
impl gateway_authority_contracts::AuthorityClock for Store {
    fn now_ms(&self) -> Result<i64, GatewayError> {
        Ok(self.now.load(Ordering::SeqCst))
    }
}
impl AuthorityStore for Store {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        let value = self.rows.lock().unwrap().get(&key.0).cloned();
        let mut race = self.race.lock().unwrap();
        if race.as_ref().is_some_and(|(target, _)| target == key) {
            let (key, replacement) = race.take().unwrap();
            let mut rows = self.rows.lock().unwrap();
            match replacement {
                Some(row) => {
                    rows.insert(key.0, row);
                }
                None => {
                    rows.remove(&key.0);
                }
            }
        }
        Ok(value)
    }
    async fn commit(&self, tx: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        let mut rows = self.rows.lock().unwrap();
        for check in tx.checks {
            if rows.get(&check.key.0).map(|row| &row.revision) != check.expected_revision.as_ref() {
                return Ok(CommitOutcome::Conflict);
            }
        }
        for mutation in tx.mutations {
            match mutation {
                RecordMutation::Put { key, record } => {
                    rows.insert(key.0, record);
                }
                RecordMutation::Delete { key } => {
                    rows.remove(&key.0);
                }
            }
        }
        let jump = self.commit_time.swap(0, Ordering::SeqCst);
        if jump != 0 {
            self.now.store(jump, Ordering::SeqCst);
        }
        Ok(CommitOutcome::Committed)
    }
}
pub struct Tokens;
impl TokenSource for Tokens {
    fn next_token(&self) -> Result<String, GatewayError> {
        Ok("actor-fixture-unused-token".into())
    }
}
pub struct Host {
    pub store: Store,
    pub fetches: AtomicUsize,
}
impl Host {
    pub fn new(store: &Store) -> Self {
        Self {
            store: store.clone(),
            fetches: AtomicUsize::new(0),
        }
    }
}
impl gateway_auth_contracts::webauthn::WebAuthnCrypto for Host {
    async fn validate_key(
        &self,
        _: &gateway_auth_contracts::webauthn::CosePublicKey,
    ) -> Result<(), GatewayError> {
        Err(fixture_unavailable())
    }
    async fn verify_signature(
        &self,
        _: gateway_auth_contracts::webauthn::SignatureInput,
    ) -> Result<bool, GatewayError> {
        Err(fixture_unavailable())
    }
}
impl AuthHost for Host {
    fn now_ms(&self) -> i64 {
        self.store.now.load(Ordering::SeqCst)
    }
    fn random_bytes(&self, length: usize) -> Result<Vec<u8>, GatewayError> {
        Ok(vec![1; length])
    }
    async fn fetch_auth(
        &self,
        _: AuthUpstreamRequest,
    ) -> Result<AuthUpstreamResponse, GatewayError> {
        self.fetches.fetch_add(1, Ordering::SeqCst);
        Ok(AuthUpstreamResponse {
            status: 200,
            body: b"{}".to_vec(),
        })
    }
    async fn kernel_principal(
        &self,
        _: &str,
        _: &str,
        _: &str,
        _: &str,
    ) -> Result<String, GatewayError> {
        Ok("org".into())
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
pub fn request(token: &str) -> AuthRequest {
    AuthRequest {
        method: "POST".into(),
        url: "https://ai.ubq.fi/v1/responses".into(),
        headers: BTreeMap::from([("authorization".into(), format!("Bearer {token}"))]),
        ..Default::default()
    }
}
pub fn context(principal: &str, key: Option<&str>) -> RequestContext {
    RequestContext {
        request_id: RequestId("original-request".into()),
        principal: PrincipalId::from_scalar(principal.into()),
        key_id: key.map(|key| ApiKeyId(key.into())),
        route: "responses".into(),
        started_at_ms: 1_000_000,
    }
}
pub fn authority(store: &Store) -> Authority<Store, Tokens> {
    Authority::with_clock(store.clone(), Tokens, store.clone())
}
pub fn user(admin: bool) -> PasskeyUser {
    PasskeyUser {
        id: "user-fixture".into(),
        handle: "operator".into(),
        is_admin: admin,
        credential_ids: vec!["credential".into()],
        created_at_ms: 1,
        updated_at_ms: 1,
    }
}
pub fn session_setup(store: &Store, admin: bool, indexed: bool) -> String {
    let token = "uos_ai_session_SYNTHETIC_ACTOR_TOKEN".to_owned();
    let session = PasskeySession {
        token: token.clone(),
        user_id: "user-fixture".into(),
        created_at_ms: 1,
        expires_at_ms: 1_100_000,
        audience_origin: None,
    };
    store.seed(
        passkeys::key("users", "user-fixture"),
        "user-1",
        user(admin),
    );
    store.seed(passkeys::key("sessions", &token), "session-1", &session);
    if indexed {
        store.rows.lock().unwrap().insert(
            session_index::index_key(&session_index::digest(&token))
                .unwrap()
                .0,
            session_index::record_for_session(&session, "session-1", None).unwrap(),
        );
    }
    token
}
pub fn api_setup(store: &Store, expiry: i64) -> String {
    let token = format!("u_{}", "a".repeat(64));
    let hash = sha256_base64url(token.as_bytes());
    store.seed(
        api_keys::hash_key(&hash),
        "hash-1",
        api_keys::ApiKeyHashRecord {
            id: "api-fixture".into(),
            expires_at_ms: expiry,
            revoked_at_ms: None,
            usage_limit_requests: 10,
            usage_requests: 0,
            usage_reset_at_ms: 2_000_000,
            window_ms: 1_000_000,
            usage_quota_version: 3,
            paid_fallback_enabled: false,
            paid_fallback_limit_microcredits: 0,
            paid_fallback_spent_microcredits: 0,
            paid_fallback_reserved_microcredits: 0,
            paid_fallback_reservation_request_id: None,
        },
    );
    store.seed(
        keys::api_policy("api-fixture"),
        "policy-1",
        gateway_authority_api_protocol::ApiKeyPolicy {
            v: 3,
            key_id: ApiKeyId("api-fixture".into()),
            expires_at_ms: expiry,
            usage_limit_requests: 10,
            window_ms: 1_000_000,
            usage_reset_at_ms: 2_000_000,
            paid_fallback_enabled: false,
            paid_fallback_limit_microcredits: 0,
            paid_fallback_quota_per_credit: 0,
            paid_fallback_pricing_checked_at_ms: 0,
            paid_fallback_models: vec![],
            paid_fallback_max_exposure_microcredits: BTreeMap::new(),
        },
    );
    token
}
// Synthetic legacy/passkey fixture has no app-session date capability; unknown dates refuse.
impl gateway_auth_passport_contracts::session::PassportDate for Host {
    fn parse_date_ms(&self, _value: &gateway_source::js_string::JsString16) -> f64 {
        f64::NAN
    }
}

fn fixture_unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Original-request native actor authority is unavailable or changed.",
        "native_actor_unavailable",
    )
}
