//! Explicit source records: only these declared text fields may use ProfileV2 strings.
use crate::{ledger::ErrorObservation, store, types::ObservabilityHost};
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use gateway_core::{
    source_control::decode_source_control_record, source_control::encode_source_control_record,
    source_control::source_json_tree, state::RecordMutation, state::StoredRecord,
};
use gateway_kv_codec::key::RecordKey;
use gateway_observability_request_records::request::bounded_text;
use serde::Serialize;

/// This accessor belongs only to the request/error record schema, never arbitrary history or quota.
pub fn record_source_json(row: &StoredRecord) -> Result<SourceJson, GatewayError> {
    decode_source_control_record(row)
}

pub fn put_record<H: ObservabilityHost>(
    host: &H,
    key: RecordKey,
    document: &SourceJson,
    expiry: i64,
) -> Result<RecordMutation, GatewayError> {
    // Preserve the existing request/error family's lossless scalar-number shape.
    // This is the retained observation row, never a request transcript shadow.
    let document = source_json_tree(document)?
        .to_json_lossless()
        .map(|value| SourceJson::from_serializable(&value))
        .transpose()?
        .unwrap_or_else(|| document.clone());
    let record = encode_source_control_record(store::uuid(host)?, &document, Some(expiry))?;
    Ok(RecordMutation::Put { key, record })
}

pub fn error_document(input: &ErrorObservation) -> Result<SourceJson, GatewayError> {
    #[derive(Serialize)]
    struct ErrorRecord<'a> {
        request_id: &'a str,
        route: &'a str,
        status: u16,
        provider: &'a str,
        model: &'a Option<JsString16>,
        reasoning: &'a Option<JsString16>,
        stream: Option<bool>,
        terminal_type: JsString16,
        failure_kind: JsString16,
        delivery_outcome: gateway_contracts::lifecycle::DeliveryOutcome,
        created_at_ms: i64,
        latency_ms: i64,
        git_sha: &'a Option<String>,
        deno_revision: &'a Option<String>,
        version: u8,
    }
    let terminal = input.terminal_type.as_deref().map(JsString16::from_utf8);
    let terminal = bounded_text(
        terminal.as_ref(),
        160,
        &JsString16::from_utf8(if input.status >= 400 {
            "http.error"
        } else {
            "error"
        }),
    );
    let fallback = if input.status >= 400 {
        JsString16::from_utf8(&format!("http_{}", input.status))
    } else {
        terminal.clone()
    };
    let failure = input.failure_kind.as_deref().map(JsString16::from_utf8);
    let failure = bounded_text(failure.as_ref(), 160, &fallback);
    SourceJson::from_serializable(&ErrorRecord {
        request_id: &input.request_id,
        route: &input.route,
        status: input.status,
        provider: &input.provider,
        model: &input.model,
        reasoning: &input.reasoning,
        stream: input.stream,
        terminal_type: terminal,
        failure_kind: failure,
        delivery_outcome: input.delivery_outcome,
        created_at_ms: input.created_at_ms,
        latency_ms: input.latency_ms,
        git_sha: &input.git_sha,
        deno_revision: &input.deno_revision,
        version: 1,
    })
}

/// Complete source v1 error-log shape validation; preserve original extra fields.
/// This named reader grants neither key custody nor a live operation.
pub fn admin_error_record_source(row: &StoredRecord) -> Result<SourceJson, GatewayError> {
    let document = record_source_json(row)?;
    let object = crate::replay_values::SourceObject::parse(&document)?;
    let field = |name: &str| {
        object
            .get(name)
            .ok_or_else(|| store::error("Incomplete admin error record"))
    };
    if field("version")?.deserialize::<f64>()? != 1.0 {
        return Err(store::error("Invalid admin error version"));
    }
    for name in [
        "request_id",
        "route",
        "provider",
        "terminal_type",
        "failure_kind",
    ] {
        field(name)?.deserialize::<JsString16>()?;
    }
    for name in ["model", "reasoning", "git_sha", "deno_revision"] {
        field(name)?.deserialize::<Option<JsString16>>()?;
    }
    for name in ["status", "created_at_ms", "latency_ms"] {
        field(name)?.deserialize::<f64>()?;
    }
    field("stream")?.deserialize::<Option<bool>>()?;
    field("delivery_outcome")?.deserialize::<gateway_contracts::lifecycle::DeliveryOutcome>()?;
    Ok(document)
}

/// Exact source isPaidFallbackUsageRollup predicate over real UTF16 strings.
/// Data validation only; model/provider membership and accounting authority are external.
pub fn valid_usage_rollup_source(document: &SourceJson) -> bool {
    let field = |name| gateway_source::source_serialization::member(document, name);
    let number = |name| field(name).and_then(|value| value.deserialize::<f64>().ok());
    number("v") == Some(1.0)
        && ["model", "provider"].iter().all(|name| {
            field(name)
                .and_then(|value| value.deserialize::<JsString16>().ok())
                .is_some_and(|value| !value.as_units().is_empty())
        })
        && [
            "bucket_start_at_ms",
            "request_count",
            "input_tokens",
            "cached_input_tokens",
            "output_tokens",
            "first_request_at_ms",
            "last_request_at_ms",
            "updated_at_ms",
        ]
        .iter()
        .all(|name| {
            number(name).is_some_and(|value| {
                value.is_finite()
                    && value.fract() == 0.0
                    && value >= 0.0
                    && value <= gateway_contracts::identity::MAX_SAFE_INTEGER as f64
            })
        })
        && ["quota_sum", "spend_microcredits"]
            .iter()
            .all(|name| number(name).is_some_and(|value| value.is_finite() && value >= 0.0))
}
/// Named rollup family reader; no mutation, expiry renewal or permission is inferred.
pub fn usage_rollup_record_source(row: &StoredRecord) -> Result<SourceJson, GatewayError> {
    let document = decode_source_control_record(row)?;
    if !valid_usage_rollup_source(&document) {
        return Err(store::error("Invalid source usage rollup"));
    }
    Ok(document)
}
