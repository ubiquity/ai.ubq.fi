//! Existing ordinary native import binding/mode DATA. No claim, Start or currentness issuer.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct NativeImportBinding {
    pub action_ref: String,
    pub operation_id: String,
    pub claim_ref: String,
    pub native_owner_id: String,
    pub account_id_hash: String,
    pub source_native_generation: String,
    pub owner_revision: String,
    pub source_revision: String,
    pub normalized_revision: String,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum NativeImportMode {
    ImportOnce,
    ReconcileOnly,
}
