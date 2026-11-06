//! The stored session record and its source-exact key tuple.
use crate::binding::{Binding, bounded, is_app_token, sha256_hex, timestamp};
use gateway_core::{error::GatewayError, state::RecordKey, tuple_key::TupleKey};
use serde::Serialize;
use serde_json::{Value, json};

/// `["uos_ai", "auth", "passport", "sessions", sha256Hex(localToken)]`. The
/// local secret itself never appears in a key or a record.
pub fn session_key(token: &str) -> Result<RecordKey, GatewayError> {
    let parts = [
        json!("uos_ai"),
        json!("auth"),
        json!("passport"),
        json!("sessions"),
        json!(sha256_hex(token.as_bytes())),
    ];
    TupleKey::from_json_parts(&parts).map(|key| key.to_record_key())
}

/// Source `SessionRecord`, serialized in the source field order. It holds a
/// bearer grant, so it deliberately has no `Debug`.
#[derive(Clone, PartialEq, Serialize)]
pub struct SessionRecord {
    pub version: u8,
    pub subject: String,
    #[serde(rename = "clientId")]
    pub client_id: String,
    pub audience: String,
    pub origin: String,
    #[serde(rename = "redirectUri")]
    pub redirect_uri: String,
    #[serde(rename = "createdAt")]
    pub created_at: String,
    #[serde(rename = "appSessionToken")]
    pub app_session_token: String,
}

impl SessionRecord {
    pub fn new(subject: &str, binding: &Binding, created_at: &str, grant: &str) -> Self {
        Self {
            version: 1,
            subject: subject.into(),
            client_id: binding.client_id.clone(),
            audience: binding.audience.clone(),
            origin: binding.origin.clone(),
            redirect_uri: binding.redirect_uri.clone(),
            created_at: created_at.into(),
            app_session_token: grant.into(),
        }
    }

    /// Source `validRecord`: any other shape is corrupt state, not absence.
    pub fn parse(value: &Value, binding: &Binding) -> Option<Self> {
        let subject = bounded(value.get("subject"), 500)?;
        let created_at = timestamp(value.get("createdAt"))?;
        let grant = value
            .get("appSessionToken")
            .and_then(Value::as_str)
            .filter(|token| is_app_token(token))?;
        (value.get("version").and_then(Value::as_f64) == Some(1.0) && binding.matches(value))
            .then(|| Self::new(subject, binding, created_at, grant))
    }

    pub fn to_json(&self) -> Result<Value, GatewayError> {
        serde_json::to_value(self).map_err(|_| {
            GatewayError::new(
                503,
                "Passport verification is unavailable.",
                Some("passport_unavailable"),
            )
        })
    }
}
