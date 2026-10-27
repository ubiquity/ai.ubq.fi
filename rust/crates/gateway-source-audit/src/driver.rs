use crate::{keys, model as source_model};
use gateway_admin_data::inventory::AuditInventory;
use gateway_admin_data::report::Audit;
use gateway_admin_data::reset::inspect_reset_row;
use gateway_admin_data::reset::inspect_reset_usage;
use gateway_admin_data::validate::source_fields;
use gateway_admin_data::validate::under;
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use gateway_ledger_audit::{paid, stream::observe_coupled, usage};
use gateway_migration_ledger::{counter::valid_api_window, rows::ObservedRow};
use gateway_profile_values::scalar::{error, integer, text};
use serde_json::json;

/// Same six complete coupled audits, retaining the original visits and first error.
pub fn inspect_inventory(rows: &dyn AuditInventory, now: i64) -> Result<Audit, GatewayError> {
    let mut known = std::collections::BTreeSet::new();
    let mut unlimited = std::collections::BTreeSet::new();
    let ((mut audit, reset, reset_error, model), ledger) =
        observe_coupled(now, |observe_ledger| {
            let mut reset = Audit::default();
            let mut reset_error = None;
            let mut model = source_model::ModelAudit::default();
            let audit = keys::visit_key_pairs_with(
                rows,
                &mut |pair| {
                    if pair.unlimited_paid {
                        unlimited.insert(pair.key_id.clone());
                    }
                    known.insert(pair.key_id);
                    Ok(())
                },
                &mut |row| {
                    observe_ledger(row);
                    if reset_error.is_none() {
                        reset_error = inspect_reset_row(row, &mut reset).err();
                    }
                    model.accept(row);
                },
            )?;
            Ok(((audit, reset, reset_error, model), known, unlimited))
        })?;
    audit.merge(ledger);
    if let Some(error) = reset_error {
        return Err(error);
    }
    audit.merge(reset);
    audit.errors.extend(model.finish()?);
    Ok(audit)
}

pub fn inspect_source_rows(
    rows: &[ObservedRow],
    complete: bool,
    now: i64,
) -> Result<Audit, GatewayError> {
    if !complete {
        return Err(error(
            "Migration validation requires complete source inventories.",
        ));
    }
    let (pairs, mut audit) = keys::inspect_key_pairs(rows)?;
    let known: std::collections::BTreeSet<String> =
        pairs.iter().map(|pair| pair.key_id.clone()).collect();
    let unlimited: std::collections::BTreeSet<String> = pairs
        .iter()
        .filter(|pair| pair.unlimited_paid)
        .map(|pair| pair.key_id.clone())
        .collect();
    audit.merge(usage::inspect_api_usage(rows, &known)?);
    audit.merge(usage::inspect_kernel(rows)?);
    audit.merge(paid::inspect_paid(rows, &known, &unlimited, now)?);
    audit.merge(inspect_reset_usage(rows)?);
    audit
        .errors
        .extend(source_model::inspect_model_snapshot(rows)?);
    let repo = rows
        .iter()
        .filter(|r| under(&r.parts, &["ubq_ai", "kernel_auth", "limits"]))
        .count();
    let org = rows
        .iter()
        .filter(|r| under(&r.parts, &["ubq_ai", "kernel_auth", "org_limits"]))
        .count();
    audit.counts.insert("kernel_repo_limits".into(), repo);
    audit.counts.insert("kernel_org_limits".into(), org);
    if repo > 0 || org > 0 {
        audit.errors.push(JsString16::from_utf8(&format!(
            "legacy combined kernel quota records remain: repo={repo} org={org}"
        )));
    }
    for pair in pairs {
        inspect_key_usage(rows, &pair, now, &mut audit)?;
    }
    Ok(audit)
}
fn inspect_key_usage(
    rows: &[ObservedRow],
    pair: &keys::AuditedKeyPair,
    now: i64,
    audit: &mut Audit,
) -> Result<(), GatewayError> {
    let record = &pair.id;
    let hash = &pair.hash;
    let id = pair.key_id.as_str();
    if !source_fields::equals(record, "usage_quota_version", &json!(3))
        || !source_fields::equals(hash, "usage_quota_version", &json!(3))
    {
        audit.errors.push(JsString16::from_utf8(&format!(
            "api key quota ledger version is not V3: {id}"
        )));
        return Ok(());
    }
    if !source_fields::equals(record, "usage_limit_requests", &json!(-1)) {
        // Same revoked-clock rule as counter::migration_policy_now, without
        // converting the complete model-bearing source policy into Value.
        let clock = if source_fields::member(record, "revoked_at_ms")
            .is_some_and(|value| value.as_str() == "null")
        {
            now
        } else {
            source_fields::integer(record, "usage_reset_at_ms")
                .map(|reset| (reset - 1).max(0))
                .ok_or_else(|| error("Invalid API-key migration clock."))?
        };
        let span = source_fields::integer(record, "window_ms")
            .ok_or_else(|| error("Invalid API-key window."))?;
        let initial = source_fields::integer(record, "usage_reset_at_ms")
            .ok_or_else(|| error("Invalid API-key reset."))?
            - span;
        let start = if clock < initial + span {
            initial
        } else {
            initial + (clock - initial).div_euclid(span) * span
        };
        let reset = start + span;
        let version = format!("v3:{span}");
        let wp = vec![
            json!("uos_ai"),
            json!("api_key_usage"),
            json!("v3"),
            json!("window"),
            json!(id),
            json!(version),
            json!(start),
        ];
        let valid = rows
            .iter()
            .find(|r| r.parts == wp)
            .map(ObservedRow::value)
            .transpose()?
            .is_some_and(|v| {
                valid_api_window(v)
                    && text(v, "key_id") == Some(id)
                    && text(v, "policy_version") == Some(&version)
                    && integer(v, "window_start_ms") == Some(start)
                    && integer(v, "window_reset_at_ms") == Some(reset)
            });
        if !valid {
            audit.errors.push(JsString16::from_utf8(&format!(
                "bounded V3 aggregate is missing or invalid: {id}"
            )));
        }
        let bp = vec![
            json!("uos_ai"),
            json!("migrations"),
            json!("kv_read_incident_v2"),
            json!("api_key_usage_baseline"),
            json!(id),
            json!(version),
            json!(start),
        ];
        let valid = rows
            .iter()
            .find(|r| r.parts == bp)
            .map(ObservedRow::value)
            .transpose()?
            .is_some_and(|v| {
                gateway_migration_ledger::counter::valid_baseline(v)
                    && text(v, "key_id") == Some(id)
                    && text(v, "policy_version") == Some(&version)
                    && integer(v, "window_start_ms") == Some(start)
            });
        if !valid {
            audit.errors.push(JsString16::from_utf8(&format!(
                "bounded counter migration baseline is missing, stale, or invalid: {id}"
            )));
        }
    }
    inspect_paid_reference(rows, record, id, audit);
    Ok(())
}

fn inspect_paid_reference(rows: &[ObservedRow], record: &SourceJson, id: &str, audit: &mut Audit) {
    if let Some(request) = source_fields::member(record, "paid_fallback_reservation_request_id")
        .filter(|value| value.as_str() != "null")
        && !rows.iter().any(|row| {
            under(&row.parts, &["uos_ai", "paid_fallback", "ledger"])
                && gateway_core::source_control::decode_source_control_record(&row.record)
                    .is_ok_and(|value| {
                        source_fields::text(&value, "key_id").as_deref() == Some(id)
                            && source_fields::member(&value, "id")
                                .is_some_and(|actual| source_fields::same_value(&actual, &request))
                            && (source_fields::equals(&value, "billing_status", &json!("pending"))
                                || source_fields::equals(
                                    &value,
                                    "billing_status",
                                    &json!("unresolved"),
                                ))
                    })
        })
    {
        audit.errors.push(JsString16::from_utf8(&format!(
            "paid fallback reservation has no pending ledger record: {id}"
        )));
    }
}
