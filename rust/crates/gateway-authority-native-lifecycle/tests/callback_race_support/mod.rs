use super::support::Store;
use gateway_core::state::{AtomicTransition, AuthorityStore, CommitOutcome, StoredRecord};
use gateway_kv_codec::key::RecordKey;
use gateway_source::error::GatewayError;
use std::cell::{Cell, RefCell};

pub(super) enum Mutation {
    Delete(RecordKey),
    Revision(RecordKey),
    ActionVersion(RecordKey),
}
pub(super) struct RaceStore {
    pub(super) inner: Store,
    pub(super) next: RefCell<Option<Mutation>>,
    pub(super) action: RefCell<Option<RecordKey>>,
    pub(super) skip_reads: Cell<u8>,
    pub(super) mutation_only: Cell<bool>,
}
impl RaceStore {
    pub(super) fn new() -> Self {
        Self {
            inner: Store::default(),
            next: RefCell::new(None),
            action: RefCell::new(None),
            skip_reads: Cell::new(0),
            mutation_only: Cell::new(false),
        }
    }
}
impl AuthorityStore for RaceStore {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        self.inner.read(key).await
    }
    async fn commit(&self, tx: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        let mut next = self.next.borrow_mut();
        let matches = next.as_ref().is_some_and(|m| {
            let key = match m {
                Mutation::Delete(k) | Mutation::Revision(k) | Mutation::ActionVersion(k) => k,
            };
            tx.checks.iter().any(|c| &c.key == key)
        });
        let actual_effect = self
            .action
            .borrow()
            .as_ref()
            .is_some_and(|key| tx.checks.iter().any(|c| &c.key == key));
        let selected = if !actual_effect || (self.mutation_only.get() && tx.mutations.is_empty()) {
            false
        } else if tx.mutations.is_empty() && self.skip_reads.get() > 0 {
            self.skip_reads.set(self.skip_reads.get() - 1);
            false
        } else {
            true
        };
        if matches && selected {
            if let Some(mutation) = next.take() {
                match mutation {
                    Mutation::Delete(key) => {
                        self.inner.0.borrow_mut().remove(&key.0);
                    }
                    Mutation::Revision(key) => {
                        if let Some(row) = self.inner.0.borrow_mut().get_mut(&key.0) {
                            row.revision = "actual-final-cas-race".into();
                        }
                    }
                    Mutation::ActionVersion(key) => {
                        let original = self
                            .inner
                            .0
                            .borrow()
                            .get(&key.0)
                            .cloned()
                            .ok_or_else(super::support::unavailable)?;
                        let raw =
                            gateway_core::source_control::decode_source_control_record(&original)?;
                        let changed = gateway_source::source_json::SourceJson::parse(
                            &raw.as_str().replacen("\"v\":3", "\"v\":2", 1),
                        )?;
                        self.inner.0.borrow_mut().insert(
                            key.0,
                            gateway_core::source_control::encode_source_control_record(
                                "actual-version-race",
                                &changed,
                                None,
                            )?,
                        );
                    }
                }
            }
        }
        drop(next);
        self.inner.commit(tx).await
    }
}
