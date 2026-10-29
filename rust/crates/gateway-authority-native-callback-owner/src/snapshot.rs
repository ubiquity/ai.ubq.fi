use gateway_authority_contracts::{invalid, text, validate_time};
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RevisionCheck, StoredRecord,
};
use gateway_kv_codec::key::RecordKey;
use gateway_source::error::GatewayError;
use serde::de::DeserializeOwned;
use std::collections::BTreeMap;

pub(crate) struct Snapshot<'a, S> {
    store: &'a S,
    reads: BTreeMap<String, Option<StoredRecord>>,
    pub(crate) now: i64,
}
impl<'a, S: AuthorityStore> Snapshot<'a, S> {
    pub(crate) fn new(store: &'a S, now: i64) -> Self {
        Self {
            store,
            reads: BTreeMap::new(),
            now,
        }
    }
    pub(crate) async fn record(
        &mut self,
        key: &RecordKey,
    ) -> Result<Option<StoredRecord>, GatewayError> {
        if !self.reads.contains_key(&key.0) {
            let record = self.store.read(key).await?;
            if let Some(row) = &record {
                row.validate_version()?;
                text(&row.revision)?;
                if let Some(expiry) = row.expires_at_ms {
                    validate_time(expiry)?;
                }
            }
            self.reads.insert(key.0.clone(), record);
        }
        let row = self.reads.get(&key.0).and_then(Option::as_ref);
        if row.is_some_and(|row| row.expires_at_ms.is_some_and(|at| at <= self.now)) {
            return Ok(None);
        }
        Ok(row.cloned())
    }
    pub(crate) async fn read<T: DeserializeOwned>(
        &mut self,
        key: &RecordKey,
    ) -> Result<Option<T>, GatewayError> {
        self.record(key)
            .await?
            .map(|row| {
                serde_json::from_value(row.json_value()?.clone())
                    .map_err(|_| invalid("Malformed authority record."))
            })
            .transpose()
    }
    pub(crate) fn revision(&self, key: &RecordKey) -> Option<String> {
        self.reads
            .get(&key.0)
            .and_then(Option::as_ref)
            .map(|row| row.revision.clone())
    }
    pub(crate) async fn commit(self) -> Result<bool, GatewayError> {
        let checks = self
            .reads
            .into_iter()
            .map(|(key, record)| RevisionCheck {
                key: RecordKey(key),
                expected_revision: record.map(|row| row.revision),
            })
            .collect();
        Ok(self
            .store
            .commit(AtomicTransition {
                checks,
                mutations: Vec::new(),
            })
            .await?
            == CommitOutcome::Committed)
    }
}
