//! Explicit private artifact/control DATA, never a restore or authorization constructor.
use crate::invalid;
use gateway_core::state::StoredRecord;
use gateway_source::error::GatewayError;

/// Private current operational evidence; this type has no restore/authorization constructor.
#[derive(Clone, serde::Serialize)]
pub struct CustodyRecord {
    pub key: String,
    pub record: StoredRecord,
}
#[derive(Clone, serde::Serialize)]
pub struct ArtifactContext {
    pub rust_git_sha: String,
    pub rust_git_tree: String,
    pub wasm_sha256: String,
    pub native_sha256: String,
    /// Separately retained stopped native nonce SQLite+WAL/SHM custody, never opened here.
    pub native_nonce_capture_sha256: Option<String>,
}
pub fn validate_artifacts(artifacts: &ArtifactContext) -> Result<(), GatewayError> {
    let hash = |value: &str, len: usize| {
        value.len() == len
            && value
                .bytes()
                .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
    };
    if !hash(&artifacts.rust_git_sha, 40)
        || !hash(&artifacts.rust_git_tree, 40)
        || !hash(&artifacts.wasm_sha256, 64)
        || !hash(&artifacts.native_sha256, 64)
        || artifacts
            .native_nonce_capture_sha256
            .as_deref()
            .is_some_and(|s| !hash(s, 64))
    {
        return Err(invalid("Rollback artifact identity is malformed"));
    }
    Ok(())
}
