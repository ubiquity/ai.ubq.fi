use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, RevisionCheck, StoredRecord,
};
use gateway_kv_codec::key::RecordKey;
use gateway_source::error::GatewayError;
use serde::{Serialize, de::DeserializeOwned};

pub fn decode<T: DeserializeOwned>(
    record: Option<&StoredRecord>,
    now: i64,
) -> Result<Option<T>, GatewayError> {
    let Some(record) = record else {
        return Ok(None);
    };
    record.validate_version()?;
    if record.expires_at_ms.is_some_and(|expiry| expiry <= now) {
        return Ok(None);
    }
    serde_json::from_value(record.json_value()?.clone())
        .map(Some)
        .map_err(|_| {
            GatewayError::unavailable("Authentication state is corrupt", "auth_state_corrupt")
        })
}

pub fn check(key: &RecordKey, record: Option<&StoredRecord>) -> RevisionCheck {
    RevisionCheck {
        key: key.clone(),
        expected_revision: record.map(|row| row.revision.clone()),
    }
}

pub fn put<T: Serialize>(
    key: RecordKey,
    value: &T,
    expiry: Option<i64>,
    revision: &str,
) -> Result<RecordMutation, GatewayError> {
    let value = serde_json::to_value(value).map_err(|_| {
        GatewayError::unavailable(
            "Authentication state cannot be encoded",
            "auth_state_corrupt",
        )
    })?;
    Ok(RecordMutation::Put {
        key,
        record: StoredRecord::json(revision, value, expiry),
    })
}

pub async fn commit<S: AuthorityStore>(
    store: &S,
    checks: Vec<RevisionCheck>,
    mutations: Vec<RecordMutation>,
) -> Result<bool, GatewayError> {
    Ok(store.commit(AtomicTransition { checks, mutations }).await? == CommitOutcome::Committed)
}
