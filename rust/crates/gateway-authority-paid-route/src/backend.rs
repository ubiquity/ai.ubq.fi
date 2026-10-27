//! Private fixed operation cache; no transaction or raw store accessor is public.
use gateway_authority_contracts::{invalid, text, validate_time};
use gateway_core::source_control::decode_source_control_record;
use gateway_core::{state::AtomicTransition, state::AuthorityStore, state::CommitOutcome};
use gateway_core::{state::RevisionCheck, state::StoredRecord};
use gateway_kv_codec::key::RecordKey;
use gateway_source::error::GatewayError;
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

    /// Establish only the existing cached metadata/revision precondition. This
    /// observes no payload and cannot replace a named family's validation.
    pub(crate) async fn observe_revision(&mut self, key: &RecordKey) -> Result<(), GatewayError> {
        self.cached_record(key).await?;
        Ok(())
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

    pub(crate) fn routing_read_store(&mut self) -> RoutingReadStore<'_, S> {
        RoutingReadStore {
            store: self.store,
            reads: std::cell::RefCell::new(&mut self.reads),
        }
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
