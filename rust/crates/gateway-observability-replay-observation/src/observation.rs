//! Source failure/client observations and pure classification; no store or dispatch effect.
use base64::{Engine, engine::general_purpose::STANDARD};
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use serde::Serialize;
#[derive(Clone, Serialize)]
pub struct FailureObservation {
    pub status: u16,
    pub stream: Option<bool>,
    pub completed: bool,
    pub terminal_type: Option<String>,
    pub failure_kind: Option<String>,
    pub synthetic_terminal_type: Option<String>,
    pub provider_route: String,
}
#[derive(Clone, Serialize)]
pub struct ClientFailureObservation {
    pub status: u16,
    pub stream: bool,
    pub completed: bool,
    pub terminal_type: Option<String>,
    pub failure_kind: Option<String>,
    pub framing_valid: bool,
    pub provider_route: String,
    pub error_code: Option<String>,
    pub error_param: Option<JsString16>,
    pub terminal_body_base64: Option<String>,
    pub terminal_body_truncated: bool,
}
pub fn bounded(s: Option<&str>, param: bool) -> Option<String> {
    s.filter(|s| {
        !s.is_empty()
            && s.len() <= 128
            && s.bytes().all(|b| {
                b.is_ascii_alphanumeric() || b"_.:-".contains(&b) || (param && b"[]".contains(&b))
            })
    })
    .map(str::to_owned)
}
// The stored Sentinel param policy remains the exact existing ASCII whitelist.
// Raw input may contain arbitrary UTF16; rejected units stay only in body evidence.
pub fn bounded_param(value: Option<&JsString16>) -> Option<JsString16> {
    value
        .filter(|value| {
            !value.as_units().is_empty()
                && value.as_units().len() <= 128
                && value.as_units().iter().all(|unit| {
                    u8::try_from(*unit)
                        .is_ok_and(|byte| byte.is_ascii_alphanumeric() || b"_.:-[]".contains(&byte))
                })
        })
        .cloned()
}
fn generic(value: Option<&str>) -> bool {
    value.is_none_or(|v| matches!(v, "server_error" | "error" | "internal_error" | "api_error"))
}
fn upstream_incomplete(value: Option<&str>) -> bool {
    let Some(value) = value else { return false };
    let lower = value.to_ascii_lowercase();
    let s = lower.strip_prefix("response_incomplete:").unwrap_or(&lower);
    [
        "gateway", "provider", "upstream", "server", "network", "timeout", "deadline",
    ]
    .iter()
    .any(|p| s.starts_with(p))
        && s.bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"_.:-".contains(&b))
}
impl ClientFailureObservation {
    pub fn from_internal(internal: &FailureObservation) -> Self {
        let cancelled = internal.terminal_type.as_deref() == Some("cancelled");
        let terminal = if cancelled {
            Some("cancelled".into())
        } else {
            internal.synthetic_terminal_type.clone().or_else(|| {
                match internal.terminal_type.as_deref() {
                    Some(
                        "response.completed" | "response.failed" | "response.incomplete" | "error",
                    ) => internal.terminal_type.clone(),
                    _ if internal.status >= 400 => Some("http.error".into()),
                    _ if internal.completed => Some("http.completed".into()),
                    _ => internal.terminal_type.clone(),
                }
            })
        };
        Self {
            status: internal.status,
            stream: internal.stream.unwrap_or(false),
            completed: internal.completed,
            terminal_type: terminal,
            failure_kind: if cancelled {
                None
            } else {
                internal.failure_kind.clone().or_else(|| {
                    internal
                        .synthetic_terminal_type
                        .as_ref()
                        .map(|_| "server_error".into())
                })
            },
            framing_valid: internal.stream != Some(true),
            provider_route: internal.provider_route.clone(),
            error_code: (!cancelled && internal.synthetic_terminal_type.is_some())
                .then(|| "server_error".into()),
            error_param: None,
            terminal_body_base64: None,
            terminal_body_truncated: false,
        }
    }
    pub fn observed_body(&mut self, bytes: &[u8]) {
        let keep = bytes.len().min(64 * 1024);
        self.terminal_body_base64 = Some(STANDARD.encode(&bytes[..keep]));
        self.terminal_body_truncated = keep < bytes.len();
    }
    pub fn prefer_internal_cause(&mut self, internal: &FailureObservation) {
        if internal.terminal_type.as_deref() == Some("cancelled") {
            self.failure_kind = None;
            self.error_code = None;
            self.error_param = None;
            self.terminal_body_base64 = None;
            self.terminal_body_truncated = false;
            self.terminal_type = Some("cancelled".into());
            return;
        }
        if generic(self.failure_kind.as_deref()) && !generic(internal.failure_kind.as_deref()) {
            self.failure_kind = internal.failure_kind.clone()
        }
    }
    pub fn signature(&self) -> Result<String, GatewayError> {
        #[derive(Serialize)]
        struct Signature<'a> {
            status: u16,
            stream: bool,
            completed: bool,
            terminal_type: &'a Option<String>,
            failure_kind: &'a Option<String>,
            framing_valid: bool,
            provider_route: &'a str,
        }
        Ok(SourceJson::from_serializable(&Signature {
            status: self.status,
            stream: self.stream,
            completed: self.completed,
            terminal_type: &self.terminal_type,
            failure_kind: &self.failure_kind,
            framing_valid: self.framing_valid,
            provider_route: &self.provider_route,
        })?
        .as_str()
        .to_owned())
    }
}
fn persistable(
    status: u16,
    stream: bool,
    completed: bool,
    terminal: Option<&str>,
    failure: Option<&str>,
    synthetic: Option<&str>,
    framing: Option<bool>,
) -> bool {
    if terminal == Some("cancelled") {
        return false;
    }
    if stream && framing == Some(false) {
        return true;
    }
    if status < 400 && completed && terminal == Some("response.completed") {
        return false;
    }
    if status >= 400 {
        return true;
    }
    if terminal == Some("response.incomplete") {
        return upstream_incomplete(failure) || synthetic.is_some();
    }
    failure.is_some()
        || synthetic.is_some()
        || matches!(
            terminal,
            Some("deadline" | "eof" | "error" | "response.failed")
        )
        || (stream && !completed && terminal.is_some())
}
pub fn should_persist(internal: &FailureObservation, client: &ClientFailureObservation) -> bool {
    internal.terminal_type.as_deref() != Some("cancelled")
        && (persistable(
            internal.status,
            internal.stream.unwrap_or(false),
            internal.completed,
            internal.terminal_type.as_deref(),
            internal.failure_kind.as_deref(),
            internal.synthetic_terminal_type.as_deref(),
            None,
        ) || persistable(
            client.status,
            client.stream,
            client.completed,
            client.terminal_type.as_deref(),
            client.failure_kind.as_deref(),
            None,
            Some(client.framing_valid),
        ))
}
pub fn should_signal(internal: &FailureObservation, client: &ClientFailureObservation) -> bool {
    if internal.terminal_type.as_deref() == Some("cancelled") {
        return false;
    }
    if client.stream && !client.framing_valid {
        return true;
    }
    if internal.status < 400
        && internal.completed
        && internal.terminal_type.as_deref() == Some("response.completed")
    {
        return false;
    }
    if internal.status >= 500 || upstream_incomplete(client.failure_kind.as_deref()) {
        return true;
    }
    if internal.terminal_type.as_deref() == Some("response.incomplete") {
        return upstream_incomplete(internal.failure_kind.as_deref())
            || internal.synthetic_terminal_type.is_some()
            || (client.terminal_type.as_deref() == Some("response.incomplete")
                && upstream_incomplete(client.failure_kind.as_deref()));
    }
    internal.synthetic_terminal_type.is_some()
        || internal.failure_kind.is_some()
        || matches!(
            internal.terminal_type.as_deref(),
            Some("deadline" | "eof" | "error")
        )
}
