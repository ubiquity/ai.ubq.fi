// Persistent identity DATA is separate from strictly expiring JWT claims.
use gateway_source::js_string::JsString16;
use serde::{Deserialize, Serialize};

pub const PASSPORT_COOKIE_MAX_AGE_SECONDS: i64 = 400 * 24 * 60 * 60;
pub const PASSPORT_VERIFIER_COOKIE: &str = "__Host-uos_ai_passport_verifier";
pub const PASSPORT_SESSION_URL: &str = "https://passport.ubq.fi/api/auth/sso/session";
pub const PASSPORT_REVOKE_URL: &str = "https://passport.ubq.fi/api/auth/sso/session/revoke";

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct PassportSessionIdentity {
    pub subject: JsString16,
    pub handle: JsString16,
    #[serde(rename = "isAdmin")]
    pub is_admin: bool,
    pub scopes: Vec<JsString16>,
}

/// The source's Date.parse primitive. Returns NaN for invalid dates, never a
/// policy decision or proof. Production JS hosts must use their actual engine.
pub trait PassportDate {
    fn parse_date_ms(&self, value: &JsString16) -> f64;
}

/// Source JWT DATA. Expiration stays mandatory; this is never a persistent SSO
/// record and there is no public authenticity constructor.
#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PassportSourceClaims {
    pub iss: String,
    pub sub: JsString16,
    #[serde(default)]
    pub handle: JsString16,
    pub is_admin: bool,
    #[serde(default)]
    pub scopes: Vec<JsString16>,
    pub aud: String,
    pub client_id: String,
    pub auth: JsString16,
    pub iat: i64,
    pub exp: i64,
    pub jti: JsString16,
    #[serde(default)]
    pub agent_token_id: Option<JsString16>,
}
impl PassportSourceClaims {
    pub fn valid(&self, now: i64) -> bool {
        let bound = |value: &JsString16, max, empty| {
            (empty || !value.as_units().is_empty()) && value.as_units().len() <= max
        };
        let safe = gateway_core::identity::MAX_SAFE_INTEGER;
        self.iss == "https://passport.ubq.fi"
            && self.aud == "ai"
            && self.client_id == "ai"
            && bound(&self.sub, 500, false)
            && bound(&self.jti, 500, false)
            && bound(&self.auth, 160, false)
            && bound(&self.handle, 500, true)
            && self.scopes.len() <= 128
            && self.scopes.iter().all(|scope| bound(scope, 500, true))
            && (-safe..=safe).contains(&self.iat)
            && (-safe..=safe).contains(&self.exp)
            && self.exp > now
            && self.exp > self.iat
            && self.iat <= now.saturating_add(60)
            && self
                .agent_token_id
                .as_ref()
                .is_none_or(|id| bound(id, 500, false))
    }
}

/// JSON.parse after TextDecoder's replacement/BOM behavior, followed by the
/// existing lossless JS JSON normalization (including last duplicate fields).
pub fn parse_source_json(
    bytes: &[u8],
) -> Result<gateway_source::source_json::SourceJson, gateway_source::error::GatewayError> {
    let text = String::from_utf8_lossy(bytes);
    let value = gateway_source::source_json::SourceJson::parse(
        text.strip_prefix('\u{feff}').unwrap_or(&text),
    )?;
    gateway_source::source_serialization::canonical(&value)
}

impl PassportSourceClaims {
    /// Historical scalar DATA conversion only; never the new authentication parser.
    pub fn from_legacy(value: crate::passport::PassportClaims) -> Self {
        Self {
            iss: value.iss,
            sub: JsString16::from_utf8(&value.sub),
            handle: JsString16::from_utf8(&value.handle),
            is_admin: value.is_admin,
            scopes: value
                .scopes
                .iter()
                .map(|v| JsString16::from_utf8(v))
                .collect(),
            aud: value.aud,
            client_id: value.client_id,
            auth: JsString16::from_utf8(&value.auth),
            iat: value.iat,
            exp: value.exp,
            jti: JsString16::from_utf8(&value.jti),
            agent_token_id: value.agent_token_id.as_deref().map(JsString16::from_utf8),
        }
    }
    /// Explicit legacy serde boundary. Source authentication must retain self's units.
    pub fn try_legacy_scalar(
        &self,
    ) -> Result<crate::passport::PassportClaims, gateway_source::error::GatewayError> {
        Ok(crate::passport::PassportClaims {
            iss: self.iss.clone(),
            sub: self.sub.try_utf8()?,
            handle: self.handle.try_utf8()?,
            is_admin: self.is_admin,
            scopes: self
                .scopes
                .iter()
                .map(JsString16::try_utf8)
                .collect::<Result<_, _>>()?,
            aud: self.aud.clone(),
            client_id: self.client_id.clone(),
            auth: self.auth.try_utf8()?,
            iat: self.iat,
            exp: self.exp,
            jti: self.jti.try_utf8()?,
            agent_token_id: self
                .agent_token_id
                .as_ref()
                .map(JsString16::try_utf8)
                .transpose()?,
        })
    }
}
