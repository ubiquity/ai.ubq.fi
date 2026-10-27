//! Private capacity record staging; never a public transaction or callback-selected write surface.
use gateway_authority_contracts::{TokenSource, invalid, text, validate_time};
use gateway_core::{
    error::GatewayError,
    state::{
        AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, RecordMutation, RevisionCheck,
        StoredRecord,
    },
};
use serde::{Serialize, de::DeserializeOwned};
use std::collections::{BTreeMap, BTreeSet};

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
    now: i64,
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

    pub(crate) fn routing_read_store(&mut self) -> RoutingReadStore<'_, S> {
        RoutingReadStore {
            store: self.store,
            reads: std::cell::RefCell::new(&mut self.reads),
        }
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

    pub fn post_checks(&self) -> Vec<RevisionCheck> {
        self.checks()
            .into_iter()
            .map(|mut check| {
                if let Some(mutation) = self.writes.get(&check.key.0) {
                    check.expected_revision = match mutation {
                        RecordMutation::Put { record, .. } => Some(record.revision.clone()),
                        RecordMutation::Delete { .. } => None,
                    };
                }
                check
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
    async fn commit(&self, _transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        Err(invalid(
            "Routing evidence reader has no mutation authority.",
        ))
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
