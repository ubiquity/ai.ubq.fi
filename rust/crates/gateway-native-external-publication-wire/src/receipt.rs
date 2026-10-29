use crate::request::{SourceExternalAccessRequest, reference};
use serde::{Deserialize, Serialize};
#[derive(Debug, Eq, PartialEq, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum SourceExternalPublicationDisposition {
    Applied,
    AlreadyCurrent,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct SourceExternalPublicationReceipt {
    pub v: u8,
    pub operation: String,
    pub disposition: SourceExternalPublicationDisposition,
    pub owner_id: String,
    pub owner_epoch: String,
    pub action_ref: String,
    pub operation_id: String,
    pub claim_ref: String,
    pub native_owner_id: String,
    pub owner_revision: String,
    pub source_revision: String,
    pub normalized_revision: String,
    pub publication_revision: String,
    pub current_source_revision: String,
    pub current_normalized_revision: String,
    pub account_id_hash: String,
    pub credential_version: String,
    pub previous_native_source_generation: String,
    pub native_generation: String,
}
impl SourceExternalPublicationReceipt {
    pub fn matches(&self, request: &SourceExternalAccessRequest) -> bool {
        self.v == 2
            && self.operation == "source_external_access_publish"
            && self.action_ref == request.action_ref
            && self.operation_id == request.operation_id
            && self.claim_ref == request.claim_ref
            && self.native_owner_id == request.native_owner_id
            && self.owner_revision == request.owner_revision
            && self.source_revision == request.source_revision
            && self.normalized_revision == request.normalized_revision
            && self.publication_revision == request.publication_revision
            && self.account_id_hash == request.account_id_hash
            && self.credential_version == request.credential_version
            && self.previous_native_source_generation == request.previous_native_source_generation
            && self.native_generation == request.native_generation
            && reference(&self.current_source_revision)
            && reference(&self.current_normalized_revision)
            && (self.disposition == SourceExternalPublicationDisposition::AlreadyCurrent
                || (self.current_source_revision == self.publication_revision
                    && self.current_normalized_revision == self.publication_revision))
    }
}
