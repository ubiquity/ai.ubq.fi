//! Private finite-operation staging; actual metadata, expiry, strong reads and final CAS remain co-owned.
use gateway_authority_contracts::{TokenSource, invalid, text, validate_time};
use gateway_core::source_control::decode_source_control_record;
use gateway_core::state::StoredRecord;
use gateway_core::{state::AtomicTransition, state::AuthorityStore, state::CommitOutcome};
use gateway_core::{state::RecordMutation, state::RevisionCheck};
use gateway_kv_codec::key::RecordKey;
use gateway_source::error::GatewayError;
use serde::{Serialize, de::DeserializeOwned};
use std::{collections::BTreeMap, collections::BTreeSet};
/// Only the named local release adapter observes this private read origin.
pub(crate) enum ApiReleaseReadError {
    Backend(GatewayError),
    Record(GatewayError),
}
impl ApiReleaseReadError {
    fn into_error(self) -> GatewayError {
        match self {
            Self::Backend(error) | Self::Record(error) => error,
        }
    }
}
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
    async fn cached_record(
        &mut self,
        key: &RecordKey,
    ) -> Result<Option<&StoredRecord>, GatewayError> {
        self.cached_record_classified(key)
            .await
            .map_err(ApiReleaseReadError::into_error)
    }
    async fn cached_record_classified(
        &mut self,
        key: &RecordKey,
    ) -> Result<Option<&StoredRecord>, ApiReleaseReadError> {
        if !self.reads.contains_key(&key.0) {
            let record = self
                .store
                .read(key)
                .await
                .map_err(ApiReleaseReadError::Backend)?;
            if let Some(value) = &record {
                value
                    .validate_version()
                    .map_err(ApiReleaseReadError::Record)?;
                text(&value.revision).map_err(ApiReleaseReadError::Record)?;
                if let Some(expiry) = value.expires_at_ms {
                    validate_time(expiry).map_err(ApiReleaseReadError::Record)?;
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

    pub async fn read<T: DeserializeOwned>(
        &mut self,
        key: &RecordKey,
    ) -> Result<Option<T>, GatewayError> {
        self.cached_record(key)
            .await?
            .map(decode_json_record)
            .transpose()
    }

    pub(crate) fn routing_read_store(&mut self) -> RoutingReadStore<'_, S> {
        RoutingReadStore {
            store: self.store,
            reads: std::cell::RefCell::new(&mut self.reads),
        }
    }

    /// Explicit lossless opt-in for named control families. The caller still owns
    /// the exact key, DTO/version and currentness checks; data grants no authority.
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

    pub fn revision(&self, key: &RecordKey) -> Option<String> {
        self.reads
            .get(&key.0)
            .and_then(Option::as_ref)
            .map(|row| row.revision.clone())
    }

    pub fn expect(&self, key: &RecordKey, revision: &Option<String>) -> bool {
        self.reads.contains_key(&key.0) && &self.revision(key) == revision
    }

    pub fn checks(&self) -> Vec<RevisionCheck> {
        self.reads
            .iter()
            .map(|(key, record)| RevisionCheck {
                key: RecordKey(key.clone()),
                expected_revision: record.as_ref().map(|row| row.revision.clone()),
            })
            .collect()
    }

    pub fn put<T: Serialize>(
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

    pub async fn commit(self) -> Result<bool, GatewayError> {
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
/// Private fixed-reader bridge. No mutation or raw store reference crosses this boundary.
pub(crate) struct RoutingReadStore<'a, S> {
    store: &'a S,
    reads: std::cell::RefCell<&'a mut BTreeMap<String, Option<StoredRecord>>>,
}
impl<S: AuthorityStore> AuthorityStore for RoutingReadStore<'_, S> {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        let missing = !self.reads.borrow().contains_key(&key.0);
        if missing {
            let record = self.store.read(key).await?;
            if let Some(row) = &record {
                row.validate_version()?;
                text(&row.revision)?;
                if let Some(expiry) = row.expires_at_ms {
                    validate_time(expiry)?;
                }
            }
            self.reads.borrow_mut().insert(key.0.clone(), record);
        }
        Ok(self.reads.borrow().get(&key.0).cloned().flatten())
    }
    async fn commit(&self, _transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        Err(invalid(
            "Routing evidence reader has no mutation authority.",
        ))
    }
}
