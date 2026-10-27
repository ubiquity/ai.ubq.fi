//! Private snapshot cache. Closed family readers keep actual revisions, including expired rows.
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
    async fn cached_record(
        &mut self,
        key: &RecordKey,
    ) -> Result<Option<&StoredRecord>, GatewayError> {
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
        let raw = self.reads.get(&key.0).and_then(Option::as_ref);
        if raw.is_some_and(|row| row.expires_at_ms.is_some_and(|expiry| expiry <= self.now)) {
            return Ok(None);
        }
        Ok(raw)
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
    pub(crate) async fn read_source_control<T: DeserializeOwned>(
        &mut self,
        key: &RecordKey,
    ) -> Result<Option<T>, GatewayError> {
        self.cached_record(key)
            .await?
            .map(|row| {
                decode_source_control_record(row)?
                    .deserialize()
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
            .map(|(key, record)| RevisionCheck {
                key: RecordKey(key.clone()),
                expected_revision: record.as_ref().map(|row| row.revision.clone()),
            })
            .collect()
    }
}
