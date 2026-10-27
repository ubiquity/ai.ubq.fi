use crate::{
    inventory::{AuditInventory, SliceInventory},
    report::Audit,
    validate::{reference, under},
};
use gateway_core::error::GatewayError;
use gateway_migration_ledger::rows::ObservedRow;
use serde_json::Value;

pub fn inspect_reset_usage(rows: &[ObservedRow]) -> Result<Audit, GatewayError> {
    inspect_reset_inventory(&SliceInventory(rows))
}
pub fn inspect_reset_inventory(rows: &dyn AuditInventory) -> Result<Audit, GatewayError> {
    let mut audit = Audit::default();
    rows.visit(&mut |row| inspect_reset_row(row, &mut audit))?;
    Ok(audit)
}
pub fn inspect_reset_row(row: &ObservedRow, audit: &mut Audit) -> Result<(), GatewayError> {
    if !under(&row.parts, &["uos_ai", "codex_reset_usage"]) {
        return Ok(());
    }
    audit.count("codex_reset_usage");
    let p = &row.parts;
    let valid = p.len() == 5
        && p[2] == "account"
        && p[3] == "v1"
        && p[4].as_str().is_some_and(|s| !s.is_empty());
    if !valid {
        audit.errors.push(
            format!("codex reset usage key is malformed: {}", reference(p))
                .as_str()
                .into(),
        );
        return Ok(());
    }
    if !row.value()?.get("enabled").is_some_and(Value::is_boolean) {
        audit.errors.push(
            format!("codex reset usage record is malformed: {}", reference(p))
                .as_str()
                .into(),
        );
    }
    Ok(())
}
