//! Bounded resumable source accounting bootstrap. Shared state contains owner ids only.
use crate::{ObservabilityRuntime, store};
use gateway_core::{
    query::QueryRequest, query::QuerySelector, query::QueryStore, state::RecordMutation,
};
use gateway_observability_contracts::{
    data, query, query::ObservationRead, types::ObservabilityHost,
};
use gateway_observability_replay_model::{
    records::read_json_record, replay_model, replay_model as model,
};
use gateway_observability_replay_records::accounting as schema;
use gateway_source::error::GatewayError;
use std::{collections::BTreeMap, collections::BTreeSet};
const PREFIXES: [&str; 7] = [
    "accounting",
    "manifest",
    "request",
    "evicted",
    "reservation",
    "chunk",
    "dedupe",
];
#[derive(Default)]
pub(crate) struct OwnerIndex {
    pub sweep: String,
    pub phase: usize,
    pub cursor: Option<String>,
    pub owners: BTreeSet<String>,
}
impl<S: QueryStore, H: ObservabilityHost> ObservabilityRuntime<S, H> {
    async fn advance_replay_owner_index(&self, sweep: &str, at: i64) -> Result<bool, GatewayError> {
        {
            let mut index = self.replay_bootstrap.borrow_mut();
            if index.sweep != sweep {
                *index = OwnerIndex {
                    sweep: sweep.into(),
                    ..Default::default()
                };
            }
        }
        let (phase, cursor) = {
            let index = self.replay_bootstrap.borrow();
            (index.phase, index.cursor.clone())
        };
        if phase >= 2 {
            return Ok(true);
        }
        let kind = if phase == 0 { "accounting" } else { "manifest" };
        let page = self
            .store
            .query(QueryRequest {
                selector: QuerySelector::Prefix {
                    prefix: model::key(kind),
                    start: None,
                    end: None,
                },
                reverse: false,
                limit: 64,
                cursor,
                now_ms: at,
            })
            .await?;
        let mut owners = Vec::new();
        for entry in page.entries {
            let owner = if phase == 0 {
                schema::accounting(&entry.record, &entry.key)?.capture_id
            } else {
                let manifest = model::Manifest::from_source(&read_json_record(&entry.record)?)?;
                if !manifest.matches_key(&entry.key)? {
                    return Err(data::error("Replay manifest owner key is invalid"));
                }
                manifest.capture_id
            };
            owners.push(owner)
        }
        let mut index = self.replay_bootstrap.borrow_mut();
        if index.sweep != sweep {
            return Ok(false);
        }
        index.owners.extend(owners);
        if let Some(cursor) = page.next_cursor {
            index.cursor = Some(cursor)
        } else {
            index.phase += 1;
            index.cursor = None
        }
        Ok(index.phase >= 2)
    }
    pub(crate) async fn bootstrap_replays(
        &self,
        limits: schema::RetentionLimits,
        at: i64,
    ) -> Result<bool, GatewayError> {
        for _ in 0..schema::ATTEMPTS {
            let (mut ledger, previous) = schema::read_ledger(self.store.as_ref(), limits).await?;
            if ledger.ready() {
                return Ok(true);
            }
            if ledger.accounting_error.is_some() {
                return Ok(false);
            }
            if ledger.bootstrap_sweep.is_none() {
                ledger.bootstrap_sweep = Some(data::uuid(self.host.as_ref())?);
            }
            let sweep = ledger.bootstrap_sweep.clone().expect("created sweep");
            let (mut phase, mut cursor) = match ledger.bootstrap_cursor.as_deref() {
                None => (0, None),
                Some(value) => {
                    let (phase, cursor) = value
                        .split_once('|')
                        .ok_or_else(|| data::error("Replay bootstrap cursor is invalid"))?;
                    (
                        phase
                            .parse::<usize>()
                            .map_err(|_| data::error("Replay bootstrap phase is invalid"))?,
                        (!cursor.is_empty()).then(|| cursor.to_owned()),
                    )
                }
            };
            if phase > PREFIXES.len() {
                return Err(data::error("Replay bootstrap phase is out of range"));
            }
            if phase == 5 && !self.advance_replay_owner_index(&sweep, at).await? {
                if previous.is_none() {
                    store::commit(
                        self.store.as_ref(),
                        vec![data::check(&model::key("budget"), None)],
                        vec![store::ledger_mutation(self.host.as_ref(), None, &ledger)?],
                    )
                    .await?;
                }
                return Ok(false);
            }
            let mut checks = vec![data::check(&model::key("budget"), previous.as_ref())];
            let mut mutations = Vec::new();
            let mut scanned = 0;
            let mut pending = BTreeMap::<String, String>::new();
            while phase < PREFIXES.len() && scanned < 4 {
                if phase == 5 && !self.advance_replay_owner_index(&sweep, at).await? {
                    break;
                }
                let kind = PREFIXES[phase];
                let page = self
                    .store
                    .query(QueryRequest {
                        selector: QuerySelector::Prefix {
                            prefix: model::key(kind),
                            start: None,
                            end: None,
                        },
                        reverse: false,
                        limit: (4 - scanned) as u32,
                        cursor: cursor.clone(),
                        now_ms: at,
                    })
                    .await?;
                for entry in &page.entries {
                    scanned += 1;
                    let result: Result<(), GatewayError> = async {
                        match kind {
                            "accounting" => {
                                let row = schema::accounting(&entry.record, &entry.key)?;
                                if matches!(
                                    row.state,
                                    schema::AccountingState::Stored
                                        | schema::AccountingState::Evicting
                                ) {
                                    ledger.stored_bytes += row.bytes;
                                    ledger.records += 1.0
                                } else {
                                    ledger.reserved_bytes += row.bytes
                                }
                            }
                            "manifest" => {
                                let manifest = model::Manifest::from_source(&read_json_record(
                                    &entry.record,
                                )?)?;
                                if !manifest.matches_key(&entry.key)? {
                                    return Err(data::error(
                                        "Replay bootstrap manifest key mismatch",
                                    ));
                                }
                                let key = schema::accounting_key(
                                    manifest.captured_at_ms,
                                    &manifest.capture_id,
                                );
                                let existing = self.store.read(&key).await?;
                                if let Some(row) = existing {
                                    if schema::accounting(&row, &key)?.fingerprint
                                        != manifest.fingerprint
                                    {
                                        return Err(data::error(
                                            "Replay bootstrap owner mismatch",
                                        ));
                                    }
                                } else if let Some(fingerprint) = pending.get(&key.0) {
                                    if *fingerprint != manifest.fingerprint {
                                        return Err(data::error(
                                            "Replay bootstrap pending owner mismatch",
                                        ));
                                    }
                                } else {
                                    let bytes = manifest.stored_bytes.unwrap_or_else(|| {
                                        schema::charge(manifest.ciphertext_bytes as usize, 262_144)
                                            + schema::OVERHEAD
                                    });
                                    let row = schema::AccountingData {
                                        version: 1.0,
                                        capture_id: manifest.capture_id,
                                        request_id: manifest.request_id.unwrap_or_default(),
                                        fingerprint: manifest.fingerprint,
                                        bytes,
                                        state: schema::AccountingState::Stored,
                                        fence: 1.0,
                                        created_at_ms: manifest.captured_at_ms,
                                        expires_at_ms: manifest.expires_at_ms,
                                        status_expires_at_ms: manifest.captured_at_ms
                                            + model::STATUS_TTL as f64,
                                        stage: 0.0,
                                        claim_kind: None,
                                    };
                                    checks.push(data::check(&entry.key, Some(&entry.record)));
                                    checks.push(data::check(&key, None));
                                    mutations.push(store::replay_mutation(
                                        self.host.as_ref(),
                                        key.clone(),
                                        None,
                                        &row,
                                        None,
                                    )?);
                                    pending.insert(key.0, row.fingerprint);
                                    ledger.stored_bytes += bytes;
                                    ledger.records += 1.0;
                                }
                            }
                            "request" => {
                                replay_model::StatusDocument::parse(&read_json_record(
                                    &entry.record,
                                )?)?;
                                ledger.status_records += 1.0;
                                ledger.metadata_bytes +=
                                    schema::row_metadata_charge(Some(&entry.record))?
                            }
                            "evicted" => {
                                let source = gateway_observability_contracts::source_object::SourceObject::parse(
                                    &read_json_record(&entry.record)?,
                                )?;
                                if !["fingerprint", "reason"].iter().all(|f| {
                                    source.get(f).is_some_and(|v| {
                                        v.deserialize::<gateway_source::js_string::JsString16>()
                                            .is_ok()
                                    })
                                }) {
                                    return Err(data::error("Replay tombstone is invalid"));
                                }
                                ledger.status_records += 1.0;
                                ledger.metadata_bytes +=
                                    schema::row_metadata_charge(Some(&entry.record))?
                            }
                            "reservation" => {
                                let value = entry.record.json_value()?;
                                if value["version"] != 1
                                    || !value["capture_id"].is_string()
                                    || !matches!(
                                        value["state"].as_str(),
                                        Some("published" | "reserved" | "revoked")
                                    )
                                    || !value["bytes"].as_f64().is_some_and(model::counter)
                                {
                                    return Err(data::error(
                                        "Replay legacy reservation is invalid",
                                    ));
                                }
                                if value["state"] != "published" {
                                    let id = value["capture_id"].as_str().expect("validated id");
                                    let chunks = query::scan(self.store.as_ref(), ObservationRead::ReplayChunks(id), false, at, 513)
                                    .await?;
                                    for chunk in chunks.iter().take(512) {
                                        let deleted = store::commit(
                                            self.store.as_ref(),
                                            vec![
                                                data::check(&chunk.key, Some(&chunk.record)),
                                                data::check(&entry.key, Some(&entry.record)),
                                                data::check(
                                                    &model::key("budget"),
                                                    previous.as_ref(),
                                                ),
                                            ],
                                            vec![RecordMutation::Delete {
                                                key: chunk.key.clone(),
                                            }],
                                        )
                                        .await?;
                                        if !deleted {
                                            return Err(data::error(
                                                "Replay legacy cleanup fence changed",
                                            ));
                                        }
                                    }
                                    if chunks.len() > 512 {
                                        return Err(GatewayError::unavailable(
                                            "Replay legacy cleanup needs another bounded pass",
                                            "replay_bootstrap_pending",
                                        ));
                                    }
                                }
                                checks.push(data::check(&entry.key, Some(&entry.record)));
                                mutations.push(RecordMutation::Delete {
                                    key: entry.key.clone(),
                                });
                            }
                            "chunk" => {
                                let parts = data::parts(&entry.key)?;
                                let id = parts
                                    .get(4)
                                    .and_then(serde_json::Value::as_str)
                                    .filter(|_| {
                                        parts.len() == 6
                                            && parts[5].as_f64().is_some_and(model::counter)
                                    })
                                    .ok_or_else(|| data::error("Replay chunk key is invalid"))?;
                                if !self.replay_bootstrap.borrow().owners.contains(id) {
                                    checks.push(data::check(&entry.key, Some(&entry.record)));
                                    mutations.push(RecordMutation::Delete {
                                        key: entry.key.clone(),
                                    });
                                }
                            }
                            "dedupe" => {
                                let source = gateway_observability_contracts::source_object::SourceObject::parse(
                                    &read_json_record(&entry.record)?,
                                )?;
                                if !source
                                    .get("manifest_key")
                                    .is_some_and(|v| v.array_items().is_ok_and(|a| a.len() == 7))
                                {
                                    return Err(data::error("Replay dedupe record is invalid"));
                                }
                            }
                            _ => unreachable!(),
                        }
                        Ok(())
                    }
                    .await;
                    if let Err(error) = result {
                        if error.envelope.error.code.as_deref() == Some("replay_bootstrap_pending")
                        {
                            return Ok(false);
                        }
                        ledger.accounting_error = Some("invalid_capture_owned_record".into());
                    }
                }
                if let Some(next) = page.next_cursor {
                    if cursor.as_ref() == Some(&next) {
                        return Err(data::error("Replay bootstrap cursor did not advance"));
                    }
                    cursor = Some(next);
                } else {
                    phase += 1;
                    cursor = None
                }
                if ledger.accounting_error.is_some() {
                    break;
                }
            }
            ledger.bootstrap_complete =
                phase == PREFIXES.len() && ledger.accounting_error.is_none();
            ledger.bootstrap_cursor = (!ledger.bootstrap_complete)
                .then(|| format!("{phase}|{}", cursor.unwrap_or_default()));
            ledger.over_budget = ledger.stored_bytes + ledger.reserved_bytes > limits.payload();
            mutations.insert(
                0,
                store::ledger_mutation(self.host.as_ref(), previous.as_ref(), &ledger)?,
            );
            if store::commit(self.store.as_ref(), checks, mutations).await? {
                return Ok(ledger.ready());
            }
        }
        Ok(false)
    }
}
