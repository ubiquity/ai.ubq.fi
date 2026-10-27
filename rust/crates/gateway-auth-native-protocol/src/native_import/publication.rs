// Exact shared import publication DATA and its operation-specific reply bound.
// No body/start/currentness/durability capability is constructed in this crate.
use gateway_auth_credentials::provider_credentials::codex_credential_identity;
use gateway_auth_native_wire::{publication_command as command, publication_data as data};
use gateway_auth_primitives::crypto::{Secret, sha256_hex};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde::{Deserialize, Serialize};
fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Native import preparation is unavailable, changed, expired, or unsupported.",
        "native_import_authority_unavailable",
    )
}
#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum NativeImportPublicationDisposition {
    Applied,
    AlreadyCurrent,
}
/// Sanitized authoritative DATA, not native physical durability/completion proof.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct NativeImportPublication {
    pub v: u8,
    pub operation_id: String,
    pub action_ref: String,
    pub disposition: NativeImportPublicationDisposition,
    pub publication: data::ImportPublicationDataV1,
}
impl NativeImportPublication {
    pub fn validate_for(
        &self,
        request: &command::ImportPublicationCommandData,
        actual_request_bytes: &[u8],
    ) -> Result<(), GatewayError> {
        request.validate()?;
        self.publication.validate()?;
        let p = &self.publication;
        let identity = codex_credential_identity(
            &request.document.account_id,
            &Secret::new(request.document.access_token.clone()),
            &Secret::new(String::new()),
        );
        if self.v != 1
            || self.operation_id != request.operation_id
            || self.action_ref != request.action_ref
            || p.operation_id != request.operation_id
            || p.claim_ref != request.claim_ref
            || p.publication_revision != request.publication_revision
            || p.native_receipt_ref != request.native_receipt_ref
            || p.native_source_generation != request.native_source_generation
            || p.access_generation != request.access_generation
            || p.credential_version != identity.credential_version.0
            || identity.routing_account_hash.0 != request.account_id_hash
            || identity.native_generation_hash != request.access_generation
            || p.source_revision != request.publication_revision
            || p.normalized_revision != request.publication_revision
            || p.publication_request_digest != sha256_hex(actual_request_bytes)
            || p.catalog_disposition != request.catalog.disposition()
        {
            return Err(unavailable());
        }
        if matches!(
            p.catalog_disposition,
            data::ImportCatalogDisposition::Normalized { .. }
        ) {
            if [
                &p.catalog_revision,
                &p.runtime_revision,
                &p.auth_generation_revision,
                &p.paid_control_config_revision,
            ]
            .into_iter()
            .any(|r| r.as_deref() != Some(request.publication_revision.as_str()))
            {
                return Err(unavailable());
            }
        } else if p.catalog_revision.is_none() {
            return Err(unavailable());
        }
        Ok(())
    }
    pub fn parse_for(body: &[u8], request_bytes: &[u8]) -> Result<Self, GatewayError> {
        if body.len() > import_publication_reply_max_bytes() {
            return Err(unavailable());
        }
        let request = command::ImportPublicationCommandData::parse(request_bytes)?;
        let response: Self = serde_json::from_slice(body).map_err(|_| unavailable())?;
        response.validate_for(&request, request_bytes)?;
        Ok(response)
    }
}
/// Derived from the two exact finite success shapes, not an enlarged old cap.
pub fn import_publication_reply_max_bytes() -> usize {
    // This is the closed serde field/tag shell, with empty string contents.
    // Reference/hash fields are ASCII; each retained control unit needs six bytes.
    let shell = concat!(
        r#"{"v":1,"operation_id":"","action_ref":"","disposition":"already_current","#,
        r#""publication":{"v":1,"operation_id":"","claim_ref":"","publication_revision":"","#,
        r#""native_receipt_ref":"","native_source_generation":"","access_generation":"","#,
        r#""credential_version":"","source_revision":"","normalized_revision":"","#,
        r#""catalog_revision":"","runtime_revision":"","auth_generation_revision":"","#,
        r#""paid_control_config_revision":"","publication_request_digest":"","catalog_disposition":"#,
    ).len() + 2;
    let common = shell + 7 * 256 + 5 * 64;
    let authenticated =
        common + 4 * 4096 * 6 + r#"{"kind":"authenticated_limited","upstream_status":429}"#.len();
    let normalized = common + 4 * 256 + command::SNAPSHOT_MAX_BYTES - 2
        + r#"{"kind":"normalized","client_version":"","upstream_status":299}"#.len();
    authenticated.max(normalized)
}
/// Larger success responses qualify only through this exact new operation and
/// complete request/reply binding. Errors retain the old publication cap.
pub fn new_reply_limit(
    request: &[u8],
    status: u16,
    response: &[u8],
) -> Result<Option<usize>, GatewayError> {
    let source = match SourceJson::parse_bytes(request) {
        Ok(source) => source,
        Err(_) => return Ok(None),
    };
    let operation = gateway_source::source_serialization::member(&source, "operation")
        .and_then(|v| v.deserialize::<String>().ok());
    if operation.as_deref() != Some("codex_auth_import_publish") {
        return Ok(None);
    }
    command::ImportPublicationCommandData::parse(request)?;
    if status != 200 {
        return if status >= 400 {
            Ok(None)
        } else {
            Err(unavailable())
        };
    }
    NativeImportPublication::parse_for(response, request)?;
    Ok(Some(import_publication_reply_max_bytes()))
}
