//! Fixed Voyage dispatch/response operations shared by sync and poll-driven jobs.
//! Uses only the original factory-only PreparedCall; retry/header parsing is private.
use chrono::{DateTime, Utc};
use gateway_extras_contract::errors::server;
use gateway_extras_contract::types::{
    DispatchFailure, DispatchFailureOrigin, ExtrasDispatch, ExtrasHost, ExtrasProvider, Headers,
    PreparedCall,
};
use gateway_providers::extras::{self, EmbeddingsProfile};
use gateway_providers::types::PrepareOptions;
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use serde_json::{Value, json};
pub fn configured_error() -> GatewayError {
    GatewayError::unavailable(
        "Embeddings provider is not configured: set VOYAGEAI_API_KEY (or store it in Deno KV)",
        "server_error",
    )
    .with_param(None)
}
fn retry_after(headers: &Headers, at: i64) -> Option<u64> {
    let value = headers
        .iter()
        .find(|(k, _)| k.eq_ignore_ascii_case("Retry-After"))?
        .1
        .trim();
    if let Ok(seconds) = value.parse::<f64>()
        && seconds.is_finite()
        && seconds > 0.0
    {
        return Some((seconds * 1000.0).trunc().min(60_000.0) as u64);
    }
    let when = DateTime::parse_from_rfc2822(value)
        .ok()?
        .with_timezone(&Utc)
        .timestamp_millis();
    (when > at).then_some((when - at).min(60_000) as u64)
}
pub fn retryable(status: u16) -> bool {
    matches!(status, 429 | 500 | 502 | 503 | 504)
}
pub async fn fetch_chunk<H: ExtrasHost>(
    host: &H,
    profile: &EmbeddingsProfile,
    inputs: &[JsString16],
    options: &PrepareOptions,
    deadline: i64,
) -> Result<(Vec<Vec<f64>>, Option<u64>), DispatchFailure> {
    #[derive(serde::Serialize)]
    struct Chunk<'a> {
        model: &'a str,
        input: &'a [JsString16],
        input_type: &'a str,
        dimensions: u64,
        encoding_format: &'a str,
        truncation: bool,
    }
    let raw = SourceJson::from_serializable(&Chunk {
        model: &profile.model,
        input: inputs,
        input_type: &profile.input_type,
        dimensions: profile.dimensions,
        encoding_format: "float",
        truncation: profile.truncation,
    })
    .map_err(DispatchFailure::transport)?;
    let call = PreparedCall::prepare(ExtrasProvider::Voyage, &raw, options, deadline)
        .map_err(|error| DispatchFailure::transport(error.diagnostic().clone()))?;
    let response = host.dispatch(ExtrasDispatch::Voyage(call)).await?;
    if !(200..300).contains(&response.status) {
        return Err(DispatchFailure {
            origin: DispatchFailureOrigin::Upstream,
            error: Box::new(server(
                format!("Voyage embeddings failed ({}).", response.status),
                "upstream_error",
            )),
            headers: response.headers.clone(),
            quota_refusal: false,
            upstream_status: Some(response.status),
            retry_after_ms: retry_after(&response.headers, host.now_ms()),
            before_transport: false,
            raw_error_body: Some(response.body),
        });
    }
    let payload: Value = serde_json::from_slice(&response.body).map_err(|_| {
        DispatchFailure::transport(server(
            "Voyage embeddings returned invalid JSON.",
            "upstream_error",
        ))
    })?;
    let rows = payload["data"].as_array().ok_or_else(|| {
        DispatchFailure::transport(server(
            "Voyage embeddings returned invalid JSON.",
            "upstream_error",
        ))
    })?;
    let mut vectors = Vec::new();
    for row in rows {
        let values = row["embedding"].as_array().ok_or_else(|| {
            DispatchFailure::transport(server(
                "Voyage embeddings response missing embedding vector.",
                "upstream_error",
            ))
        })?;
        let vector = values
            .iter()
            .map(|v| {
                v.as_f64().filter(|n| n.is_finite()).ok_or_else(|| {
                    DispatchFailure::transport(server(
                        "Voyage embeddings response contained non-numeric values.",
                        "upstream_error",
                    ))
                })
            })
            .collect::<Result<Vec<_>, _>>()?;
        vectors.push(vector);
    }
    let tokens = payload["usage"]["total_tokens"]
        .as_f64()
        .filter(|v| v.is_finite())
        .map(|v| v.max(0.0).trunc() as u64);
    Ok((vectors, tokens))
}
pub fn result_body(profile: &EmbeddingsProfile, vectors: &[Vec<f64>], tokens: u64) -> Value {
    let data: Vec<Value> = vectors
        .iter()
        .enumerate()
        .map(|(index, vector)| {
            let embedding = if profile.encoding_format == "base64" {
                json!(extras::base64_encode(
                    &vector
                        .iter()
                        .flat_map(|v| (*v as f32).to_le_bytes())
                        .collect::<Vec<_>>()
                ))
            } else {
                json!(vector)
            };
            json!({"object":"embedding","index":index,"embedding":embedding})
        })
        .collect();
    json!({"object":"list","data":data,"model":profile.model,"usage":{"prompt_tokens":tokens,"total_tokens":tokens}})
}
pub fn usage(tokens: u64) -> Value {
    json!({"input_tokens":tokens,"output_tokens":0,"total_tokens":tokens})
}
