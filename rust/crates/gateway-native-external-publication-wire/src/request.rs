use gateway_auth_credentials::native_credentials::{NativeCredentials, parse_native_document};
use gateway_source::error::GatewayError;
use serde::{Deserialize, Serialize};
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ExternalAccessDocument {
    pub account_id: String,
    pub access_token: String,
    pub user_id: String,
    #[serde(deserialize_with = "required_nullable")]
    pub native_refreshed_at: Option<String>,
}
fn required_nullable<'de, D: serde::Deserializer<'de>>(
    value: D,
) -> Result<Option<String>, D::Error> {
    Option::<String>::deserialize(value)
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct SourceExternalAccessRequest {
    pub operation: String,
    pub v: u8,
    pub action_ref: String,
    pub operation_id: String,
    pub claim_ref: String,
    pub native_owner_id: String,
    pub owner_revision: String,
    pub source_revision: String,
    pub normalized_revision: String,
    pub publication_revision: String,
    pub account_id_hash: String,
    pub credential_version: String,
    pub previous_native_source_generation: String,
    pub native_generation: String,
    pub document: ExternalAccessDocument,
}
pub(crate) fn invalid() -> GatewayError {
    GatewayError::new(
        400,
        "Source external publication is invalid.",
        Some("source_external_publication_invalid"),
    )
}
pub(crate) fn reference(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 256
        && value
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'_' | b'-' | b'.' | b':'))
}
fn generation(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|b| b.is_ascii_digit() || matches!(b, b'a'..=b'f'))
}
pub(crate) fn native(value: &ExternalAccessDocument) -> Result<NativeCredentials, GatewayError> {
    let raw = serde_json::json!({"auth_mode":"chatgptAuthTokens","OPENAI_API_KEY":null,"tokens":{"account_id":value.account_id,"access_token":value.access_token,"id_token":value.access_token,"refresh_token":""},"last_refresh":value.native_refreshed_at});
    let parsed = parse_native_document(&raw)?;
    if parsed.external_user_id.as_deref() != Some(&value.user_id) {
        return Err(invalid());
    }
    Ok(parsed)
}
impl SourceExternalAccessRequest {
    pub fn parse(bytes: &[u8]) -> Result<Self, GatewayError> {
        if bytes.len() > gateway_auth_native_wire::native_read::NATIVE_MAX_BODY_BYTES {
            return Err(invalid());
        }
        let value: Self = serde_json::from_slice(bytes).map_err(|_| invalid())?;
        if value.operation != "source_external_access_publish"
            || value.v != 2
            || [
                &value.action_ref,
                &value.operation_id,
                &value.claim_ref,
                &value.native_owner_id,
                &value.owner_revision,
                &value.source_revision,
                &value.normalized_revision,
                &value.publication_revision,
            ]
            .iter()
            .any(|s| !reference(s))
            || [
                &value.account_id_hash,
                &value.credential_version,
                &value.previous_native_source_generation,
                &value.native_generation,
            ]
            .iter()
            .any(|s| !generation(s))
        {
            return Err(invalid());
        }
        let observed = native(&value.document)?;
        if observed.routing_account_hash() != value.account_id_hash
            || observed.routing_credential_version() != value.credential_version
            || observed.generation_hash() != value.native_generation
        {
            return Err(invalid());
        }
        Ok(value)
    }
}
