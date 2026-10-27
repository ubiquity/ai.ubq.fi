use gateway_auth::provider_credentials::{codex_source_pool_key, parse_codex_source_pool};
use gateway_authority_contracts::{AuthorityClock, TokenSource, invalid};
use gateway_authority_keys as keys;
use gateway_authority_routing_evidence::reader::{
    RoutingEvidence, RoutingEvidenceReader, RoutingReadMode,
};
use gateway_authority_routing_records::{
    codec::{normalize, parse_routing_state},
    types::{
        PoolAccount, ProbeCircuit, ProbeLease, QuotaBlock, QuotaBlockSource, RoutingState,
        SubscriptionPool,
    },
};
use gateway_authority_routing_selection::{
    SelectionFactory, SelectionOperations, SelectionService,
    types::{AdmittedAccount, CohortProof, RouteSelection},
};
use gateway_contracts::{
    identity::ActiveSelection, identity::PoolVersionstamp, model::ModelId,
    provider::CodexQuotaClass,
};
use gateway_core::{
    state::AtomicTransition, state::AuthorityStore, state::CommitOutcome, state::RecordMutation,
    state::StoredRecord,
};
use gateway_kv_codec::key::RecordKey;
use gateway_source::error::GatewayError;
use serde::Serialize;
use serde_json::{Value, json};
use std::{
    cell::{Cell, RefCell},
    collections::BTreeMap,
    future::Future,
    sync::atomic::{AtomicI64, Ordering},
    task::{Context, Poll, Waker},
};

pub const NOW: i64 = 1_000_000;
pub const CLASS: CodexQuotaClass = CodexQuotaClass::Standard;
pub type TestResult = Result<(), GatewayError>;

#[derive(Default)]
pub struct ObservedStore {
    pub rows: RefCell<BTreeMap<String, StoredRecord>>,
    pub reads: RefCell<Vec<RecordKey>>,
    pub transitions: RefCell<Vec<AtomicTransition>>,
    pub reject: Cell<usize>,
    pub race: RefCell<Option<(RecordKey, StoredRecord)>>,
    pub failure: RefCell<Option<RecordKey>>,
}
impl AuthorityStore for ObservedStore {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        self.reads.borrow_mut().push(key.clone());
        if self.failure.borrow().as_ref() == Some(key) {
            return Err(invalid("synthetic backend read failed"));
        }
        Ok(self.rows.borrow().get(&key.0).cloned())
    }
    async fn commit(&self, transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        self.transitions.borrow_mut().push(transition.clone());
        if let Some((key, row)) = self.race.borrow_mut().take() {
            self.rows.borrow_mut().insert(key.0, row);
        }
        if self.reject.get() > 0 {
            self.reject.set(self.reject.get() - 1);
            return Ok(CommitOutcome::Conflict);
        }
        let mut rows = self.rows.borrow_mut();
        if transition.checks.iter().any(|check| {
            rows.get(&check.key.0).map(|row| &row.revision) != check.expected_revision.as_ref()
        }) {
            return Ok(CommitOutcome::Conflict);
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
#[derive(Default)]
pub struct Tokens {
    pub next: Cell<u64>,
    pub fixed: RefCell<Option<String>>,
}
impl TokenSource for Tokens {
    fn next_token(&self) -> Result<String, GatewayError> {
        if let Some(value) = self.fixed.borrow().as_ref() {
            return Ok(value.clone());
        }
        self.next.set(self.next.get() + 1);
        Ok(format!("selection-token-{}", self.next.get()))
    }
}
pub struct Clock(pub AtomicI64);
impl AuthorityClock for Clock {
    fn now_ms(&self) -> Result<i64, GatewayError> {
        Ok(self.0.load(Ordering::SeqCst))
    }
}
pub struct Fixture {
    pub store: ObservedStore,
    pub tokens: Tokens,
    pub clock: Clock,
    pub with_clock: Cell<bool>,
}
impl SelectionFactory for Fixture {
    type Store = ObservedStore;
    type Tokens = Tokens;
    fn selection(&self) -> SelectionService<'_, ObservedStore, Tokens> {
        SelectionService::new(
            &self.store,
            &self.tokens,
            self.with_clock
                .get()
                .then_some(&self.clock as &dyn AuthorityClock),
        )
    }
}
impl Fixture {
    pub fn new() -> Result<Self, GatewayError> {
        let fixture = Self {
            store: ObservedStore::default(),
            tokens: Tokens::default(),
            clock: Clock(AtomicI64::new(NOW)),
            with_clock: Cell::new(true),
        };
        fixture.install_source(json!({"accounts":[
            {"account_id":"a","access_token":"synthetic-a","refresh_token":"synthetic-refresh-a","updated_at_ms":1},
            {"account_id":"b","access_token":"synthetic-b","refresh_token":"synthetic-refresh-b","updated_at_ms":2}
        ],"updated_at_ms":2}), "source-r1")?;
        Ok(fixture)
    }
    pub fn install_source(&self, raw: Value, revision: &str) -> Result<(), GatewayError> {
        let source =
            parse_codex_source_pool(&raw)?.ok_or_else(|| invalid("synthetic source missing"))?;
        let pool = SubscriptionPool {
            v: 1,
            pool_versionstamp: PoolVersionstamp(revision.into()),
            accounts: source
                .accounts
                .iter()
                .map(|account| {
                    let identity = account.identity();
                    PoolAccount {
                        account_id_hash: identity.routing_account_hash,
                        admin_account_hash: identity.administrative_account_hash,
                        credential_version: identity.credential_version,
                        enabled: true,
                    }
                })
                .collect(),
        };
        self.store.rows.borrow_mut().insert(
            codex_source_pool_key().0,
            StoredRecord::json(revision, raw, None),
        );
        self.put(keys::pool(), "pool-r1", &pool)
    }
    pub fn raw_source(&self) -> Result<Value, GatewayError> {
        Ok(self.row(&codex_source_pool_key())?.value)
    }
    pub fn pool(&self) -> Result<SubscriptionPool, GatewayError> {
        serde_json::from_value(self.row(&keys::pool())?.value)
            .map_err(|_| invalid("synthetic pool malformed"))
    }
    pub fn row(&self, key: &RecordKey) -> Result<StoredRecord, GatewayError> {
        self.store
            .rows
            .borrow()
            .get(&key.0)
            .cloned()
            .ok_or_else(|| invalid("synthetic row missing"))
    }
    pub fn put<T: Serialize>(&self, key: RecordKey, revision: &str, value: &T) -> TestResult {
        self.store.rows.borrow_mut().insert(
            key.0,
            StoredRecord::json(
                revision,
                serde_json::to_value(value).map_err(|_| invalid("synthetic encoding failed"))?,
                None,
            ),
        );
        Ok(())
    }
    pub fn state(&self) -> Result<RoutingState, GatewayError> {
        let raw = self.store.rows.borrow().get(&keys::routing().0).cloned();
        normalize(
            &self.pool()?,
            raw.map(|row| parse_routing_state(&row.value)).transpose()?,
            NOW,
        )
    }
    pub fn write_state(&self, state: &RoutingState) -> TestResult {
        self.put(
            keys::routing(),
            &format!("state-test-{}", self.tokens.next_token()?),
            state,
        )
    }
    pub fn active(&self) -> Result<ActiveSelection, GatewayError> {
        serde_json::from_value(self.row(&keys::active())?.value)
            .map_err(|_| invalid("synthetic active malformed"))
    }
    pub fn select(&self) -> Result<RouteSelection, GatewayError> {
        ready(self.select_account(&ModelId::from("gpt-test"), CLASS, NOW))
    }
    pub fn admitted(&self) -> Result<AdmittedAccount, GatewayError> {
        match self.select()? {
            RouteSelection::Eligible(value) => Ok(value),
            other => Err(invalid(&format!(
                "expected eligible selection, got {other:?}"
            ))),
        }
    }
    pub fn proof(&self) -> Result<CohortProof, GatewayError> {
        match self.select()? {
            RouteSelection::QuotaBlocked { proof, .. }
            | RouteSelection::CredentialsInvalid { proof } => Ok(proof),
            _ => Err(invalid("expected cohort proof")),
        }
    }
    pub fn evidence(&self, now: i64) -> Result<RoutingEvidence, GatewayError> {
        ready(RoutingEvidenceReader::new(&self.store, now)?.read(RoutingReadMode::GlobalSelection))
    }
    pub fn block_all(&self) -> TestResult {
        let mut state = self.state()?;
        for slot in &mut state.slots {
            slot.insert_quota_block(CLASS, block(NOW - 100, NOW + 10_000));
        }
        self.write_state(&state)
    }
    pub fn capacity(&self, used: [f64; 2], reset: Option<i64>, sampled: i64) -> TestResult {
        let pool = self.pool()?;
        self.put(keys::capacity(), "capacity-r1", &json!({"v":1,"updated_at_ms":sampled,"observations":pool.accounts.iter().enumerate().map(|(slot, account)| json!({"slot":slot,"account_id_hash":account.account_id_hash,"state":"available","snapshot_at_ms":sampled,"windows":{"primary":{"used_percent":used[slot],"reset_at_ms":reset}},"additional_rate_limits":[]})).collect::<Vec<_>>()}))
    }
    pub fn clear_observations(&self) {
        self.store.reads.borrow_mut().clear();
        self.store.transitions.borrow_mut().clear();
    }
    pub fn last_transition(&self) -> Result<AtomicTransition, GatewayError> {
        self.store
            .transitions
            .borrow()
            .last()
            .cloned()
            .ok_or_else(|| invalid("synthetic transition missing"))
    }
}
pub fn block(at: i64, reset: i64) -> QuotaBlock {
    QuotaBlock {
        blocked_until_ms: reset,
        source: QuotaBlockSource::BodyResetsAt,
        legacy_fallback: false,
        observed_at_ms: Some(at),
        reset_at_ms: Some(reset),
        reset_is_stable: true,
        reset_ambiguous: false,
        recovery_pending: false,
    }
}
pub fn lease(class: CodexQuotaClass) -> ProbeLease {
    ProbeLease {
        token: "synthetic-probe".into(),
        generation: 0,
        circuit: ProbeCircuit::Quota,
        quota_class: Some(class),
        expires_at_ms: NOW + 100,
    }
}
pub fn ready<F: Future>(future: F) -> F::Output {
    let mut future = std::pin::pin!(future);
    match future
        .as_mut()
        .poll(&mut Context::from_waker(Waker::noop()))
    {
        Poll::Ready(value) => value,
        Poll::Pending => panic!("synthetic store unexpectedly yielded"),
    }
}
