//! Admin runtime fixture storage retaining CAS, query and read-count behavior.
use gateway_core::{
    query::{QueryEntry, QueryPage, QueryRequest, QueryStore},
    state::{AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, StoredRecord},
};
use gateway_kv_codec::key::RecordKey;
use gateway_source::error::GatewayError;
use serde_json::Value;
use std::{
    cell::{Cell, RefCell},
    collections::BTreeMap,
};

#[derive(Default)]
pub struct Store {
    pub rows: RefCell<BTreeMap<String, StoredRecord>>,
    pub reads: Cell<usize>,
    pub conflicts: Cell<u32>,
}
impl Store {
    pub fn put(&self, key: RecordKey, value: Value) {
        self.rows
            .borrow_mut()
            .insert(key.0, StoredRecord::json("fixture", value, None));
    }
    pub fn get(&self, key: RecordKey) -> Option<Value> {
        self.rows.borrow().get(&key.0).map(|r| r.value.clone())
    }
}
impl AuthorityStore for Store {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        self.reads.set(self.reads.get() + 1);
        Ok(self.rows.borrow().get(&key.0).cloned())
    }
    async fn commit(&self, tx: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        if self.conflicts.get() > 0 {
            self.conflicts.set(self.conflicts.get() - 1);
            return Ok(CommitOutcome::Conflict);
        }
        let mut rows = self.rows.borrow_mut();
        for check in tx.checks {
            if rows.get(&check.key.0).map(|r| &r.revision) != check.expected_revision.as_ref() {
                return Ok(CommitOutcome::Conflict);
            }
        }
        for write in tx.mutations {
            match write {
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
                entries.push(QueryEntry {
                    key,
                    record: record.clone(),
                })
            }
        }
        entries.sort_by_key(|e| {
            gateway_kv_codec::tuple_key::TupleKey::from_record_key(&e.key)
                .unwrap_or_else(|error| panic!("Invalid fixture key: {error}"))
        });
        if request.reverse {
            entries.reverse()
        }
        let more = entries.len() > request.limit as usize;
        entries.truncate(request.limit as usize);
        let next_cursor = if more {
            Some(
                validated.cursor_after(
                    &entries
                        .last()
                        .unwrap_or_else(|| panic!("Missing fixture page cursor"))
                        .key,
                )?,
            )
        } else {
            None
        };
        Ok(QueryPage {
            entries,
            next_cursor,
        })
    }
}
