use crate::Mapping;
use crate::Transform;
use crate::decode;
use crate::digest;
use crate::invalid;
use crate::key;
use crate::parts;
use crate::starts;
use crate::unavailable;
use gateway_core::error::GatewayError;
use gateway_core::query::{QueryRequest, QuerySelector, QueryStore};
use gateway_core::source_json::SourceJson;
use gateway_core::state::RecordKey;
use gateway_core::state::StoredRecord;
use gateway_core::tuple_key::TupleKey;
use gateway_migration_data::capture::PrivateCapture;
use gateway_migration_data::capture::SourceEntry;
use gateway_migration_data::custody::{ArtifactContext, CustodyRecord, validate_artifacts};
use gateway_migration_data::error::MigrationError;
use gateway_migration_data::manifest::Manifest;
use gateway_migration_data::manifest::manifest_key;
use gateway_migration_data::manifest::mapping_key;
use gateway_migration_values::inverse::{
    inverse_value_for_key, source_profile_key, transform_for_new,
};
use gateway_profile_source::codec::to_source_profile as to_profile;
use gateway_profile_values::date::CanonicalDates;
use serde::Deserialize;
use serde_json::Value;
use serde_json::json;
use std::collections::{BTreeMap, BTreeSet};
mod native_import_custody;

/// Export only after the current target owner has actually stopped its writers.
/// QueryStore pages are not a cross-page snapshot; no stale backup is read or restored.
pub(crate) struct LatestRow {
    pub key: RecordKey,
    pub record: StoredRecord,
    pub mapping: Option<Mapping>,
}
fn control_row(key: &RecordKey, p: &[Value], record: &StoredRecord) -> Result<bool, GatewayError> {
    if starts(p, &["uos_ai", "native_live"]) {
        record.validate_version()?;
        if p.len() != 5 || p[2] != "v1" || record.expires_at_ms.is_some() {
            return Err(invalid(
                "Native live custody key or permanent retention is invalid",
            ));
        }
        gateway_authority::native_live::validate_live_custody(
            p[3].as_str()
                .ok_or_else(|| invalid("Native live custody kind is invalid"))?,
            p[4].as_str()
                .ok_or_else(|| invalid("Native live custody reference is invalid"))?,
            record.json_value()?,
        )?;
        return Ok(true);
    }
    if starts(p, &["uos_ai", "routing_runtime"]) {
        return Ok(true);
    }
    if starts(
        p,
        &["uos_ai", "codex_live_calls", "v1", "pending_retirement"],
    ) || starts(p, &["uos_ai", "auth", "session_revision_index"])
    {
        record.validate_version()?;
        if starts(p, &["uos_ai", "auth", "session_revision_index"]) {
            gateway_auth::session_index::validate_custody_record(key, record)?;
        }
        if starts(
            p,
            &["uos_ai", "codex_live_calls", "v1", "pending_retirement"],
        ) {
            #[derive(Deserialize)]
            #[serde(deny_unknown_fields)]
            struct Retirement {
                schema_version: u8,
                call_id: String,
                upstream_status: u16,
                created_at_ms: i64,
            }
            let marker: Retirement = decode(record.json_value()?)?;
            if p.len() != 5
                || p[4] != marker.call_id
                || marker.schema_version != 1
                || ![404, 410].contains(&marker.upstream_status)
                || !(0..=gateway_core::identity::MAX_SAFE_INTEGER).contains(&marker.created_at_ms)
                || record.expires_at_ms.is_some()
            {
                return Err(invalid("Pending live-call retirement custody is malformed"));
            }
        }
        return Ok(true);
    }
    if *key == gateway_auth::native_replay::native_nonce_key() {
        record.validate_version()?;
        return Ok(true);
    }
    if starts(p, &["uos_ai", "authority", "native_owner_renewal"]) {
        gateway_authority::native_control::custody::validate_native_control_custody(key, record)?;
        return Ok(true);
    }
    Ok(false)
}

fn authority_control(
    all: &BTreeMap<String, StoredRecord>,
    key: &RecordKey,
    p: &[Value],
    record: &StoredRecord,
    now_ms: i64,
) -> Result<bool, GatewayError> {
    if *key == gateway_authority::keys::kernel_defaults() {
        let mut expected = gateway_authority::kernel::KernelDefaults::default();
        if let Some(source) =
            all.get(&crate::key(json!(["default", "kernel_policy_limit_requests"])).0)
        {
            expected.usage_limit_requests =
                gateway_admin_contracts::fields::limit(source.json_value()?).unwrap_or(-1);
        }
        if let Some(source) = all.get(&crate::key(json!(["default", "kernel_policy_window_ms"])).0)
        {
            expected.window_ms = gateway_admin_contracts::fields::positive(source.json_value()?)
                .unwrap_or(604800000);
        }
        let actual: gateway_authority::kernel::KernelDefaults = decode(record.json_value()?)?;
        if actual != expected {
            return Err(invalid(
                "Latest derived Kernel defaults differ from source defaults; inverse would lose policy",
            ));
        }
    }
    if p.get(3).and_then(Value::as_str) == Some("api_policy") {
        let id = p
            .get(4)
            .and_then(Value::as_str)
            .ok_or_else(|| invalid("Derived API policy ID is missing"))?;
        let source = all
            .get(&gateway_auth::api_keys::id_key(id).0)
            .ok_or_else(|| invalid("Derived API policy has no source ID"))?;
        let source: gateway_admin_keys::keys::ApiKeyRecord = decode(source.json_value()?)?;
        let mut expected = gateway_admin_keys::keys::authority_policy(&source, now_ms);
        let actual: gateway_authority::quota::ApiKeyPolicy = decode(record.json_value()?)?;
        if source.policy.revoked_at_ms.is_some() && actual.expires_at_ms <= now_ms {
            expected.expires_at_ms = actual.expires_at_ms;
        }
        if actual != expected {
            return Err(invalid(
                "Latest derived API policy differs from the source ID; inverse would lose policy",
            ));
        }
    }
    if p.get(3).and_then(Value::as_str).is_some_and(|s| {
        [
            "codex_pool",
            "paid_routing",
            "paid_control_owner",
            "paid_control_config",
            "paid_control_observation",
            "codex_reset_config",
        ]
        .contains(&s)
    }) {
        record.validate_version()?;
        return Ok(true);
    }
    if p.get(3).and_then(Value::as_str).is_some_and(|s| {
        [
            "api_policy",
            "codex_pool",
            "kernel_defaults",
            "paid_routing",
            "paid_control_owner",
            "paid_control_config",
            "paid_control_observation",
            "codex_reset_config",
        ]
        .contains(&s)
    }) {
        return Ok(false);
    }
    Err(unavailable(
        "Unknown normalized authority state cannot be silently omitted from inverse",
    ))
}

async fn collect_rows<S: QueryStore>(
    store: &S,
    now_ms: i64,
) -> Result<BTreeMap<String, StoredRecord>, GatewayError> {
    let mut all = BTreeMap::new();
    let mut cursor = None;
    let mut seen = BTreeSet::new();
    loop {
        let page = store
            .query(QueryRequest {
                selector: QuerySelector::Prefix {
                    prefix: TupleKey::from_parts(&[])?.to_record_key(),
                    start: None,
                    end: None,
                },
                reverse: false,
                limit: 256,
                cursor: cursor.clone(),
                now_ms,
            })
            .await?;
        for entry in page.entries {
            if all.insert(entry.key.0.clone(), entry.record).is_some() {
                return Err(invalid("Inverse query repeated a source key"));
            }
        }
        cursor = page.next_cursor;
        if let Some(c) = &cursor {
            if !seen.insert(c.clone()) {
                return Err(invalid("Inverse query cursor loop"));
            }
        } else {
            break;
        }
    }
    Ok(all)
}

fn validate_inventory(
    all: &BTreeMap<String, StoredRecord>,
    manifest: &Manifest,
) -> Result<(), GatewayError> {
    let mapping_prefix = key(json!([
        "uos_ai",
        "migration",
        "v3",
        "row",
        manifest.capture_id
    ]));
    let prefix = TupleKey::from_record_key(&mapping_prefix)?;
    let mappings = all
        .iter()
        .filter_map(|(key, record)| {
            let tuple = TupleKey::from_record_key(&RecordKey(key.clone())).ok()?;
            tuple
                .is_strict_descendant_of(&prefix)
                .then_some((key.clone(), record.clone()))
        })
        .collect::<Vec<_>>();
    if mappings.len() != manifest.source_rows
        || gateway_migration_data::manifest::mapping_checksum(mappings)?
            != manifest.mapping_checksum
    {
        return Err(invalid("Inverse mapping inventory is missing or modified"));
    }
    let mut routing_rows = Vec::new();
    for (name, record) in all {
        let key = RecordKey(name.clone());
        if starts(&parts(&key)?, &["uos_ai", "routing_runtime"]) {
            routing_rows.push((key, record));
        }
    }
    gateway_routing_runtime::custody::validate_journal_rows(
        routing_rows.iter().map(|(key, record)| (key, *record)),
    )?;
    Ok(())
}

pub(crate) async fn export_rows<S: QueryStore>(
    store: &S,
    now_ms: i64,
) -> Result<(Manifest, Vec<LatestRow>, Vec<CustodyRecord>), GatewayError> {
    let manifest = store
        .read(&manifest_key())
        .await?
        .ok_or_else(|| invalid("No adopted capture manifest"))?;
    let manifest: Manifest = decode(manifest.json_value()?)?;
    if manifest.v != 4 || now_ms < manifest.captured_at_ms {
        return Err(invalid("Inverse capture clock/version invalid"));
    }
    let mut all = collect_rows(store, now_ms).await?;
    validate_inventory(&all, &manifest)?;
    let mut entries = Vec::new();
    let mut controls = Vec::new();
    for (k, record) in &all {
        let key = RecordKey(k.clone());
        let p = parts(&key)?;
        // A legacy reader does not understand pending retirement. Retain the
        // suppressed mapping only in private custody, never as a joinable Bun row.
        if p.len() == 4
            && starts(&p, &["uos_ai", "codex_live_calls", "v1"])
            && all.contains_key(
                &crate::key(json!([
                    "uos_ai",
                    "codex_live_calls",
                    "v1",
                    "pending_retirement",
                    p[3]
                ]))
                .0,
            )
        {
            controls.push(k.clone());
            continue;
        }
        if let Some(mapping) = all.get(&mapping_key(&manifest.capture_id, k).0) {
            let mapping: Mapping = decode(mapping.json_value()?)?;
            entries.push((key, Some(mapping)));
            continue;
        }
        if starts(&p, &["uos_ai", "migration"]) {
            continue;
        }
        if control_row(&key, &p, record)? {
            controls.push(k.clone());
            continue;
        }
        if starts(&p, &["uos_ai", "authority"]) {
            if authority_control(&all, &key, &p, record, now_ms)? {
                controls.push(k.clone());
            }
            continue;
        }
        if let Some(expected) = manifest.derived.get(k)
            && digest(record)? == *expected
        {
            continue;
        }
        if !gateway_migration_projection::projection::known(&p) {
            return Err(unavailable(
                "Latest target includes an unmapped source key family",
            ));
        }
        entries.push((key, None));
    }
    native_import_custody::validate(&all)?;
    // No payload moves until ALL inventory/journal/import checks above succeed.
    let entries = entries
        .into_iter()
        .map(|(key, mapping)| {
            let record = all
                .remove(&key.0)
                .ok_or_else(|| invalid("Current inverse record is missing"))?;
            Ok(LatestRow {
                key,
                record,
                mapping,
            })
        })
        .collect::<Result<Vec<_>, GatewayError>>()?;
    let controls = controls
        .into_iter()
        .map(|key| {
            Ok(CustodyRecord {
                record: all
                    .remove(&key)
                    .ok_or_else(|| invalid("Current inverse record is missing"))?,
                key,
            })
        })
        .collect::<Result<Vec<_>, GatewayError>>()?;
    Ok((manifest, entries, controls))
}

async fn export_internal<S: QueryStore>(
    store: &S,
    now_ms: i64,
) -> Result<(PrivateCapture, Vec<CustodyRecord>), MigrationError> {
    let (manifest, rows, controls) = export_rows(store, now_ms).await?;
    if manifest.native_revision.is_some() {
        return Err(MigrationError::from(unavailable(
            "A native graph capture requires explicit native latest-state export",
        )));
    }
    let mut entries = Vec::new();
    for row in rows {
        let mut source = if let Some(mapping) = &row.mapping {
            mapping.source.clone()
        } else {
            SourceEntry {
                key: vec![],
                value: SourceJson::parse("null")?,
                versionstamp: String::new(),
                expires_at_ms: None,
            }
        };
        let transform = row
            .mapping
            .as_ref()
            .map(|m| m.transform)
            .unwrap_or(transform_for_new(&parts(&row.key)?));
        if transform == Transform::Identity || row.mapping.is_none() {
            source.key = source_profile_key(&row.key)?;
        }
        if !row.mapping.as_ref().is_some_and(|m| {
            m.transform != Transform::Identity
                && digest(&row.record).is_ok_and(|d| d == m.baseline_digest)
        }) {
            source.value = to_profile(&inverse_value_for_key(
                transform,
                &row.key,
                &row.record,
                row.mapping.as_ref().map(|m| &m.source),
            )?)?;
        }
        source.versionstamp = row.record.revision;
        source.expires_at_ms = row.record.expires_at_ms;
        entries.push(source);
    }
    let result = PrivateCapture {
        v: 1,
        source_git_sha: manifest.source_git_sha,
        captured_at_ms: now_ms,
        entries,
    };
    // A private export is not a successful rollback until every latest source/index
    // invariant can be re-adopted. This reads no old database or backup.
    gateway_migration_prepare::engine::prepare(&result, &CanonicalDates)?;
    Ok((result, controls))
}

/// Source data alone is insufficient once a Rust transport/control owner exists.
pub async fn export_latest<S: QueryStore>(
    store: &S,
    now_ms: i64,
) -> Result<PrivateCapture, MigrationError> {
    let (source, controls) = export_internal(store, now_ms).await?;
    if !controls.is_empty() {
        return Err(MigrationError::from(GatewayError::unavailable(
            "Rust-only control/replay custody requires export_rollback_bundle; source-only export would omit it",
            "migration_control_custody_required",
        )));
    }
    Ok(source)
}

#[derive(Clone, serde::Serialize)]
pub struct PrivateRollbackBundle {
    pub v: u32,
    pub source: PrivateCapture,
    pub rust_controls: Vec<CustodyRecord>,
    pub artifacts: ArtifactContext,
}
pub async fn export_rollback_bundle<S: QueryStore>(
    store: &S,
    now_ms: i64,
    artifacts: ArtifactContext,
) -> Result<PrivateRollbackBundle, MigrationError> {
    validate_artifacts(&artifacts)?;
    let (source, rust_controls) = export_internal(store, now_ms).await?;
    Ok(PrivateRollbackBundle {
        v: 1,
        source,
        rust_controls,
        artifacts,
    })
}
