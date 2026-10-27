use crate::snapshot::safe_time;
use gateway_source::{
    js_string::JsString16, source_json::SourceJson, source_serialization as source,
};
use serde::{Deserialize, Serialize};

pub const FRESH_MS: i64 = 30_000;
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct UsageState {
    pub source: String,
    pub state: String,
    pub status: Option<u16>,
    pub message: Option<JsString16>,
    pub attempted_at_ms: i64,
    pub observed_at_ms: Option<i64>,
}
impl UsageState {
    pub fn fresh(&self, now: i64) -> bool {
        now >= self.attempted_at_ms && now - self.attempted_at_ms < FRESH_MS
    }
}
pub fn read(value: &SourceJson) -> Option<UsageState> {
    let state = source::member(value, "state")
        .as_ref()
        .and_then(source::string)?
        .try_utf8()
        .ok()?;
    if source::member(value, "source")
        .as_ref()
        .and_then(source::string)
        != Some("oauth_usage".into())
        || ![
            "ok",
            "scope_missing",
            "auth_invalid",
            "rate_limited",
            "unsupported_credential",
            "unreachable",
            "upstream_error",
            "http_error",
            "invalid_response",
        ]
        .contains(&state.as_str())
    {
        return None;
    }
    let attempted = source::member(value, "attempted_at_ms")
        .as_ref()
        .and_then(source::number)?;
    let observed = source::member(value, "observed_at_ms")?;
    let observed = if observed.as_str() == "null" {
        None
    } else {
        let n = source::number(&observed)?;
        if !safe_time(n) || n <= 0.0 {
            return None;
        }
        Some(n as i64)
    };
    let status = source::member(value, "status")?;
    let status = if status.as_str() == "null" {
        None
    } else {
        let n = source::number(&status)?;
        if n.fract() != 0.0 || !(100.0..=599.0).contains(&n) {
            return None;
        }
        Some(n as u16)
    };
    let message = source::member(value, "message")?;
    let message = if message.as_str() == "null" {
        None
    } else {
        Some(source::string(&message)?)
    };
    if !safe_time(attempted) || attempted <= 0.0 {
        return None;
    }
    Some(UsageState {
        source: "oauth_usage".into(),
        state,
        status,
        message: message.and_then(|m| m.sanitize_js_whitespace_prefix(240)),
        attempted_at_ms: attempted as i64,
        observed_at_ms: observed,
    })
}
