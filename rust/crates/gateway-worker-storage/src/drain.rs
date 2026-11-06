//! Dedicated SQL control tables. Their rows never enter the business tuple inventory.
use gateway_authority_provisioning::{model::*, policy};
use gateway_core::error::GatewayError;
use worker::{SqlStorage, SqlStorageValue as Sql, Storage};

fn failure() -> GatewayError {
    policy::refused("Durable writer control is invalid or unavailable")
}
fn encode<T: serde::Serialize>(value: &T) -> Result<String, GatewayError> {
    serde_json::to_string(value).map_err(|_| failure())
}

pub fn gate(sql: &SqlStorage) -> Result<Gate, GatewayError> {
    let mut rows = sql
        .exec("SELECT payload FROM uos_drain_gate WHERE singleton=1", None)
        .map_err(|_| failure())?
        .raw();
    let row = rows
        .next()
        .transpose()
        .map_err(|_| failure())?
        .ok_or_else(failure)?;
    let [Sql::String(payload)] = row.as_slice() else {
        return Err(failure());
    };
    let value: Gate = serde_json::from_str(payload).map_err(|_| failure())?;
    if value.v != 1 || rows.next().is_some() {
        return Err(failure());
    }
    policy::identity(&value.identity)?;
    Ok(value)
}

pub fn put_gate(sql: &SqlStorage, value: &Gate) -> Result<(), GatewayError> {
    sql.exec("INSERT INTO uos_drain_gate(singleton,payload) VALUES(1,?) ON CONFLICT(singleton) DO UPDATE SET payload=excluded.payload", Some(vec![Sql::String(encode(value)?)] )).map_err(|_| failure())?;
    Ok(())
}

fn leases(sql: &SqlStorage) -> Result<Vec<Lease>, GatewayError> {
    let cursor = sql
        .exec(
            "SELECT payload FROM uos_drain_lease ORDER BY id LIMIT 4097",
            None,
        )
        .map_err(|_| failure())?;
    let mut result = Vec::new();
    for row in cursor.raw() {
        let row = row.map_err(|_| failure())?;
        let [Sql::String(payload)] = row.as_slice() else {
            return Err(failure());
        };
        let value: Lease = serde_json::from_str(payload).map_err(|_| failure())?;
        if value.v != 1 {
            return Err(failure());
        }
        result.push(value);
    }
    if result.len() > policy::MAX_LEASES {
        return Err(failure());
    }
    Ok(result)
}

fn lease(sql: &SqlStorage, id: &str) -> Result<Option<Lease>, GatewayError> {
    let cursor = sql
        .exec(
            "SELECT payload FROM uos_drain_lease WHERE id=?",
            Some(vec![Sql::String(id.into())]),
        )
        .map_err(|_| failure())?;
    let mut rows = cursor.raw();
    let Some(row) = rows.next().transpose().map_err(|_| failure())? else {
        return Ok(None);
    };
    let [Sql::String(payload)] = row.as_slice() else {
        return Err(failure());
    };
    let result: Lease = serde_json::from_str(payload).map_err(|_| failure())?;
    if result.id != id || result.v != 1 || rows.next().is_some() {
        return Err(failure());
    }
    Ok(Some(result))
}

fn put_lease(sql: &SqlStorage, value: &Lease) -> Result<(), GatewayError> {
    sql.exec("INSERT INTO uos_drain_lease(id,payload) VALUES(?,?) ON CONFLICT(id) DO UPDATE SET payload=excluded.payload", Some(vec![Sql::String(value.id.clone()),Sql::String(encode(value)?)] )).map_err(|_| failure())?;
    Ok(())
}

fn delete_lease(sql: &SqlStorage, id: &str) -> Result<(), GatewayError> {
    sql.exec(
        "DELETE FROM uos_drain_lease WHERE id=?",
        Some(vec![Sql::String(id.into())]),
    )
    .map_err(|_| failure())?;
    Ok(())
}

pub fn initialize(sql: &SqlStorage, identity: &Identity) -> Result<(), GatewayError> {
    let cursor = sql.exec("SELECT name FROM sqlite_master WHERE type='table' AND name IN ('uos_drain_gate','uos_drain_lease')", None).map_err(|_| failure())?;
    let rows: Vec<_> = cursor.raw().collect();
    if rows.is_empty() {
        let occupied = canonical_count(sql)? != 0;
        sql.exec("CREATE TABLE uos_drain_gate(singleton INTEGER PRIMARY KEY CHECK(singleton=1),payload TEXT NOT NULL)", None).map_err(|_| failure())?;
        sql.exec(
            "CREATE TABLE uos_drain_lease(id TEXT PRIMARY KEY NOT NULL,payload TEXT NOT NULL)",
            None,
        )
        .map_err(|_| failure())?;
        return put_gate(sql, &policy::new_gate(identity.clone(), occupied)?);
    }
    if rows.len() != 2 || rows.iter().any(Result::is_err) {
        return Err(failure());
    }
    sql.exec("SELECT id,payload FROM uos_drain_lease LIMIT 0", None)
        .map_err(|_| failure())?;
    let current = gate(sql)?;
    if current.identity.object_id != identity.object_id
        || (current.phase != Phase::LegacyRunning && current.identity != *identity)
    {
        return Err(failure());
    }
    Ok(())
}

pub fn canonical_count(sql: &SqlStorage) -> Result<u64, GatewayError> {
    let mut rows = sql
        .exec("SELECT COUNT(*) FROM uos_authority_records", None)
        .map_err(|_| failure())?
        .raw();
    let row = rows
        .next()
        .transpose()
        .map_err(|_| failure())?
        .ok_or_else(failure)?;
    match row.as_slice() {
        [Sql::Integer(count)] => u64::try_from(*count).map_err(|_| failure()),
        _ => Err(failure()),
    }
}

pub fn authorize(
    sql: &SqlStorage,
    access: &Access,
    write: bool,
    ingress: bool,
) -> Result<(), GatewayError> {
    let current = gate(sql)?;
    match access {
        Access::Legacy if current.phase == Phase::LegacyRunning && ingress => Ok(()),
        Access::OperatorRead if !write => Ok(()),
        Access::Frozen(reference)
            if !write
                && current.phase == Phase::Frozen
                && current.frozen.as_ref() == Some(reference) =>
        {
            Ok(())
        }
        Access::Permit(permit) => {
            let owner = lease(sql, &permit.id)?.ok_or_else(failure)?;
            policy::valid_permit(&current, permit, &owner)?;
            policy::authorize(&current, &owner)
        }
        _ => Err(policy::closed()),
    }
}

pub fn status(sql: &SqlStorage) -> Result<Status, GatewayError> {
    Ok(Status {
        gate: gate(sql)?,
        counts: policy::counts(&leases(sql)?),
    })
}

fn acquire(sql: &SqlStorage, request: Acquire, ingress: bool) -> Result<Reply, GatewayError> {
    let mut current = gate(sql)?;
    if let Some(existing) = lease(sql, &request.id)? {
        if existing.capability_sha256 != request.capability_sha256
            || existing.kind != request.kind
            || existing.terminal
        {
            return Err(failure());
        }
        return Ok(Reply::Acquired {
            serving_epoch: existing.serving_epoch,
        });
    }
    let existing = leases(sql)?;
    if existing.len() >= policy::MAX_LEASES {
        return Err(GatewayError::new(
            503,
            "Durable writer tracking is at capacity",
            Some("state_transfer_tracking_capacity"),
        ));
    }
    if let LeaseKind::ImportFrame(import) = &request.kind {
        if current.phase == Phase::Provisioning {
            if !policy::hex(&import.capture_id, 64)
                || !policy::hex(&import.intent_digest, 64)
                || !policy::hex(&import.header_digest, 64)
                || !policy::hex(&import.chain_digest, 64)
            {
                return Err(failure());
            }
            current.phase = Phase::Importing;
            current.import = Some(import.clone());
            put_gate(sql, &current)?;
        }
        if existing
            .iter()
            .any(|value| matches!(value.kind, LeaseKind::ImportFrame(_)))
        {
            return Err(policy::refused(
                "An import frame is still active or unknown",
            ));
        }
    }
    let parent = request
        .parent
        .as_ref()
        .map(|value| lease(sql, &value.id))
        .transpose()?
        .flatten();
    policy::acquire(&current, &request, parent.as_ref(), ingress)?;
    put_lease(
        sql,
        &Lease {
            v: 1,
            id: request.id,
            serving_epoch: current.serving_epoch,
            capability_sha256: request.capability_sha256,
            parent_id: request.parent.map(|value| value.id),
            kind: request.kind,
            blocker: None,
            terminal: false,
        },
    )?;
    Ok(Reply::Acquired {
        serving_epoch: current.serving_epoch,
    })
}

fn join_native(
    sql: &SqlStorage,
    selector: NativeSelector,
    id: String,
    digest: String,
) -> Result<Reply, GatewayError> {
    let current = gate(sql)?;
    if !matches!(current.phase, Phase::Running | Phase::Draining)
        || !policy::hex(&id, 64)
        || !policy::hex(&digest, 64)
    {
        return Err(policy::closed());
    }
    let values = leases(sql)?;
    if values.len() >= policy::MAX_LEASES {
        return Err(failure());
    }
    let mut candidates=values.iter().filter(|lease| !lease.terminal && lease.serving_epoch==current.serving_epoch && matches!(&lease.kind,LeaseKind::NativeOperation(binding) if selector.matches(binding)));
    let parent = candidates
        .next()
        .ok_or_else(|| policy::refused("Native callback has no registered operation"))?;
    if candidates.next().is_some() {
        return Err(policy::refused("Native callback selector is ambiguous"));
    }
    let LeaseKind::NativeOperation(binding) = &parent.kind else {
        return Err(failure());
    };
    if let Some(existing) = lease(sql, &id)? {
        if existing.capability_sha256 != digest
            || existing.parent_id.as_ref() != Some(&parent.id)
            || existing.terminal
        {
            return Err(failure());
        }
    } else {
        put_lease(
            sql,
            &Lease {
                v: 1,
                id,
                serving_epoch: current.serving_epoch,
                capability_sha256: digest,
                parent_id: Some(parent.id.clone()),
                kind: LeaseKind::NativeJoin(binding.clone()),
                blocker: None,
                terminal: false,
            },
        )?;
    }
    Ok(Reply::Acquired {
        serving_epoch: current.serving_epoch,
    })
}

fn native_published(sql: &SqlStorage, join: Permit) -> Result<Reply, GatewayError> {
    let current = gate(sql)?;
    let joined = lease(sql, &join.id)?.ok_or_else(failure)?;
    policy::valid_permit(&current, &join, &joined)?;
    policy::authorize(&current, &joined)?;
    let LeaseKind::NativeJoin(binding) = &joined.kind else {
        return Err(failure());
    };
    let mut parent =
        lease(sql, joined.parent_id.as_deref().ok_or_else(failure)?)?.ok_or_else(failure)?;
    if !matches!(&parent.kind,LeaseKind::NativeOperation(value) if value==binding) {
        return Err(failure());
    }
    parent.blocker = None;
    parent.terminal = true;
    put_lease(sql, &parent)?;
    // The shared receiver verified the exact operation's terminal proof first.
    // Retire only its write capabilities: a Commit orders wholly before this
    // transaction or rejects after it. No socket/HTTP completion is inferred.
    for mut sibling in leases(sql)? {
        if sibling.parent_id.as_ref() == Some(&parent.id)
            && matches!(&sibling.kind,LeaseKind::NativeJoin(value) if value==binding)
        {
            sibling.terminal = true;
            sibling.blocker = None;
            put_lease(sql, &sibling)?;
        }
    }
    collect_terminal(sql)?;
    freeze_if_drained(sql)?;
    Ok(Reply::Status {
        status: status(sql)?,
    })
}

fn freeze_if_drained(sql: &SqlStorage) -> Result<(), GatewayError> {
    let mut current = gate(sql)?;
    if current.phase != Phase::Draining || !policy::counts(&leases(sql)?).empty() {
        return Ok(());
    }
    let digest = current.drain_digest.clone().ok_or_else(failure)?;
    let previous = current.frozen.clone().ok_or_else(failure)?;
    policy::freeze(
        &mut current,
        &digest,
        previous.capture_id,
        previous.finalization_digest,
    )?;
    put_gate(sql, &current)
}

fn collect_terminal(sql: &SqlStorage) -> Result<(), GatewayError> {
    loop {
        let values = leases(sql)?;
        let removable: Vec<_> = values
            .iter()
            .filter(|value| {
                value.terminal
                    && value.blocker.is_none()
                    && !values
                        .iter()
                        .any(|child| child.parent_id.as_deref() == Some(&value.id))
            })
            .map(|value| value.id.clone())
            .collect();
        if removable.is_empty() {
            return Ok(());
        }
        for id in removable {
            delete_lease(sql, &id)?;
        }
    }
}

fn finish(
    sql: &SqlStorage,
    permit: Permit,
    blocker: Option<Blocker>,
) -> Result<Reply, GatewayError> {
    let mut current = gate(sql)?;
    let Some(mut owner) = lease(sql, &permit.id)? else {
        freeze_if_drained(sql)?;
        return Ok(Reply::Finished {
            present: false,
            status: status(sql)?,
        });
    };
    let mut validation = owner.clone();
    validation.terminal = false;
    policy::valid_permit(&current, &permit, &validation)?;
    if owner.terminal {
        collect_terminal(sql)?;
        return Ok(Reply::Finished {
            present: true,
            status: status(sql)?,
        });
    }
    owner.blocker = blocker;
    owner.terminal = blocker.is_none();
    put_lease(sql, &owner)?;
    collect_terminal(sql)?;
    let count = policy::counts(&leases(sql)?);
    if current.phase == Phase::Draining && count.empty() {
        let digest = current.drain_digest.clone().ok_or_else(failure)?;
        let previous = current.frozen.clone().ok_or_else(failure)?;
        policy::freeze(
            &mut current,
            &digest,
            previous.capture_id,
            previous.finalization_digest,
        )?;
        put_gate(sql, &current)?;
    }
    Ok(Reply::Finished {
        present: true,
        status: status(sql)?,
    })
}

fn json_record(
    sql: &SqlStorage,
    key: &gateway_core::state::RecordKey,
) -> Result<Option<gateway_core::state::StoredRecord>, GatewayError> {
    crate::sqlite::read_control_record(sql, key)
}

fn finalization_facts(sql: &SqlStorage) -> Result<FinalizeFacts, GatewayError> {
    let key = |parts: serde_json::Value| gateway_auth::store::key(parts);
    let manifest = json_record(
        sql,
        &key(serde_json::json!(["uos_ai", "migration", "v3", "manifest"])),
    )?
    .ok_or_else(failure)?;
    let value = manifest.json_value()?;
    let owner_key = gateway_authority::keys::paid_control_owner();
    let owner = json_record(sql, &owner_key)?
        .map(|record| {
            let value = record.json_value()?;
            let text = |field: &str| {
                value
                    .get(field)
                    .and_then(serde_json::Value::as_str)
                    .map(str::to_owned)
                    .ok_or_else(failure)
            };
            if value.get("v").and_then(serde_json::Value::as_u64) != Some(1)
                || value.as_object().is_none_or(|v| v.len() != 4)
                || record.expires_at_ms.is_some()
            {
                return Err(failure());
            }
            Ok(OwnerBinding {
                owner_id: text("owner_id")?,
                owner_epoch: text("owner_epoch")?,
                key_fingerprint: text("key_fingerprint")?,
                owner_revision: record.revision.clone(),
            })
        })
        .transpose()?;
    Ok(FinalizeFacts {
        progress_present: json_record(
            sql,
            &key(serde_json::json!(["uos_ai", "migration", "v3", "staged"])),
        )?
        .is_some(),
        manifest_capture_id: value
            .get("capture_id")
            .and_then(serde_json::Value::as_str)
            .ok_or_else(failure)?
            .into(),
        manifest_revision: manifest.revision,
        canonical_records: canonical_count(sql)?,
        owner,
    })
}

fn control(
    sql: &SqlStorage,
    authorization: Authorization,
    digest: String,
    ingress: bool,
) -> Result<Reply, GatewayError> {
    let mut current = gate(sql)?;
    match authorization {
        Authorization::FinalizeImport(plan) => {
            let reference = policy::finalize(
                &mut current,
                &plan,
                &finalization_facts(sql)?,
                &digest,
                &policy::counts(&leases(sql)?),
            )?;
            put_gate(sql, &current)?;
            Ok(Reply::Frozen { reference })
        }
        Authorization::Activate(reference) => {
            let replayed = policy::activate(&mut current, &reference, &digest, ingress)?;
            put_gate(sql, &current)?;
            Ok(Reply::Activated {
                serving_epoch: current.serving_epoch,
                replayed,
                status: status(sql)?,
            })
        }
        Authorization::Drain(binding) => {
            if current.phase == Phase::Frozen
                && current
                    .frozen
                    .as_ref()
                    .is_some_and(|value| value.freeze_id == digest)
            {
                return Ok(Reply::Frozen {
                    reference: current.frozen.ok_or_else(failure)?,
                });
            }
            policy::binding(&current, &binding)?;
            if !matches!(current.phase, Phase::Running | Phase::Draining) {
                return Err(policy::closed());
            }
            if current
                .drain_digest
                .as_ref()
                .is_some_and(|old| old != &digest)
            {
                return Err(failure());
            }
            current.phase = Phase::Draining;
            current.drain_digest = Some(digest.clone());
            if policy::counts(&leases(sql)?).empty() {
                let previous = current.frozen.clone().ok_or_else(failure)?;
                policy::freeze(
                    &mut current,
                    &digest,
                    previous.capture_id,
                    previous.finalization_digest,
                )?;
            }
            put_gate(sql, &current)?;
            Ok(Reply::Status {
                status: status(sql)?,
            })
        }
    }
}

pub async fn execute(
    storage: Storage,
    operation: Operation,
    ingress: bool,
) -> Result<Reply, GatewayError> {
    let storage = std::rc::Rc::new(storage);
    let result = std::rc::Rc::new(std::cell::RefCell::new(None));
    let slot = result.clone();
    let transaction_storage = storage.clone();
    let transaction = storage
        .transaction(move |_| async move {
            let sql = transaction_storage.sql();
            let value = (|| -> Result<Reply, GatewayError> {
                Ok(match operation {
                    Operation::Acquire { request } => acquire(&sql, request, ingress)?,
                    Operation::JoinNative {
                        selector,
                        id,
                        capability_sha256,
                    } => join_native(&sql, selector, id, capability_sha256)?,
                    Operation::NativePublished { join } => native_published(&sql, join)?,
                    Operation::Finish { permit, blocker } => finish(&sql, permit, blocker)?,
                    Operation::Control {
                        authorization,
                        intent_digest,
                    } => control(&sql, authorization, intent_digest, ingress)?,
                    Operation::Status => Reply::Status {
                        status: status(&sql)?,
                    },
                    Operation::AwaitFrozen { .. } => {
                        return Err(policy::refused(
                            "AwaitFrozen requires its owned stream composition",
                        ));
                    }
                })
            })();
            *slot.borrow_mut() = Some(value);
            if slot.borrow().as_ref().is_some_and(Result::is_err) {
                return Err(worker::Error::RustError(
                    "Durable control transition refused".into(),
                ));
            }
            Ok(())
        })
        .await;
    if transaction.is_err() {
        return Err(result
            .borrow_mut()
            .take()
            .and_then(Result::err)
            .unwrap_or_else(failure));
    }
    storage.sync().await.map_err(|_| failure())?;
    let value = result.borrow_mut().take().ok_or_else(failure)?;
    value
}
