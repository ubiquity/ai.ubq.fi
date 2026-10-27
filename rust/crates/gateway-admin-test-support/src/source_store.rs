use gateway_source::error::GatewayError;
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, StoredRecord,
};
use gateway_kv_codec::key::RecordKey;
use std::{
    cell::{Cell, RefCell},
    collections::BTreeMap,
};

#[derive(Default)]
pub struct Store {
    pub rows: RefCell<BTreeMap<String, StoredRecord>>,
    pub conflict: Cell<bool>,
}
impl AuthorityStore for Store {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        Ok(self.rows.borrow().get(&key.0).cloned())
    }
    async fn commit(&self, transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        if self.conflict.replace(false) {
            return Ok(CommitOutcome::Conflict);
        }
        let mut rows = self.rows.borrow_mut();
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
impl gateway_core::query::QueryStore for Store {
    async fn query(
        &self,
        _: gateway_core::query::QueryRequest,
    ) -> Result<gateway_core::query::QueryPage, GatewayError> {
        Err(GatewayError::unavailable(
            "This transport fixture does not permit queries",
            "fixture_query_denied",
        ))
    }
}
impl AuthorityStore for &Store {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        Store::read(self, key).await
    }
    async fn commit(&self, transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        Store::commit(self, transition).await
    }
}
