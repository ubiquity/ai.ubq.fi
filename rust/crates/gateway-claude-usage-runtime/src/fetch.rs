use gateway_auth::types::AuthHost;
use gateway_claude_roster::claude::{AccountBinding, CredentialKind};
use gateway_claude_usage_protocol::{snapshot, state::UsageState};
use gateway_claude_usage_quota::{QuotaObservation, QuotaWindow};
use gateway_core::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use std::{collections::BTreeMap, future::Future};

pub const URL: &str = "https://api.anthropic.com/api/oauth/usage";
pub const USER_AGENT: &str = "claude-code/2.1.310";
pub const TIMEOUT_MS: i32 = 8_000;
/// Response text is read completely by the real platform. No new streaming/body quota.
pub struct Response {
    pub status: u16,
    pub body: Result<JsString16, GatewayError>,
    pub retry_after: Option<String>,
    pub observed_at_ms: i64,
}
pub enum GetFailure {
    Setup(GatewayError),
    Unreachable(Option<JsString16>),
}
pub trait UsageHost: AuthHost {
    fn parse_usage_date(&self, value: &JsString16) -> Option<i64>;
    /// Only the fixed GET, source OAuth headers, manual redirect and 8s/caller cancellation.
    /// A transport failure is a sanitized source message, never credential or pool proof.
    fn usage_get(
        &self,
        account: &AccountBinding,
    ) -> impl Future<Output = Result<Response, GetFailure>>;
    fn accepted_usage(
        &self,
        event: &gateway_claude_usage_quota::observation::AcceptedObservation,
    ) -> impl Future<Output = Result<(), GatewayError>>;
}
#[derive(Clone, Debug)]
pub struct Failure {
    pub kind: &'static str,
    pub status: Option<u16>,
    pub observed_at_ms: i64,
    pub message: Option<JsString16>,
    pub retry_after_ms: Option<f64>,
}
#[derive(Clone, Debug)]
pub enum ResultData {
    Success {
        status: u16,
        observed_at_ms: i64,
        observation: QuotaObservation,
    },
    Failure(Failure),
}
impl ResultData {
    pub fn usage(&self) -> UsageState {
        let (state, status, at, message, observed) = match self {
            Self::Success {
                status,
                observed_at_ms,
                ..
            } => (
                "ok",
                Some(*status),
                *observed_at_ms,
                None,
                Some(*observed_at_ms),
            ),
            Self::Failure(f) => (f.kind, f.status, f.observed_at_ms, f.message.clone(), None),
        };
        UsageState {
            source: "oauth_usage".into(),
            state: state.into(),
            status,
            message,
            attempted_at_ms: at,
            observed_at_ms: observed,
        }
    }
    pub fn observation(&self) -> Option<&QuotaObservation> {
        match self {
            Self::Success { observation, .. } => Some(observation),
            _ => None,
        }
    }
}
fn fail(
    kind: &'static str,
    status: Option<u16>,
    at: i64,
    message: Option<JsString16>,
    retry_after_ms: Option<f64>,
) -> ResultData {
    ResultData::Failure(Failure {
        kind,
        status,
        observed_at_ms: at,
        message,
        retry_after_ms,
    })
}
pub fn observation(
    payload: &SourceJson,
    at: i64,
    dates: &impl Fn(&JsString16) -> Option<i64>,
) -> Option<QuotaObservation> {
    let snapshot = snapshot::parse(payload, at, dates)?;
    let mut out = QuotaObservation::empty(at);
    for (key, id) in [("five_hour", "5h"), ("seven_day", "7d")] {
        if let Some(row) = source::member(&snapshot.payload, key).filter(|v| v.as_str() != "null") {
            let reset = source::member(&row, "resets_at")
                .as_ref()
                .and_then(source::string)
                .as_ref()
                .and_then(dates)
                .filter(|at| *at > 0 && *at <= gateway_core::identity::MAX_SAFE_INTEGER);
            let meter = snapshot
                .meters
                .iter()
                .find(|m| m.kind == JsString16::from_utf8(key));
            out.windows.insert(
                id.into(),
                QuotaWindow {
                    status: None,
                    utilization: source::member(&row, "utilization")
                        .as_ref()
                        .and_then(source::number)
                        .map(|n| (n / 100.0).clamp(0.0, 1.0)),
                    reset_at_ms: meter
                        .and_then(|m| m.reset_at_ms)
                        .or(reset)
                        .map(|n| n as f64),
                    observed_at_ms: at as f64,
                },
            );
        }
    }
    out.usage_snapshot = Some(snapshot::serialize(&snapshot).ok()?);
    Some(out)
}
pub fn headers(secret: &str) -> BTreeMap<String, String> {
    BTreeMap::from([
        ("Authorization".into(), format!("Bearer {secret}")),
        ("Accept".into(), "application/json".into()),
        ("Content-Type".into(), "application/json".into()),
        ("anthropic-beta".into(), "oauth-2025-04-20".into()),
        ("anthropic-version".into(), "2023-06-01".into()),
        ("User-Agent".into(), USER_AGENT.into()),
    ])
}
fn error_message(text: &JsString16) -> Option<JsString16> {
    let bounded = text.prefix_units(4096);
    let value = bounded
        .try_utf8()
        .ok()
        .and_then(|s| SourceJson::parse(&s).ok());
    let message = value
        .as_ref()
        .and_then(|v| source::member(v, "error"))
        .and_then(|v| source::member(&v, "message"))
        .as_ref()
        .and_then(source::string)
        .or_else(|| {
            value
                .as_ref()
                .and_then(|v| source::member(v, "message"))
                .as_ref()
                .and_then(source::string)
        })
        .unwrap_or(bounded);
    message.sanitize_js_whitespace_prefix(240)
}
fn classification(status: u16, message: Option<&JsString16>) -> &'static str {
    let text = message
        .map(|m| String::from_utf16_lossy(m.as_units()).to_ascii_lowercase())
        .unwrap_or_default();
    let scope = text.contains("user:profile")
        || ["missing", "required", "insufficient"].iter().any(|word| {
            text.find(word)
                .zip(text.rfind("scope"))
                .is_some_and(|(a, b)| a < b)
                || text
                    .find("scope")
                    .zip(text.rfind(word))
                    .is_some_and(|(a, b)| a < b)
        });
    if scope || status == 403 {
        "scope_missing"
    } else if status == 401 {
        "auth_invalid"
    } else if status == 429 {
        "rate_limited"
    } else if status >= 500 {
        "upstream_error"
    } else {
        "http_error"
    }
}
fn retry_after(
    value: Option<&str>,
    at: i64,
    dates: &impl Fn(&JsString16) -> Option<i64>,
) -> Option<f64> {
    let value = value.filter(|v| !v.is_empty())?;
    let n = gateway_profile_values::coercion::js_number(&serde_json::Value::String(value.into()));
    if n.is_finite() && n >= 0.0 {
        return Some((n * 1000.0).trunc());
    }
    dates(&JsString16::from_utf8(value))
        .filter(|at| *at > 0)
        .map(|date| ((date as f64) - (at as f64)).max(0.0))
}
pub async fn fetch<H: UsageHost>(
    host: &H,
    account: &AccountBinding,
) -> Result<ResultData, GatewayError> {
    if account.credential.kind != CredentialKind::Oauth {
        return Ok(fail("unsupported_credential",None,host.now_ms(),Some("Claude Console API keys do not expose the OAuth usage endpoint; gateway traffic continues to update the meter.".into()),None));
    }
    let response = match host.usage_get(account).await {
        Ok(v) => v,
        Err(GetFailure::Setup(error)) => return Err(error),
        Err(GetFailure::Unreachable(message)) => {
            return Ok(fail(
                "unreachable",
                None,
                host.now_ms(),
                message.and_then(|m| m.sanitize_js_whitespace_prefix(240)),
                None,
            ));
        }
    };
    let at = response.observed_at_ms;
    if !(200..300).contains(&response.status) {
        let message = response.body.ok().as_ref().and_then(error_message);
        let kind = classification(response.status, message.as_ref());
        let message=message.or_else(||(kind=="scope_missing").then(||"Claude OAuth credential cannot read usage because the user:profile scope is missing.".into()));
        let retry = if kind == "rate_limited" {
            retry_after(response.retry_after.as_deref(), at, &|v| {
                host.parse_usage_date(v)
            })
        } else {
            None
        };
        return Ok(fail(kind, Some(response.status), at, message, retry));
    }
    let payload = response
        .body
        .ok()
        .and_then(|v| v.try_utf8().ok())
        .and_then(|v| SourceJson::parse(&v).ok());
    let Some(payload) = payload else {
        return Ok(fail(
            "invalid_response",
            Some(response.status),
            at,
            Some("Claude usage returned an unreadable JSON payload.".into()),
            None,
        ));
    };
    let Some(observation) = observation(&payload, at, &|v| host.parse_usage_date(v)) else {
        return Ok(fail(
            "invalid_response",
            Some(response.status),
            at,
            Some("Claude usage returned an invalid or unrecognized meter payload.".into()),
            None,
        ));
    };
    Ok(ResultData::Success {
        status: response.status,
        observed_at_ms: at,
        observation,
    })
}
