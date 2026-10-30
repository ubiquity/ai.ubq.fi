//! Versioned private actor DATA, never a currentness or authentication proof.
use crate::{
    passport::PassportConfig,
    session::{PassportSessionIdentity, PassportSourceClaims},
};
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use serde::{Deserialize, Serialize};

/// Exact local source row binding. No credentials, role/scope snapshot or expiry.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct PassportAppSessionBinding {
    pub session_digest: String,
    pub row_revision: String,
    pub subject: JsString16,
    pub created_at: JsString16,
    pub app_token_digest: String,
    pub client_id: String,
    pub audience: String,
    pub origin: String,
    pub redirect_uri: String,
}
impl PassportAppSessionBinding {
    /// DATA shape only. Date.parse, row currentness and central authority are not inferred here.
    pub fn has_valid_shape(&self) -> bool {
        self.has_valid_shape_for(&PassportConfig::production())
    }
    pub fn has_valid_shape_for(&self, fixed: &PassportConfig) -> bool {
        fixed.validated().is_some()
            && digest_shape(&self.session_digest)
            && digest_shape(&self.app_token_digest)
            && source_bound(&self.subject, 500, false)
            && source_bound(&self.created_at, 100, false)
            && self.client_id == fixed.client_id
            && self.audience == fixed.audience
            && self.origin == fixed.origin
            && self.redirect_uri == fixed.redirect_uri
    }
}
pub fn digest_shape(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}
fn source_bound(value: &JsString16, max: usize, empty: bool) -> bool {
    (empty || !value.as_units().is_empty()) && value.as_units().len() <= max
}

/// Separate lossless JWT provenance; historical native v1 stays scalar and unchanged.
/// Both fields and expiry remain mandatory. Authority alone mints live actor proofs.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct PassportJwtBindingV2 {
    pub sub: JsString16,
    pub jti: JsString16,
    pub expires_at_ms: i64,
}
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum PassportActorDataV2 {
    PassportAppSession(PassportAppSessionBinding),
    PassportJwt(PassportJwtBindingV2),
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Envelope<T> {
    version: u8,
    kind: String,
    data: T,
}
impl PassportActorDataV2 {
    pub fn to_source_json(&self) -> Result<SourceJson, GatewayError> {
        match self {
            Self::PassportAppSession(data) if data.has_valid_shape() => {
                SourceJson::from_serializable(&Envelope {
                    version: 2,
                    kind: "passport_app_session".into(),
                    data,
                })
            }
            Self::PassportJwt(data)
                if source_bound(&data.sub, 500, false) && source_bound(&data.jti, 500, false) =>
            {
                SourceJson::from_serializable(&Envelope {
                    version: 2,
                    kind: "passport_jwt".into(),
                    data,
                })
            }
            _ => Err(invalid()),
        }
    }
    pub fn from_source_json(value: &SourceJson) -> Result<Self, GatewayError> {
        // RawValue keeps source units; internally tagged serde Content must not buffer this DATA.
        let raw: Envelope<SourceJson> = value.deserialize()?;
        if raw.version != 2 {
            return Err(invalid());
        }
        let value = match raw.kind.as_str() {
            "passport_app_session" => Self::PassportAppSession(raw.data.deserialize()?),
            "passport_jwt" => Self::PassportJwt(raw.data.deserialize()?),
            _ => return Err(invalid()),
        };
        value.to_source_json()?;
        Ok(value)
    }
    /// New v2 digest/MAC input only. Never substitute this for historical v1 framing.
    pub fn framed_bytes(&self) -> Result<Vec<u8>, GatewayError> {
        let mut bytes = b"uos_ai:auth:passport_actor:v2\0".to_vec();
        bytes.extend_from_slice(self.to_source_json()?.as_bytes());
        Ok(bytes)
    }
}
fn invalid() -> GatewayError {
    GatewayError::unavailable(
        "Passport actor DATA is invalid.",
        "passport_actor_data_invalid",
    )
}

/// Authenticated service output DATA. It never constructs an Authority currentness value.
#[derive(Clone, Debug, PartialEq)]
pub struct PassportAppSessionData {
    pub identity: PassportSessionIdentity,
    pub binding: PassportAppSessionBinding,
}
#[derive(Clone, Debug, PartialEq)]
pub enum PassportSessionData {
    Jwt(PassportSourceClaims),
    App(PassportAppSessionData),
}
impl PassportSessionData {
    pub fn subject(&self) -> &JsString16 {
        match self {
            Self::Jwt(c) => &c.sub,
            Self::App(c) => &c.identity.subject,
        }
    }
    pub fn handle(&self) -> &JsString16 {
        match self {
            Self::Jwt(c) => &c.handle,
            Self::App(c) => &c.identity.handle,
        }
    }
    pub fn scopes(&self) -> &[JsString16] {
        match self {
            Self::Jwt(c) => &c.scopes,
            Self::App(c) => &c.identity.scopes,
        }
    }
    pub fn is_admin(&self) -> bool {
        match self {
            Self::Jwt(c) => c.is_admin && c.agent_token_id.is_none(),
            Self::App(c) => c.identity.is_admin,
        }
    }
}
