//! Shared validation for the private business-record transition; no storage effects.
use gateway_core::{
    error::GatewayError,
    identity::MAX_SAFE_INTEGER,
    state::{AtomicTransition, RecordKey, RecordMutation, StoredRecord},
    tuple_key::TupleKey,
};
use std::collections::{BTreeMap, BTreeSet};
const MAX_REVISION_BYTES: usize = 4096;
const MAX_SQL_ROW_BYTES: usize = 2_000_000;
pub enum PreparedMutation {
    Put {
        key: String,
        sort_key: Vec<u8>,
        record: StoredRecord,
        payload: String,
        encoding: String,
    },
    Delete {
        key: String,
    },
}

pub struct PreparedTransition {
    pub checks: BTreeMap<String, Option<String>>,
    pub mutations: Vec<PreparedMutation>,
}

pub fn prepare_transition(
    transition: AtomicTransition,
) -> Result<PreparedTransition, GatewayError> {
    let mut checks = BTreeMap::new();
    for check in transition.checks {
        validate_key(&check.key)?;
        if let Some(revision) = &check.expected_revision {
            validate_revision(revision)?;
        }
        if checks
            .insert(check.key.0, check.expected_revision)
            .is_some()
        {
            return Err(invalid(
                "An authority transition contains duplicate checks.",
            ));
        }
    }
    let mut keys = BTreeSet::new();
    let mut prepared = Vec::with_capacity(transition.mutations.len());
    for mutation in transition.mutations {
        let key = match &mutation {
            RecordMutation::Put { key, .. } | RecordMutation::Delete { key } => key,
        };
        validate_key(key)?;
        if !keys.insert(key.0.clone()) {
            return Err(invalid(
                "An authority transition contains duplicate mutation keys.",
            ));
        }
        match mutation {
            RecordMutation::Put { key, record } => {
                validate_record(&record)?;
                let sort_key = TupleKey::from_record_key(&key)?.sort_bytes().to_vec();
                let payload = serde_json::to_string(&record.value)
                    .map_err(|_| invalid("Authority data cannot be encoded."))?;
                let encoding = serde_json::to_value(record.value_encoding)
                    .map_err(|_| invalid("Authority value encoding is invalid."))?
                    .as_str()
                    .ok_or_else(|| invalid("Authority value encoding is invalid."))?
                    .to_owned();
                let bytes = key
                    .0
                    .len()
                    .checked_add(sort_key.len())
                    .and_then(|size| size.checked_add(record.revision.len()))
                    .and_then(|size| size.checked_add(payload.len()))
                    .and_then(|size| size.checked_add(encoding.len()))
                    .and_then(|size| size.checked_add(48));
                if bytes.is_none_or(|size| size > MAX_SQL_ROW_BYTES) {
                    return Err(invalid(
                        "Authority data exceeds the platform's SQL row limit.",
                    ));
                }
                prepared.push(PreparedMutation::Put {
                    key: key.0,
                    sort_key,
                    record,
                    payload,
                    encoding,
                });
            }
            RecordMutation::Delete { key } => {
                prepared.push(PreparedMutation::Delete { key: key.0 })
            }
        }
    }
    Ok(PreparedTransition {
        checks,
        mutations: prepared,
    })
}

pub fn validate_key(key: &RecordKey) -> Result<(), GatewayError> {
    TupleKey::from_record_key(key)?.validate_stored_key()?;
    if key.0.len() > MAX_SQL_ROW_BYTES {
        return Err(invalid(
            "The authority key exceeds the platform's SQL row limit.",
        ));
    }
    Ok(())
}

fn validate_revision(revision: &str) -> Result<(), GatewayError> {
    if revision.is_empty() || revision.len() > MAX_REVISION_BYTES {
        return Err(invalid("The authority revision is invalid."));
    }
    Ok(())
}

pub fn validate_record(record: &StoredRecord) -> Result<(), GatewayError> {
    record.validate_version()?;
    validate_revision(&record.revision)?;
    if record
        .expires_at_ms
        .is_some_and(|expiry| !(0..=MAX_SAFE_INTEGER).contains(&expiry))
    {
        return Err(invalid("The authority absolute expiry is invalid."));
    }
    Ok(())
}

fn invalid(message: &str) -> GatewayError {
    GatewayError::unavailable(message, "authority_state_invalid")
}
