use gateway_core::{
    query::{QueryEntry, QueryPage, QueryRequest, QueryStore},
    state::{AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, StoredRecord},
};
use gateway_kv_codec::{key::RecordKey, tuple_key::TupleKey};
use gateway_source::error::GatewayError;
use serde_json::{Value, json};
use std::{
    cell::{Cell, RefCell},
    collections::BTreeMap,
    rc::Rc,
};

fn json_tuple(parts: &[Value]) -> TupleKey {
    match TupleKey::from_json_parts(parts) {
        Ok(key) => key,
        Err(error) => panic!("called `Result::unwrap()` on an `Err` value: {error:?}"),
    }
}
fn record_tuple(key: &RecordKey) -> TupleKey {
    match TupleKey::from_record_key(key) {
        Ok(key) => key,
        Err(error) => panic!("called `Result::unwrap()` on an `Err` value: {error:?}"),
    }
}
fn key(parts: Value) -> RecordKey {
    let Some(parts) = parts.as_array() else {
        panic!("called `Option::unwrap()` on a `None` value");
    };
    json_tuple(parts).to_record_key()
}
pub fn error(code: &str) -> GatewayError {
    GatewayError::unavailable("fixture", code)
}
#[derive(Clone)]
pub struct Memory {
    pub rows: Rc<RefCell<BTreeMap<String, StoredRecord>>>,
    pub clock: Rc<Cell<i64>>,
    pub rate_conflicts: Rc<Cell<usize>>,
    pub cache_quota: Rc<Cell<usize>>,
}
impl Memory {
    pub fn new(clock: Rc<Cell<i64>>) -> Self {
        Self {
            rows: Rc::new(RefCell::new(BTreeMap::new())),
            clock,
            rate_conflicts: Rc::new(Cell::new(0)),
            cache_quota: Rc::new(Cell::new(0)),
        }
    }
    pub fn seed(&self, parts: Value, value: Value, expiry: Option<i64>) {
        self.rows
            .borrow_mut()
            .insert(key(parts).0, StoredRecord::json("seed", value, expiry));
    }
    pub fn budget(&self, requests: u64, tokens: u64, window_start: i64) {
        self.seed(
            json!(["embeddings", "v1", "rate", "voyage"]),
            json!({"window_start_ms":window_start,"requests":requests,"tokens":tokens}),
            None,
        )
    }
    pub fn live(&self, key: &RecordKey) -> Option<StoredRecord> {
        self.rows
            .borrow()
            .get(&key.0)
            .filter(|r| r.expires_at_ms.is_none_or(|e| e > self.clock.get()))
            .cloned()
    }
}
impl AuthorityStore for Memory {
    async fn read(&self, k: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        Ok(self.live(k))
    }
    async fn commit(&self, t: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        let rate = key(json!(["embeddings", "v1", "rate", "voyage"]));
        if t.checks.iter().any(|c| c.key == rate) && self.rate_conflicts.get() > 0 {
            self.rate_conflicts.set(self.rate_conflicts.get() - 1);
            return Ok(CommitOutcome::Conflict);
        }
        if self.cache_quota.get() > 0
            && t.mutations.iter().any(|m| match m {
                RecordMutation::Put { key, .. } => {
                    record_tuple(key).is_strict_descendant_of(&json_tuple(&[
                        json!("embeddings"),
                        json!("v2"),
                        json!("cache"),
                    ]))
                }
                _ => false,
            })
        {
            self.cache_quota.set(self.cache_quota.get() - 1);
            return Err(error("storage_quota"));
        }
        for c in &t.checks {
            if self.live(&c.key).map(|r| r.revision) != c.expected_revision {
                return Ok(CommitOutcome::Conflict);
            }
        }
        let mut rows = self.rows.borrow_mut();
        for m in t.mutations {
            match m {
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
impl QueryStore for Memory {
    async fn query(&self, r: QueryRequest) -> Result<QueryPage, GatewayError> {
        let validated = r.validate()?;
        let mut entries = Vec::new();
        for (key, row) in self.rows.borrow().iter() {
            let key = RecordKey(key.clone());
            if validated.accepts_record(&key, row)? {
                entries.push(QueryEntry {
                    key,
                    record: row.clone(),
                });
            }
        }
        entries.sort_by_key(|e| record_tuple(&e.key));
        if r.reverse {
            entries.reverse()
        }
        let more = entries.len() > r.limit as usize;
        entries.truncate(r.limit as usize);
        let next_cursor = if more {
            entries
                .last()
                .map(|e| validated.cursor_after(&e.key))
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
