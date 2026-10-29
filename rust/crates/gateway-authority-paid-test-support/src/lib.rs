//! Shared synthetic paid fixture only; real tests use actual defining checked operations.
use gateway_authority::store::Authority;
use gateway_authority_contracts::{TokenSource, unavailable};
use gateway_authority_keys as keys;
use gateway_authority_paid_backfill::{PaidBackfillOperations, PaidBackfillReceipt};
use gateway_authority_paid_records::{PaidAttempt, PaidCorrelation, PaidUsage};
use gateway_core::state::AtomicTransition;
use gateway_core::state::CommitOutcome;
use gateway_core::state::RecordMutation;
use gateway_core::{query::QueryPage, query::QueryRequest, state::AuthorityStore};
use gateway_core::{query::QueryStore, state::StoredRecord};
use gateway_kv_codec::tuple_key::TupleKey;
use gateway_kv_codec::{key::RecordKey, tuple_key::ScalarKeyPart};
use gateway_source::error::GatewayError;
use serde_json::{Value, json};
use std::{cell::Cell, cell::RefCell, collections::BTreeMap, future::Future, rc::Rc};
use std::{task::Context, task::Poll, task::Waker};
pub const NOW: i64 = 1_792_329_600_000;
pub type TestResult = Result<(), Box<dyn std::error::Error>>;
#[derive(Clone, Default)]
pub struct Store {
    pub rows: Rc<RefCell<BTreeMap<String, StoredRecord>>>,
    pub sequence: Rc<Cell<u64>>,
    pub failure: Rc<RefCell<Option<(RecordKey, bool)>>>,
    pub race: Rc<RefCell<Option<(RecordKey, Value)>>>,
}
impl Store {
    pub fn put(&self, key: RecordKey, value: Value) {
        let revision = self.next_token().unwrap();
        self.rows
            .borrow_mut()
            .insert(key.0, StoredRecord::json(revision, value, None));
    }
    pub fn row(&self, key: &RecordKey) -> StoredRecord {
        self.rows.borrow().get(&key.0).unwrap().clone()
    }
    pub fn value(&self, key: &RecordKey) -> Value {
        self.row(key).json_value().unwrap().clone()
    }
    pub fn fail(&self, key: RecordKey, error: bool) {
        *self.failure.borrow_mut() = Some((key, error));
    }
    pub fn authority(&self) -> Authority<Self, Self> {
        Authority::new(self.clone(), self.clone())
    }
}
impl TokenSource for Store {
    fn next_token(&self) -> Result<String, GatewayError> {
        let next = self.sequence.get() + 1;
        self.sequence.set(next);
        Ok(format!("revision-{next}"))
    }
}
impl AuthorityStore for Store {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        Ok(self.rows.borrow().get(&key.0).cloned())
    }
    async fn commit(&self, transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        let fail = self.failure.borrow().as_ref().is_some_and(|(key, _)| {
            transition.mutations.iter().any(|mutation| match mutation {
                RecordMutation::Put { key: k, .. } | RecordMutation::Delete { key: k } => k == key,
            })
        });
        if fail {
            let (_, error) = self.failure.borrow_mut().take().unwrap();
            if error {
                return Err(crate::unavailable(
                    "fixture_store_failure",
                    "source fixture commit failed",
                ));
            }
            return Ok(CommitOutcome::Conflict);
        }
        let race = self
            .race
            .borrow()
            .as_ref()
            .is_some_and(|(key, _)| transition.checks.iter().any(|check| &check.key == key));
        if race {
            let (key, value) = self.race.borrow_mut().take().unwrap();
            self.put(key, value);
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
impl QueryStore for Store {
    async fn query(&self, request: QueryRequest) -> Result<QueryPage, GatewayError> {
        let validated = request.validate()?;
        let mut entries = Vec::new();
        for (key, record) in self.rows.borrow().iter() {
            let key = RecordKey(key.clone());
            if validated.accepts_record(&key, record)? {
                entries.push(gateway_core::query::QueryEntry {
                    key,
                    record: record.clone(),
                });
            }
        }
        entries.sort_by_key(|entry| {
            TupleKey::from_record_key(&entry.key)
                .unwrap()
                .sort_bytes()
                .to_vec()
        });
        let more = entries.len() > request.limit as usize;
        entries.truncate(request.limit as usize);
        let next_cursor = if more {
            entries
                .last()
                .map(|entry| validated.cursor_after(&entry.key))
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
pub fn run<F: Future>(future: F) -> F::Output {
    let mut future = std::pin::pin!(future);
    let mut context = Context::from_waker(Waker::noop());
    loop {
        if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
            return value;
        }
    }
}
pub fn decode_json<T: serde::de::DeserializeOwned>(value: Value) -> Result<T, GatewayError> {
    gateway_source::source_json::SourceJson::from_serializable(&value)?.deserialize()
}
pub fn request(id: &str, settled: bool) -> Value {
    json!({"v":3,"reservation":{"key_id":"key","request_id":format!("original-{id}"),"attempt_id":id,"provider":"metered","window_reset_at_ms":NOW,"created_at_ms":NOW-1000,"reserved_microcredits":10},
        "policy_version":"v1","model":" model ","route":"responses","path":"/v1/responses","stream":false,"reasoning":null,"quota_per_credit":1_000_000,"provider_request_id":id,
        "dispatch_state":"dispatched","terminal_state":"completed","billing_state":if settled {"settled"} else {"pending"},"provider_quota":if settled {json!(1.5)} else {Value::Null},
        "input_tokens":10,"cached_input_tokens":2,"output_tokens":3,"spend_microcredits":if settled {json!(5)} else {Value::Null},"transport_started":true,"reconciliation_attempts":0,
        "last_reconciliation_at_ms":null,"dispatched_at_ms":NOW-900,"terminal_at_ms":NOW-800,"settled_at_ms":if settled {json!(NOW-800)} else {Value::Null},"updated_at_ms":NOW-800,"usage_rollup_at_ms":null,"source_extra":{"preserved":true}})
}
pub fn key(id: &str) -> RecordKey {
    keys::paid_attempt("key", id)
}
pub fn rollup_key(id: &str) -> RecordKey {
    keys::paid_rollup(
        (NOW - 1000) / 3_600_000 * 3_600_000,
        "model",
        "metered",
        gateway_authority_paid_records::rollup_shard(id),
    )
}
pub fn window(reset: i64) -> Value {
    json!({"v":3,"key_id":"key","policy_version":"v1","window_reset_at_ms":reset,"limit_microcredits":-1,"settled_microcredits":0,"reserved_microcredits":0,"pending_count":0,"updated_at_ms":NOW,"source_extra":"retained"})
}
pub fn backfill(store: &Store, limit: u32) -> PaidBackfillReceipt {
    run(store.authority().backfill_paid_usage(limit, NOW)).unwrap()
}

pub fn family(name: &str) -> RecordKey {
    TupleKey::from_scalar_parts(&[
        ScalarKeyPart::String("uos_ai"),
        ScalarKeyPart::String("paid_fallback"),
        ScalarKeyPart::String("v3"),
        ScalarKeyPart::String(name),
    ])
    .to_record_key()
}
pub fn pending_for_live_settlement(store: &Store, model: &str) -> PaidAttempt {
    let mut raw = request("a", false);
    raw["model"] = json!(model);
    let row: PaidAttempt = decode_json(raw.clone()).unwrap();
    store.put(key("a"), raw);
    store.put(
        keys::paid_pending("key", "a"),
        json!({"key_id":"key","attempt_id":"a","created_at_ms":NOW-1000,
            "next_reconciliation_at_ms":NOW}),
    );
    let mut active = window(NOW);
    active["reserved_microcredits"] = json!(10);
    active["pending_count"] = json!(1);
    store.put(keys::paid_window("key", NOW), active);
    row
}
pub fn live_usage() -> PaidUsage {
    PaidUsage {
        provider_quota: 5.0,
        input_tokens: 10,
        cached_input_tokens: Some(2),
        output_tokens: 3,
        correlation: PaidCorrelation::ProviderRequestId("a".into()),
    }
}
