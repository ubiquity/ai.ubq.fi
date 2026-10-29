//! Private complete live effect transaction; no caller-selected operation or readset is exposed.
use gateway_authority_contracts::{TokenSource, invalid, text, validate_time};
use gateway_core::{
    source_control::{decode_source_control_record, encode_source_control_record},
    state::{
        AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, RevisionCheck,
        StoredRecord,
    },
};
use gateway_kv_codec::key::RecordKey;
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde::{Serialize, de::DeserializeOwned};
use std::collections::{BTreeMap, BTreeSet};
fn decode_json_record<T: DeserializeOwned>(row: &StoredRecord) -> Result<T, GatewayError> {
    serde_json::from_value(row.json_value()?.clone())
        .map_err(|_| invalid("Malformed authority record."))
}
pub(crate) struct Transaction<'a, S, I> {
    store: &'a S,
    tokens: &'a I,
    pub(crate) now: i64,
    reads: BTreeMap<String, Option<StoredRecord>>,
    writes: BTreeMap<String, RecordMutation>,
}
impl<'a, S: AuthorityStore, I: TokenSource> Transaction<'a, S, I> {
    pub(crate) fn new(store: &'a S, tokens: &'a I, now: i64) -> Result<Self, GatewayError> {
        validate_time(now)?;
        Ok(Self {
            store,
            tokens,
            now,
            reads: BTreeMap::new(),
            writes: BTreeMap::new(),
        })
    }
    pub(crate) async fn cached_record(
        &mut self,
        key: &RecordKey,
    ) -> Result<Option<&StoredRecord>, GatewayError> {
        if !self.reads.contains_key(&key.0) {
            let record = self.store.read(key).await?;
            if let Some(value) = &record {
                value.validate_version()?;
                text(&value.revision)?;
                if let Some(expiry) = value.expires_at_ms {
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
            .map(decode_json_record)
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
    pub(crate) fn expiry(&self, key: &RecordKey) -> Option<i64> {
        self.reads
            .get(&key.0)
            .and_then(Option::as_ref)
            .and_then(|row| row.expires_at_ms)
    }
    pub(crate) fn expect(&self, key: &RecordKey, revision: &Option<String>) -> bool {
        self.reads.contains_key(&key.0) && &self.revision(key) == revision
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
    pub(crate) fn put<T: Serialize>(
        &mut self,
        key: &RecordKey,
        value: &T,
        expiry: Option<i64>,
    ) -> Result<(), GatewayError> {
        if !self.reads.contains_key(&key.0) {
            return Err(invalid(
                "A mutation is missing its strong-read precondition.",
            ));
        }
        if let Some(expiry) = expiry {
            validate_time(expiry)?;
        }
        let revision = self.tokens.next_token()?;
        text(&revision)?;
        if self
            .reads
            .values()
            .flatten()
            .any(|row| row.revision == revision)
        {
            return Err(invalid("Token source reused an existing revision."));
        }
        let record = StoredRecord::json(
            revision,
            serde_json::to_value(value).map_err(|_| invalid("Authority serialization failed."))?,
            expiry,
        );
        self.writes.insert(
            key.0.clone(),
            RecordMutation::Put {
                key: key.clone(),
                record,
            },
        );
        Ok(())
    }
    pub(crate) fn put_source_control<T: Serialize + ?Sized>(
        &mut self,
        key: &RecordKey,
        value: &T,
        expiry: Option<i64>,
    ) -> Result<(), GatewayError> {
        if !self.reads.contains_key(&key.0) {
            return Err(invalid(
                "A mutation is missing its strong-read precondition.",
            ));
        }
        if let Some(expiry) = expiry {
            validate_time(expiry)?;
        }
        let revision = self.tokens.next_token()?;
        text(&revision)?;
        if self
            .reads
            .values()
            .flatten()
            .any(|row| row.revision == revision)
        {
            return Err(invalid("Token source reused an existing revision."));
        }
        let document = SourceJson::from_serializable(value)?;
        let record = encode_source_control_record(revision, &document, expiry)?;
        self.writes.insert(
            key.0.clone(),
            RecordMutation::Put {
                key: key.clone(),
                record,
            },
        );
        Ok(())
    }
    pub(crate) fn delete(&mut self, key: &RecordKey) -> Result<(), GatewayError> {
        if !self.reads.contains_key(&key.0) {
            return Err(invalid(
                "A deletion is missing its strong-read precondition.",
            ));
        }
        self.writes
            .insert(key.0.clone(), RecordMutation::Delete { key: key.clone() });
        Ok(())
    }
    pub(crate) async fn commit(self) -> Result<bool, GatewayError> {
        let checks = self.checks();
        let mut revisions = BTreeSet::new();
        for mutation in self.writes.values() {
            if let RecordMutation::Put { record, .. } = mutation
                && !revisions.insert(record.revision.clone())
            {
                return Err(invalid("Token source duplicated a mutation revision."));
            }
        }
        Ok(self
            .store
            .commit(AtomicTransition {
                checks,
                mutations: self.writes.into_values().collect(),
            })
            .await?
            == CommitOutcome::Committed)
    }
}
