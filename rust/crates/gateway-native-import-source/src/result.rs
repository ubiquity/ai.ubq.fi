use gateway_core::source_json::SourceJson;
use std::collections::BTreeMap;

/// Native-only source result data, not a durable publication proof or CF completion grant.
pub struct NativeImportResult {
    pub status: u16,
    pub headers: BTreeMap<String, String>,
    pub body: SourceJson,
    pub completion: NativeImportCompletion,
}
pub enum NativeImportCompletion {
    Published {
        publication_revision: String,
        native_receipt_ref: String,
    },
    AlreadyCurrent {
        publication_revision: String,
        native_receipt_ref: String,
    },
    PendingUnknown,
    Conflict,
    Unavailable,
    Refused,
}
