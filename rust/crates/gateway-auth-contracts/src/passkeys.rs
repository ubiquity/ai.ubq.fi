use gateway_auth_primitives::{crypto::decode_base64url, error::invalid};
use gateway_source::error::GatewayError;
use serde::{Deserialize, Serialize};
use serde_json::Value;
pub const CHALLENGE_TTL_MS: i64 = 300_000;
pub const SESSION_TTL_MS: i64 = 604_800_000;

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct PasskeyUser {
    pub id: String,
    pub handle: String,
    pub is_admin: bool,
    pub credential_ids: Vec<String>,
    pub created_at_ms: i64,
    pub updated_at_ms: i64,
}
#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct PasskeyCredential {
    pub credential_id: String,
    pub user_id: String,
    pub public_key: String,
    pub sign_count: u32,
    pub transports: Vec<String>,
    pub created_at_ms: i64,
}
#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct PasskeySession {
    pub token: String,
    pub user_id: String,
    pub created_at_ms: i64,
    pub expires_at_ms: i64,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub audience_origin: Option<String>,
}
#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct PasskeyChallenge {
    pub challenge: String,
    #[serde(rename = "type")]
    pub kind: String,
    pub origin: String,
    pub rp_id: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub audience_origin: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub user_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub handle: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub is_admin: Option<bool>,
    pub created_at_ms: i64,
    pub expires_at_ms: i64,
}
#[derive(Clone, Debug)]
pub struct SessionView {
    pub session: PasskeySession,
    pub user: PasskeyUser,
}
#[derive(Clone, Debug)]
pub struct RegistrationVerification {
    pub response: Value,
    pub challenge: String,
    pub origin: String,
    pub rp_id: String,
    pub require_user_verification: bool,
}
#[derive(Clone, Debug)]
pub struct VerifiedRegistration {
    pub credential_id: String,
    pub public_key: Vec<u8>,
    pub sign_count: u32,
    pub transports: Vec<String>,
}
#[derive(Clone, Debug)]
pub struct AssertionVerification {
    pub response: Value,
    pub challenge: String,
    pub origin: String,
    pub rp_id: String,
    pub require_user_verification: bool,
    pub credential: PasskeyCredential,
}

pub fn client_data(response: &Value) -> Result<Value, GatewayError> {
    let encoded = response
        .get("response")
        .and_then(|v| v.get("clientDataJSON"))
        .and_then(Value::as_str)
        .ok_or_else(|| invalid("Invalid passkey response"))?;
    let bytes = decode_base64url(encoded).map_err(|_| invalid("Invalid passkey response"))?;
    serde_json::from_slice(&bytes).map_err(|_| invalid("Invalid passkey response"))
}
