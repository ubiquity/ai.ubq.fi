//! Exact source request-log normalization and lossless passive HTTP failure DATA.
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use serde::Serialize;
use serde_json::Value;
use std::collections::BTreeMap;

pub fn trim(value: &JsString16) -> JsString16 {
    let whitespace =
        |v: &u16| char::from_u32(u32::from(*v)).is_some_and(gateway_schema::util::js_whitespace);
    let units = value.as_units();
    let start = units
        .iter()
        .position(|v| !whitespace(v))
        .unwrap_or(units.len());
    let end = units
        .iter()
        .rposition(|v| !whitespace(v))
        .map_or(start, |i| i + 1);
    JsString16::from_units(units[start..end].to_vec())
}
pub fn bounded_text(
    value: Option<&JsString16>,
    maximum: usize,
    fallback: &JsString16,
) -> JsString16 {
    let value = value
        .map(trim)
        .filter(|v| !v.as_units().is_empty())
        .unwrap_or_else(|| fallback.clone());
    value.prefix_units(maximum)
}
fn uppercase(value: JsString16) -> JsString16 {
    let mut out = Vec::new();
    for ch in char::decode_utf16(value.as_units().iter().copied()) {
        match ch {
            Ok(ch) => {
                for upper in ch.to_uppercase() {
                    let mut buffer = [0; 2];
                    out.extend_from_slice(upper.encode_utf16(&mut buffer));
                }
            }
            Err(error) => out.push(error.unpaired_surrogate()),
        }
    }
    JsString16::from_units(out)
}
fn fields(value: &SourceJson) -> Result<Option<BTreeMap<String, SourceJson>>, GatewayError> {
    let value = SourceJson::parse(
        value
            .as_str()
            .trim_matches(|c| matches!(c, ' ' | '\t' | '\r' | '\n')),
    )?;
    if value.as_str().starts_with('[') {
        return Ok(Some(BTreeMap::new()));
    }
    if !value.as_str().starts_with('{') {
        return Ok(None);
    }
    let mut fields = BTreeMap::new();
    for (key, value) in value.object_entries()? {
        if let Ok(key) = key.try_utf8() {
            fields.insert(key, value);
        }
    }
    Ok(Some(fields))
}
pub fn number(value: Option<&SourceJson>, fallback: f64) -> f64 {
    value
        .and_then(|v| v.deserialize::<f64>().ok())
        .filter(|n| n.is_finite())
        .map_or(fallback, f64::trunc)
}
fn optional_number(value: Option<&SourceJson>) -> Option<f64> {
    let n = number(value, -1.0);
    (n >= 0.0).then_some(n)
}
fn text(
    values: &BTreeMap<String, SourceJson>,
    name: &str,
    max: usize,
    fallback: &str,
) -> JsString16 {
    let value = values
        .get(name)
        .and_then(|v| v.deserialize::<JsString16>().ok());
    bounded_text(value.as_ref(), max, &JsString16::from_utf8(fallback))
}
fn optional_text(values: &BTreeMap<String, SourceJson>, name: &str) -> Option<JsString16> {
    let v = text(values, name, 120, "");
    (!v.as_units().is_empty()).then_some(v)
}

#[derive(Clone, Debug, Serialize)]
pub struct RequestLogRecord {
    pub id: JsString16,
    pub key_id: JsString16,
    pub route: JsString16,
    pub path: JsString16,
    pub method: JsString16,
    pub status_code: f64,
    pub stream: bool,
    pub model: Option<JsString16>,
    pub reasoning: Option<JsString16>,
    pub created_at_ms: f64,
    pub provider: String,
    pub fallback_reason: Option<JsString16>,
    pub provider_request_id: Option<JsString16>,
    pub billing_status: String,
    pub completed_at_ms: Option<f64>,
    pub latency_ms: Option<f64>,
    pub input_tokens: Option<f64>,
    pub output_tokens: Option<f64>,
    pub provider_quota: Option<f64>,
    pub quota_per_credit: Option<f64>,
    pub spend_microcredits: Option<f64>,
    pub paid_fallback_window_reset_at_ms: Option<f64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub native_http_failure: Option<SourceJson>,
}
impl RequestLogRecord {
    pub fn normalize(
        value: &SourceJson,
        key_id: &str,
        request_id: &JsString16,
        created: f64,
    ) -> Result<Option<Self>, GatewayError> {
        let Some(v) = fields(value)? else {
            return Ok(None);
        };
        let provider = v
            .get("provider")
            .and_then(|v| v.deserialize::<String>().ok())
            .filter(|v| {
                matches!(
                    v.as_str(),
                    "cerebras"
                        | "deepseek"
                        | "lithos"
                        | "openrouter"
                        | "voyage"
                        | "surplus"
                        | "metered"
                )
            })
            .unwrap_or_else(|| "chatgpt_codex".into());
        let billing = v
            .get("billing_status")
            .and_then(|v| v.deserialize::<String>().ok())
            .filter(|v| {
                matches!(
                    v.as_str(),
                    "pending" | "reconciled" | "not_billed" | "unresolved"
                )
            })
            .unwrap_or_else(|| "not_applicable".into());
        Ok(Some(Self {
            id: {
                let id = v.get("id").and_then(|v| v.deserialize::<JsString16>().ok());
                bounded_text(id.as_ref(), 120, request_id)
            },
            key_id: text(&v, "key_id", 200, key_id),
            route: text(&v, "route", 120, "unknown"),
            path: text(&v, "path", 1024, ""),
            method: uppercase(text(&v, "method", 16, "UNKNOWN")),
            status_code: {
                let n = number(v.get("status_code"), 0.0);
                if n <= 0.0 { 0.0 } else { n.min(599.0) }
            },
            stream: v.get("stream").and_then(|v| v.deserialize::<bool>().ok()) == Some(true),
            model: optional_text(&v, "model"),
            reasoning: optional_text(&v, "reasoning"),
            created_at_ms: number(v.get("created_at_ms"), created),
            provider,
            fallback_reason: optional_text(&v, "fallback_reason"),
            provider_request_id: optional_text(&v, "provider_request_id"),
            billing_status: billing,
            completed_at_ms: optional_number(v.get("completed_at_ms")),
            latency_ms: optional_number(v.get("latency_ms")),
            input_tokens: optional_number(v.get("input_tokens")),
            output_tokens: optional_number(v.get("output_tokens")),
            provider_quota: optional_number(v.get("provider_quota")),
            quota_per_credit: optional_number(v.get("quota_per_credit")),
            spend_microcredits: optional_number(v.get("spend_microcredits")),
            paid_fallback_window_reset_at_ms: optional_number(
                v.get("paid_fallback_window_reset_at_ms"),
            ),
            native_http_failure: v.get("native_http_failure").cloned(),
        }))
    }
    pub fn source_json(&self) -> Result<SourceJson, GatewayError> {
        SourceJson::from_serializable(self)
    }
}
pub fn merge_request_fields(
    existing: Option<&RequestLogRecord>,
    patch: &Value,
    key_id: &str,
    request_id: &JsString16,
    created: f64,
) -> Result<RequestLogRecord, GatewayError> {
    merge_request_fields_source(
        existing,
        &SourceJson::from_serializable(patch)?,
        key_id,
        request_id,
        created,
    )
}
pub fn merge_request_fields_source(
    existing: Option<&RequestLogRecord>,
    patch: &SourceJson,
    key_id: &str,
    request_id: &JsString16,
    created: f64,
) -> Result<RequestLogRecord, GatewayError> {
    let mut values = match existing {
        Some(v) => fields(&v.source_json()?)?
            .ok_or_else(|| invalid_record("Invalid typed request object"))?,
        None => BTreeMap::new(),
    };
    if let Some(patch) = fields(patch)? {
        values.extend(patch);
    }
    values.insert("id".into(), SourceJson::from_serializable(request_id)?);
    values.insert("key_id".into(), SourceJson::from_serializable(key_id)?);
    values.insert(
        "created_at_ms".into(),
        SourceJson::from_serializable(&created)?,
    );
    RequestLogRecord::normalize(
        &SourceJson::from_serializable(&values)?,
        key_id,
        request_id,
        created,
    )?
    .ok_or_else(|| invalid_record("Invalid typed request record"))
}
fn invalid_record(message: &str) -> GatewayError {
    GatewayError::unavailable(message, "observability_state_invalid")
}
