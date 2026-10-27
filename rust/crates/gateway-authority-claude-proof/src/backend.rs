//! Private fixed Claude source reads and original source-fence CAS; never a public transaction.
use gateway_authority_contracts::{invalid, text, validate_time};
use gateway_authority_keys as keys;
use gateway_core::{
    source_control::decode_source_control_record, state::AtomicTransition, state::AuthorityStore,
    state::CommitOutcome, state::RevisionCheck, state::StoredRecord,
};
use gateway_kv_codec::{key::RecordKey, value::ValueEncoding};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde::de::DeserializeOwned;
use std::collections::BTreeMap;

pub(crate) struct ReadTransaction<'a, S> {
    store: &'a S,
    pub(crate) now: i64,
    reads: BTreeMap<String, Option<StoredRecord>>,
}
impl<'a, S: AuthorityStore> ReadTransaction<'a, S> {
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
    pub(crate) async fn read_claude_quota(
        &mut self,
        account_id: &str,
        cohort_id: &str,
    ) -> Result<Option<SourceJson>, GatewayError> {
        let key = keys::claude_quota(account_id, cohort_id);
        self.cached_record(&key)
            .await?
            .map(|row| match row.value_encoding {
                ValueEncoding::ProfileV2 => {
                    Ok(crate::quota_proof_metadata(&row.profile_v2_value()?))
                }
                _ => SourceJson::from_serializable(row.json_value()?),
            })
            .transpose()
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
    pub(crate) async fn commit(self) -> Result<bool, GatewayError> {
        Ok(self
            .store
            .commit(AtomicTransition {
                checks: self.checks(),
                mutations: vec![],
            })
            .await?
            == CommitOutcome::Committed)
    }
}
