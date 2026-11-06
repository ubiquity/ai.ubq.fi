//! A verified private export archive as a read-only query store. Each line is
//! one exact export page body; order, uniqueness, hash and count are checked.
use crate::refused;
use gateway_core::error::GatewayError;
use gateway_core::query::{QueryEntry, QueryPage, QueryRequest, QueryStore};
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, StoredRecord,
};
use gateway_core::tuple_key::TupleKey;
use gateway_migration::staged::CanonicalRecord;
use gateway_state_transfer::validate_record;
use std::collections::BTreeMap;

pub struct ArchiveStore {
    rows: BTreeMap<Vec<u8>, (RecordKey, StoredRecord)>,
}

impl ArchiveStore {
    /// `bytes` are the complete archive file; nothing is trusted before the
    /// whole-file hash and the exact record count match the export receipt.
    pub fn from_bytes(bytes: &[u8], sha256: &str, records: usize) -> Result<Self, GatewayError> {
        if gateway_auth::crypto::sha256_hex(bytes) != sha256 {
            return Err(refused("Export archive hash differs from its receipt"));
        }
        let mut rows = BTreeMap::new();
        let mut previous: Option<Vec<u8>> = None;
        for line in bytes.split(|byte| *byte == b'\n') {
            if line.is_empty() {
                continue;
            }
            let page: Vec<CanonicalRecord> = serde_json::from_slice(line)
                .map_err(|_| refused("Export archive page is malformed"))?;
            for item in page {
                validate_record(&item.record)?;
                let sort = TupleKey::from_record_key(&item.key)?.sort_bytes().to_vec();
                if previous.as_ref().is_some_and(|last| last >= &sort) {
                    return Err(refused("Export archive keys are not strictly ordered"));
                }
                previous = Some(sort.clone());
                rows.insert(sort, (item.key, item.record));
            }
        }
        if !bytes.is_empty() && bytes.last() != Some(&b'\n') {
            return Err(refused("Export archive is truncated"));
        }
        if rows.len() != records {
            return Err(refused("Export archive count differs from its receipt"));
        }
        Ok(Self { rows })
    }
    pub fn len(&self) -> usize {
        self.rows.len()
    }
    pub fn is_empty(&self) -> bool {
        self.rows.is_empty()
    }
}

impl AuthorityStore for ArchiveStore {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        let sort = TupleKey::from_record_key(key)?.sort_bytes().to_vec();
        Ok(self.rows.get(&sort).map(|(_, record)| record.clone()))
    }
    async fn commit(&self, _transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        Err(refused("An export archive is read-only evidence"))
    }
}

impl QueryStore for ArchiveStore {
    async fn query(&self, request: QueryRequest) -> Result<QueryPage, GatewayError> {
        let validated = request.validate()?;
        let range = self
            .rows
            .range(validated.lower_inclusive.clone()..validated.upper_exclusive.clone());
        let ordered: Box<dyn Iterator<Item = _>> = if validated.reverse {
            Box::new(range.rev())
        } else {
            Box::new(range)
        };
        let mut entries = Vec::new();
        let mut more = false;
        for (_, (key, record)) in ordered {
            if !validated.accepts_record(key, record)? {
                continue;
            }
            if entries.len() == validated.limit as usize {
                more = true;
                break;
            }
            entries.push(QueryEntry {
                key: key.clone(),
                record: record.clone(),
            });
        }
        let next_cursor = match (more, entries.last()) {
            (true, Some(last)) => Some(validated.cursor_after(&last.key)?),
            _ => None,
        };
        Ok(QueryPage {
            entries,
            next_cursor,
        })
    }
}
