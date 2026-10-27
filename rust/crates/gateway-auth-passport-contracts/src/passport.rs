use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use url::Url;
pub const PASSPORT_ISSUER: &str = "https://passport.ubq.fi";
pub const PASSPORT_AUTHORIZE_URL: &str = "https://passport.ubq.fi/authorize";
pub const PASSPORT_EXCHANGE_URL: &str = "https://passport.ubq.fi/api/auth/sso/exchange";
pub const PASSPORT_JWKS_URL: &str = "https://passport.ubq.fi/api/auth/jwks";
pub const PASSPORT_PRODUCTION_CLIENT_ID: &str = "ai";
pub const PASSPORT_PRODUCTION_AUDIENCE: &str = "ai";
pub const PASSPORT_PRODUCTION_ORIGIN: &str = "https://ai.ubq.fi";
pub const PASSPORT_PRODUCTION_REDIRECT_URI: &str = "https://ai.ubq.fi/auth/callback";
pub const PASSPORT_COOKIE: &str = "__Host-uos_ai_passport_session";
pub const PASSPORT_STATE_COOKIE: &str = "__Host-uos_ai_passport_state";
pub const PASSPORT_STATE_TTL_SECONDS: i64 = 600;
pub const PASSPORT_JWKS_CACHE_MS: i64 = 300_000;
pub const PASSPORT_JWKS_MAX_BYTES: usize = 262_144;
pub const PASSPORT_EXCHANGE_MAX_BYTES: usize = 65_536;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PassportConfig {
    pub client_id: String,
    pub audience: String,
    pub origin: String,
    pub redirect_uri: String,
}

impl PassportConfig {
    pub fn production() -> Self {
        Self {
            client_id: PASSPORT_PRODUCTION_CLIENT_ID.into(),
            audience: PASSPORT_PRODUCTION_AUDIENCE.into(),
            origin: PASSPORT_PRODUCTION_ORIGIN.into(),
            redirect_uri: PASSPORT_PRODUCTION_REDIRECT_URI.into(),
        }
    }

    /// Explicit preview registration only. Partial or non-HTTPS values disable
    /// browser login rather than silently reusing the production `ai` client.
    pub fn from_values(values: &BTreeMap<String, String>) -> Option<Self> {
        let value = |name: &str| {
            values
                .get(name)
                .map(|value| value.trim())
                .filter(|value| !value.is_empty())
        };
        let client_id = value("UOS_AI_PASSPORT_CLIENT_ID")?;
        let audience = value("UOS_AI_PASSPORT_AUDIENCE").unwrap_or(PASSPORT_PRODUCTION_AUDIENCE);
        let origin = value("UOS_AI_PASSPORT_ORIGIN")?;
        let redirect_uri = value("UOS_AI_PASSPORT_REDIRECT_URI")?;
        if !valid_https_origin(origin) || client_id.len() > 120 || audience.len() > 160 {
            return None;
        }
        let redirect = Url::parse(redirect_uri).ok()?;
        if redirect.scheme() != "https"
            || redirect.origin().ascii_serialization() != origin.trim_end_matches('/')
            || redirect.query().is_some()
            || redirect.fragment().is_some()
        {
            return None;
        }
        Some(Self {
            client_id: client_id.into(),
            audience: audience.into(),
            origin: origin.trim_end_matches('/').into(),
            redirect_uri: redirect_uri.into(),
        })
    }
}

fn valid_https_origin(value: &str) -> bool {
    let Ok(url) = Url::parse(value) else {
        return false;
    };
    url.scheme() == "https"
        && url.port().is_none()
        && url.host_str().is_some()
        && url.path() == "/"
        && url.query().is_none()
        && url.fragment().is_none()
        && url.origin().ascii_serialization() == value.trim_end_matches('/')
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct PassportClaims {
    pub iss: String,
    pub sub: String,
    #[serde(default)]
    pub handle: String,
    #[serde(rename = "isAdmin")]
    pub is_admin: bool,
    #[serde(default)]
    pub scopes: Vec<String>,
    pub aud: String,
    #[serde(rename = "clientId")]
    pub client_id: String,
    pub auth: String,
    pub iat: i64,
    pub exp: i64,
    pub jti: String,
    #[serde(
        rename = "agentTokenId",
        default,
        skip_serializing_if = "Option::is_none"
    )]
    pub agent_token_id: Option<String>,
}

impl PassportClaims {
    pub fn valid(&self, config: &PassportConfig, now_seconds: i64) -> bool {
        self.iss == PASSPORT_ISSUER
            && self.aud == config.audience
            && self.client_id == config.client_id
            && !self.sub.is_empty()
            && self.sub.len() <= 500
            && !self.jti.is_empty()
            && self.jti.len() <= 500
            && !self.auth.is_empty()
            && self.auth.len() <= 160
            && self.handle.len() <= 500
            && self.scopes.len() <= 128
            && self.scopes.iter().all(|scope| scope.len() <= 500)
            && self.exp > now_seconds
            && self.iat <= now_seconds.saturating_add(60)
            && self.exp > self.iat
            && self
                .agent_token_id
                .as_ref()
                .is_none_or(|id| !id.is_empty() && id.len() <= 500)
    }
}
