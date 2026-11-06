//! SQLite-backed coordination storage. Transactions never contain external I/O.

use gateway_authority_lifecycle::model::{Access, Identity};
use gateway_core::{
    error::GatewayError,
    identity::MAX_SAFE_INTEGER,
    query::{QueryEntry, QueryPage, QueryRequest, QueryStore},
    state::{
        AtomicTransition, AuthorityStore, CommitOutcome, PORTABLE_STATE_VERSION, RecordKey,
        StoredRecord,
    },
    tuple_key::TupleKey,
};
use gateway_worker_authority::transition::{
    PreparedMutation, PreparedTransition, prepare_transition, validate_key, validate_record,
};
use serde::Deserialize;
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
};
use worker::{SqlStorage, SqlStorageValue, Storage};

// The platform limits one SQL row to 2 MB; this is not a history retention limit.
const RECORD_COLUMNS: &str =
    "record_key, sort_key, schema_version, value_encoding, revision, payload, expires_at_ms";
// A page may end before its row limit, with a continuation, to bound wire memory.
const QUERY_PAGE_BYTE_BUDGET: usize = 8 * 1024 * 1024;

#[derive(Clone)]
pub struct SqliteAuthorityStore {
    pub(crate) storage: Rc<Storage>,
    access: Access,
    ingress: bool,
}

#[derive(Deserialize)]
struct TableName {
    name: String,
}

#[derive(Deserialize)]
struct FormatVersion {
    schema_version: u32,
    key_codec: String,
    value_codec: String,
}

impl SqliteAuthorityStore {
    pub async fn open(
        storage: Storage,
        identity: Identity,
        access: Access,
        ingress: bool,
    ) -> Result<Self, GatewayError> {
        let storage = Rc::new(storage);
        let transaction_storage = Rc::clone(&storage);
        storage.transaction(move |_| async move {
            let sql = transaction_storage.sql();
            let names: Vec<TableName> = sql.exec(
                "SELECT name FROM sqlite_master WHERE type = 'table' AND name IN ('uos_authority_metadata', 'uos_authority_records')",
                None,
            )?.to_array()?;
            let metadata = names.iter().any(|table| table.name == "uos_authority_metadata");
            let records = names.iter().any(|table| table.name == "uos_authority_records");
            match (metadata, records) {
                (false, false) => {
                    sql.exec("CREATE TABLE uos_authority_metadata (singleton INTEGER PRIMARY KEY CHECK (singleton = 1), schema_version INTEGER NOT NULL, key_codec TEXT NOT NULL, value_codec TEXT NOT NULL)", None)?;
                    sql.exec("CREATE TABLE uos_authority_records (record_key TEXT PRIMARY KEY NOT NULL, sort_key BLOB UNIQUE NOT NULL, schema_version INTEGER NOT NULL, value_encoding TEXT NOT NULL, revision TEXT NOT NULL, payload TEXT NOT NULL, expires_at_ms INTEGER)", None)?;
                    sql.exec("INSERT INTO uos_authority_metadata (singleton, schema_version, key_codec, value_codec) VALUES (1, ?, 'tuple-v1', 'explicit-value-v1')", Some(vec![SqlStorageValue::Integer(i64::from(PORTABLE_STATE_VERSION))]))?;
                }
                (true, true) => {}
                _ => return Err(worker::Error::RustError("Incomplete authority storage schema".to_owned())),
            }
            let versions: Vec<FormatVersion> = sql.exec("SELECT schema_version, key_codec, value_codec FROM uos_authority_metadata WHERE singleton = 1", None)?.to_array()?;
            if versions.len() != 1 || versions[0].schema_version != PORTABLE_STATE_VERSION || versions[0].key_codec != "tuple-v1" || versions[0].value_codec != "explicit-value-v1" {
                return Err(worker::Error::RustError("Unsupported authority storage schema".to_owned()));
            }
            // Existing table names alone cannot establish the expected columns/types.
            sql.exec("SELECT record_key, sort_key, schema_version, value_encoding, revision, payload, expires_at_ms FROM uos_authority_records LIMIT 0", None)?;
            Ok(())
        }).await.map_err(|_| unavailable("authority_storage_schema_unavailable"))?;
        storage
            .sync()
            .await
            .map_err(|_| unavailable("authority_storage_unavailable"))?;
        let gate_storage = Rc::clone(&storage);
        storage
            .transaction(move |_| async move {
                gateway_worker_drain::control::initialize(&gate_storage.sql(), &identity)
                    .map_err(|error| worker::Error::RustError(error.envelope.error.message))
            })
            .await
            .map_err(|_| unavailable("authority_control_schema_unavailable"))?;
        storage
            .sync()
            .await
            .map_err(|_| unavailable("authority_storage_unavailable"))?;
        Ok(Self {
            storage,
            access,
            ingress,
        })
    }
}

impl AuthorityStore for SqliteAuthorityStore {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        validate_key(key)?;
        gateway_worker_drain::control::authorize(
            &self.storage.sql(),
            &self.access,
            false,
            self.ingress,
        )?;
        read_record(&self.storage.sql(), key, now_ms()?)
    }

    async fn commit(&self, transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        let PreparedTransition { checks, mutations } = prepare_transition(transition)?;
        let outcome = Rc::new(Cell::new(CommitOutcome::Conflict));
        let transaction_outcome = Rc::clone(&outcome);
        let failure = Rc::new(RefCell::new(None));
        let transaction_failure = Rc::clone(&failure);
        let transaction_storage = Rc::clone(&self.storage);
        let access = self.access.clone();
        let ingress = self.ingress;
        let result = self.storage.transaction(move |_| async move {
            let apply = || -> Result<CommitOutcome, GatewayError> {
                let sql = transaction_storage.sql();
                gateway_worker_drain::control::authorize(&sql, &access, true, ingress)?;
                let now = now_ms()?;
                for (key, expected_revision) in &checks {
                    let current = read_record(&sql, &RecordKey(key.clone()), now)?;
                    if current.as_ref().map(|record| &record.revision) != expected_revision.as_ref() {
                        return Ok(CommitOutcome::Conflict);
                    }
                }
                // All checks run before the first write. Platform transaction rollback
                // also protects against an error in a later mutation.
                for mutation in &mutations {
                    match mutation {
                        PreparedMutation::Put { key, sort_key, record, payload, encoding } => {
                            let expiry = record.expires_at_ms.map_or(SqlStorageValue::Null, SqlStorageValue::Integer);
                            sql.exec(
                                "INSERT INTO uos_authority_records (record_key, sort_key, schema_version, value_encoding, revision, payload, expires_at_ms) VALUES (?, ?, ?, ?, ?, ?, ?) ON CONFLICT(record_key) DO UPDATE SET sort_key = excluded.sort_key, schema_version = excluded.schema_version, value_encoding = excluded.value_encoding, revision = excluded.revision, payload = excluded.payload, expires_at_ms = excluded.expires_at_ms",
                                Some(vec![SqlStorageValue::String(key.clone()), SqlStorageValue::Blob(sort_key.clone()), SqlStorageValue::Integer(i64::from(record.schema_version)), SqlStorageValue::String(encoding.clone()), SqlStorageValue::String(record.revision.clone()), SqlStorageValue::String(payload.clone()), expiry]),
                            ).map_err(|_| unavailable("authority_storage_unavailable"))?;
                        }
                        PreparedMutation::Delete { key } => {
                            sql.exec("DELETE FROM uos_authority_records WHERE record_key = ?", Some(vec![SqlStorageValue::String(key.clone())])).map_err(|_| unavailable("authority_storage_unavailable"))?;
                        }
                    }
                }
                Ok(CommitOutcome::Committed)
            };
            match apply() {
                Ok(value) => { transaction_outcome.set(value); Ok(()) }
                Err(error) => {
                    transaction_failure.replace(Some(error));
                    Err(worker::Error::RustError("Authority transition refused".to_owned()))
                }
            }
        }).await;
        if result.is_err() {
            return Err(failure
                .borrow_mut()
                .take()
                .unwrap_or_else(|| unavailable("authority_storage_unavailable")));
        }
        if outcome.get() == CommitOutcome::Committed {
            self.storage
                .sync()
                .await
                .map_err(|_| unavailable("authority_storage_unavailable"))?;
        }
        Ok(outcome.get())
    }
}

fn read_record(
    sql: &SqlStorage,
    key: &RecordKey,
    now: i64,
) -> Result<Option<StoredRecord>, GatewayError> {
    let query = format!("SELECT {RECORD_COLUMNS} FROM uos_authority_records WHERE record_key = ?");
    let cursor = sql
        .exec(&query, Some(vec![SqlStorageValue::String(key.0.clone())]))
        .map_err(|_| unavailable("authority_storage_unavailable"))?;
    let mut rows = cursor.raw();
    let Some(row) = rows.next() else {
        return Ok(None);
    };
    let (entry, _) = decode_row(&row.map_err(|_| invalid("Stored authority data is malformed."))?)?;
    if entry.key != *key || rows.next().is_some() {
        return Err(invalid("Authority keys are not unique."));
    }
    let record = entry.record;
    // Invalid or unsupported state remains an error even when its expiry is old.
    validate_record(&record)?;
    Ok(
        if record.expires_at_ms.is_some_and(|expiry| expiry <= now) {
            None
        } else {
            Some(record)
        },
    )
}

impl QueryStore for SqliteAuthorityStore {
    async fn query(&self, request: QueryRequest) -> Result<QueryPage, GatewayError> {
        gateway_worker_drain::control::authorize(
            &self.storage.sql(),
            &self.access,
            false,
            self.ingress,
        )?;
        let query = request.validate()?;
        let mut parameters = vec![
            SqlStorageValue::Blob(query.lower_inclusive.clone()),
            SqlStorageValue::Blob(query.upper_exclusive.clone()),
            SqlStorageValue::Integer(query.now_ms),
        ];
        let seek = if let Some(resume) = query.resume_key() {
            parameters.push(SqlStorageValue::Blob(resume.to_vec()));
            if query.reverse {
                " AND sort_key < ?"
            } else {
                " AND sort_key > ?"
            }
        } else {
            ""
        };
        parameters.push(SqlStorageValue::Integer(i64::from(query.limit) + 1));
        let order = if query.reverse { "DESC" } else { "ASC" };
        let sql = format!(
            "SELECT {RECORD_COLUMNS} FROM uos_authority_records WHERE sort_key >= ? AND sort_key < ? AND (expires_at_ms IS NULL OR expires_at_ms > ?){seek} ORDER BY sort_key {order} LIMIT ?"
        );
        let cursor = self
            .storage
            .sql()
            .exec(&sql, Some(parameters))
            .map_err(|_| unavailable("authority_storage_unavailable"))?;
        let mut entries = Vec::new();
        let mut bytes = 0_usize;
        let mut more = false;
        for row in cursor.raw() {
            let (entry, encoded_bytes) =
                decode_row(&row.map_err(|_| invalid("Stored authority data is malformed."))?)?;
            if !query.accepts_record(&entry.key, &entry.record)? {
                return Err(invalid(
                    "The authority SQL index does not match its tuple key.",
                ));
            }
            if entries.len() >= query.limit as usize
                || (!entries.is_empty()
                    && bytes.saturating_add(encoded_bytes) > QUERY_PAGE_BYTE_BUDGET)
            {
                more = true;
                break;
            }
            bytes = bytes.saturating_add(encoded_bytes);
            entries.push(entry);
        }
        let next_cursor = if more {
            Some(
                query.cursor_after(
                    &entries
                        .last()
                        .ok_or_else(|| invalid("The authority query made no progress."))?
                        .key,
                )?,
            )
        } else {
            None
        };
        Ok(QueryPage {
            entries,
            next_cursor,
        })
    }
}

fn decode_row(row: &[SqlStorageValue]) -> Result<(QueryEntry, usize), GatewayError> {
    let [
        SqlStorageValue::String(key),
        SqlStorageValue::Blob(sort_key),
        SqlStorageValue::Integer(version),
        SqlStorageValue::String(encoding),
        SqlStorageValue::String(revision),
        SqlStorageValue::String(payload),
        expiry,
    ] = row
    else {
        return Err(invalid("Stored authority data has invalid SQL types."));
    };
    let key = RecordKey(key.clone());
    validate_key(&key)?;
    if TupleKey::from_record_key(&key)?.sort_bytes() != sort_key {
        return Err(invalid(
            "The authority SQL index does not match its tuple key.",
        ));
    }
    let expires_at_ms = match expiry {
        SqlStorageValue::Null => None,
        SqlStorageValue::Integer(value) => Some(*value),
        _ => return Err(invalid("Stored authority expiry is malformed.")),
    };
    let record = StoredRecord {
        schema_version: u32::try_from(*version)
            .map_err(|_| invalid("Stored authority version is malformed."))?,
        value_encoding: serde_json::from_value(serde_json::Value::String(encoding.clone()))
            .map_err(|_| invalid("Stored authority value encoding is unsupported."))?,
        revision: revision.clone(),
        value: serde_json::from_str(payload)
            .map_err(|_| invalid("Stored authority data is malformed."))?,
        expires_at_ms,
    };
    validate_record(&record)?;
    let encoded_bytes = key
        .0
        .len()
        .saturating_add(revision.len())
        .saturating_add(payload.len())
        .saturating_add(encoding.len())
        .saturating_add(128);
    Ok((QueryEntry { key, record }, encoded_bytes))
}

pub fn now_ms() -> Result<i64, GatewayError> {
    let now = worker::js_sys::Date::now();
    if !now.is_finite() || now.fract() != 0.0 || !(0.0..=MAX_SAFE_INTEGER as f64).contains(&now) {
        return Err(unavailable("authority_clock_unavailable"));
    }
    Ok(now as i64)
}

fn invalid(message: &str) -> GatewayError {
    GatewayError::unavailable(message, "authority_state_invalid")
}

fn unavailable(code: &str) -> GatewayError {
    GatewayError::unavailable("The authority storage is unavailable.", code)
}

pub fn read_control_record(
    sql: &SqlStorage,
    key: &RecordKey,
) -> Result<Option<StoredRecord>, GatewayError> {
    read_record(sql, key, 0)
}
