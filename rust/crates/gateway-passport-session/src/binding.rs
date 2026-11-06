//! Fixed client binding and exact token shapes shared by sign-in and session use.
use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
use time::{OffsetDateTime, format_description::well_known::Rfc3339};

/// Local browser secret: `uos_ai_passport_session_` plus 32 random bytes.
pub const LOCAL_TOKEN_PREFIX: &str = "uos_ai_passport_session_";
/// Upstream app grant: `uos_app_session_` plus 43 base64url characters.
pub const APP_TOKEN_PREFIX: &str = "uos_app_session_";
/// Browser persistence is renewed on use; the authoritative store has no expiry.
pub const PASSPORT_COOKIE_MAX_AGE_SECONDS: i64 = 400 * 24 * 60 * 60;

/// The registered client a grant is bound to. Every stored record, upstream
/// request and introspection reply must carry exactly these four values.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Binding {
    pub client_id: String,
    pub audience: String,
    pub origin: String,
    pub redirect_uri: String,
}

impl Binding {
    fn fields(&self) -> [(&'static str, &str); 4] {
        [
            ("clientId", &self.client_id),
            ("audience", &self.audience),
            ("origin", &self.origin),
            ("redirectUri", &self.redirect_uri),
        ]
    }

    /// Strict string equality for every binding field, as the source `===`.
    pub fn matches(&self, value: &Value) -> bool {
        self.fields()
            .iter()
            .all(|(name, expected)| value.get(*name).and_then(Value::as_str) == Some(*expected))
    }

    /// The exact upstream request body: `JSON.stringify(BINDING)` field order.
    pub fn to_json(&self) -> Value {
        json!({
            "clientId": self.client_id,
            "audience": self.audience,
            "origin": self.origin,
            "redirectUri": self.redirect_uri,
        })
    }
}

/// `^[A-Za-z0-9_-]{43}$`: base64url without padding of 32 bytes.
pub fn is_random43(value: &str) -> bool {
    value.len() == 43
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'-')
}

/// `^uos_ai_passport_session_[A-Za-z0-9_-]{43}$`.
pub fn is_persistent_session(token: &str) -> bool {
    token
        .strip_prefix(LOCAL_TOKEN_PREFIX)
        .is_some_and(is_random43)
}

/// `^uos_app_session_[A-Za-z0-9_-]{43}$`.
pub fn is_app_token(token: &str) -> bool {
    token
        .strip_prefix(APP_TOKEN_PREFIX)
        .is_some_and(is_random43)
}

/// Source `boundedString`: a non-empty string of at most `limit` UTF-16 units.
pub fn bounded(value: Option<&Value>, limit: usize) -> Option<&str> {
    value
        .and_then(Value::as_str)
        .filter(|text| !text.is_empty() && text.encode_utf16().count() <= limit)
}

/// A bounded creation timestamp that parses as an RFC 3339 instant. Passport
/// emits `Date#toISOString`; anything else fails closed.
pub fn timestamp(value: Option<&Value>) -> Option<&str> {
    bounded(value, 100).filter(|text| OffsetDateTime::parse(text, &Rfc3339).is_ok())
}

pub fn base64url(bytes: &[u8]) -> String {
    URL_SAFE_NO_PAD.encode(bytes)
}

pub fn sha256_hex(bytes: &[u8]) -> String {
    Sha256::digest(bytes)
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect()
}

/// PKCE S256: `base64url(SHA-256(ascii(verifier)))`.
pub fn pkce_challenge(verifier: &str) -> String {
    base64url(&Sha256::digest(verifier.as_bytes()))
}
