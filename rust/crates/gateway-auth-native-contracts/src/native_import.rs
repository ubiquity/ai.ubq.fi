use gateway_auth_native_wire::native_read::NATIVE_WIRE_VERSION;
use gateway_source::error::GatewayError;
use serde::{Deserialize, Serialize};
pub mod catalog_seed;
pub mod state;
pub const NATIVE_IMPORT_CODEX_AUTH_PATH: &str = "/v1/native/import/codex-auth";
pub const NATIVE_IMPORT_PROFILE_PATH: &str = "/v1/native/import/profile";
pub const NATIVE_IMPORT_OPERATION_HEADER: &str = "x-uos-native-operation-id";
pub const NATIVE_IMPORT_ACTION_HEADER: &str = "x-uos-native-action-ref";
pub const NATIVE_IMPORT_BODY_HEADER: &str = "x-uos-native-body-sha256";
#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum NativeImportKind {
    CodexAuth,
    NativeProfile,
}
impl NativeImportKind {
    pub fn path(self) -> &'static str {
        match self {
            Self::CodexAuth => NATIVE_IMPORT_CODEX_AUTH_PATH,
            Self::NativeProfile => NATIVE_IMPORT_PROFILE_PATH,
        }
    }
    pub fn body_limit_bytes(self) -> usize {
        match self {
            Self::CodexAuth => 32 * 1024 * 1024,
            Self::NativeProfile => 5 * 1024 * 1024,
        }
    }
}
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct NativeImportMetadata {
    pub v: u8,
    pub operation_id: String,
    pub action_ref: String,
}
impl NativeImportMetadata {
    pub fn validate(&self) -> Result<(), GatewayError> {
        if self.v != NATIVE_WIRE_VERSION
            || !reference(&self.operation_id)
            || !reference(&self.action_ref)
        {
            return Err(invalid());
        }
        Ok(())
    }
}
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
pub enum NativeImportTarget {
    ExistingAccount { account_id_hash: String },
    PreparedAccountSlot { slot_ref: String },
    PreparedProfile { profile_ref: String },
}
impl NativeImportTarget {
    pub fn kind(&self) -> NativeImportKind {
        match self {
            Self::PreparedProfile { .. } => NativeImportKind::NativeProfile,
            _ => NativeImportKind::CodexAuth,
        }
    }
    pub fn validate(&self) -> Result<(), GatewayError> {
        let valid = match self {
            Self::ExistingAccount { account_id_hash } => digest(account_id_hash),
            Self::PreparedAccountSlot { slot_ref } => reference(slot_ref),
            Self::PreparedProfile { profile_ref } => reference(profile_ref),
        };
        if !valid {
            return Err(invalid());
        }
        Ok(())
    }
}
pub(crate) fn reference(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 256
        && value
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_' | b'.' | b':'))
}
pub(crate) fn digest(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}
fn invalid() -> GatewayError {
    GatewayError::new(
        400,
        "Invalid native import metadata",
        Some("native_import_invalid"),
    )
}
