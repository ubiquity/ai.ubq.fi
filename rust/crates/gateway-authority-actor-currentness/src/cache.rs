//! Private metadata/expiry cache for fixed actor families; no public storage surface.
use gateway_authority_contracts::{invalid, text, validate_time};
use gateway_core::{
    source_control::decode_source_control_record, state::AuthorityStore, state::RevisionCheck,
    state::StoredRecord,
};
use gateway_kv_codec::key::RecordKey;
use gateway_source::error::GatewayError;
use serde::de::DeserializeOwned;
use std::collections::BTreeMap;
pub(crate) struct ReadCache<'a, S> {
    store: &'a S,
    pub(crate) now: i64,
    reads: BTreeMap<String, Option<StoredRecord>>,
}
impl<'a, S: AuthorityStore> ReadCache<'a, S> {
    pub(crate) fn new(store: &'a S, now: i64) -> Result<Self, GatewayError> {
        validate_time(now)?;
        Ok(Self {
            store,
            now,
            reads: BTreeMap::new(),
        })
    }
    pub(crate) async fn cached_record(
        &mut self,
        key: &RecordKey,
    ) -> Result<Option<&StoredRecord>, GatewayError> {
        if !self.reads.contains_key(&key.0) {
            let row = self.store.read(key).await?;
            if let Some(row) = &row {
                row.validate_version()?;
                text(&row.revision)?;
                if let Some(expiry) = row.expires_at_ms {
                    validate_time(expiry)?;
                }
            }
            self.reads.insert(key.0.clone(), row);
        }
        let row = self.reads.get(&key.0).and_then(Option::as_ref);
        if row.is_some_and(|row| row.expires_at_ms.is_some_and(|expiry| expiry <= self.now)) {
            return Ok(None);
        }
        Ok(row)
    }
    pub(crate) async fn observe_revision(&mut self, key: &RecordKey) -> Result<(), GatewayError> {
        self.cached_record(key).await?;
        Ok(())
    }
    pub(crate) async fn read<T: DeserializeOwned>(
        &mut self,
        key: &RecordKey,
    ) -> Result<Option<T>, GatewayError> {
        self.cached_record(key)
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
    pub(crate) fn checks(&self) -> Vec<RevisionCheck> {
        self.reads
            .iter()
            .map(|(key, row)| RevisionCheck {
                key: RecordKey(key.clone()),
                expected_revision: row.as_ref().map(|row| row.revision.clone()),
            })
            .collect()
    }
    pub(crate) fn minimum_expiry(&self) -> Option<i64> {
        self.reads
            .values()
            .flatten()
            .filter_map(|row| row.expires_at_ms)
            .min()
    }
}
pub(crate) fn source_document(
    row: &StoredRecord,
) -> Result<gateway_source::source_json::SourceJson, GatewayError> {
    decode_source_control_record(row)
}
