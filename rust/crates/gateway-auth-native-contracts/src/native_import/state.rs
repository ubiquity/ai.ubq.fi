use super::{NativeImportMetadata, NativeImportTarget};
use gateway_auth_native_wire::control::NATIVE_RENEWAL_MAX_MS;
use gateway_contracts::identity::MAX_SAFE_INTEGER;
use gateway_source::error::GatewayError;
use serde::{Deserialize, Serialize};
pub const IMPORT_AUTHORITY_OPERATION: &str = "codex_auth_import_authority";
pub const IMPORT_STARTED_OPERATION: &str = "codex_auth_import_started";
fn invalid() -> GatewayError {
    GatewayError::new(
        400,
        "Invalid fixed native import authority frame",
        Some("native_import_authority_wire_invalid"),
    )
}
fn text(value: &str) -> Result<(), GatewayError> {
    if !value.trim().is_empty() && value.len() <= 4096 {
        Ok(())
    } else {
        Err(invalid())
    }
}
fn hash(value: &str) -> Result<(), GatewayError> {
    if super::digest(value) {
        Ok(())
    } else {
        Err(invalid())
    }
}
fn time(value: i64) -> Result<(), GatewayError> {
    if (0..=MAX_SAFE_INTEGER).contains(&value) {
        Ok(())
    } else {
        Err(invalid())
    }
}
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ImportAuthorityStateData {
    pub action_version: u8,
    pub metadata: NativeImportMetadata,
    pub target: NativeImportTarget,
    pub action_revision: String,
    pub native_owner_id: String,
    pub account_id_hash: String,
    pub source_native_generation: String,
    pub owner_revision: String,
    pub source_revision: String,
    pub normalized_revision: String,
    pub created_at_ms: i64,
    pub deadline_ms: i64,
}
impl ImportAuthorityStateData {
    pub fn validate(&self) -> Result<(), GatewayError> {
        self.metadata.validate()?;
        self.target.validate()?;
        if !matches!(self.action_version, 2 | 3)
            || self.created_at_ms.checked_add(NATIVE_RENEWAL_MAX_MS) != Some(self.deadline_ms)
            || !matches!(&self.target, NativeImportTarget::ExistingAccount { account_id_hash } if account_id_hash == &self.account_id_hash)
        {
            return Err(invalid());
        }
        for value in [
            &self.metadata.action_ref,
            &self.account_id_hash,
            &self.source_native_generation,
        ] {
            hash(value)?;
        }
        for value in [
            &self.action_revision,
            &self.native_owner_id,
            &self.owner_revision,
            &self.source_revision,
            &self.normalized_revision,
        ] {
            text(value)?;
        }
        time(self.created_at_ms)?;
        time(self.deadline_ms)
    }
}
