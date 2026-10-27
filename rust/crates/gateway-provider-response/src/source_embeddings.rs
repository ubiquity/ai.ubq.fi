use gateway_provider_source::source as s;
use gateway_provider_source::extras::base64_encode;
use gateway_provider_protocol::types::DecodePlan;
use gateway_provider_source::extras::malformed;
use gateway_core::error::GatewayError;
use gateway_core::source_json::SourceJson;
use serde_json::Value;
use serde_json::json;

pub fn decode_embeddings(value: &Value, plan: &DecodePlan) -> Result<SourceJson, GatewayError> {
    // Preparation validated this named profile; decode needs only these finite controls.
    let count = s::present(&plan.request, "input").and_then(|value| gateway_core::source_serialization::array(&value)).map_or(1, |rows| rows.len());
    let dimensions = s::present(&plan.request, "dimensions").and_then(|value| gateway_core::source_serialization::number(&value)).unwrap_or(1024.0) as u64;
    let encoding_format =
        s::present(&plan.request, "encoding_format").and_then(|observed| s::control(&observed))
            .unwrap_or_else(|| "float".into());
    let rows = value["data"]
        .as_array()
        .ok_or_else(|| malformed("Voyage embeddings returned invalid JSON."))?;
    if rows.len() != count {
        return Err(malformed(
            "Voyage embeddings returned a different vector count.",
        ));
    }
    let mut data = vec![];
    for (index, row) in rows.iter().enumerate() {
        let vector = row["embedding"]
            .as_array()
            .filter(|v| v.len() == dimensions as usize && v.iter().all(Value::is_number))
            .ok_or_else(|| malformed("Voyage embedding dimensions or values are invalid."))?;
        let embedding = if encoding_format == "base64" {
            let mut bytes = vec![];
            for value in vector {
                bytes.extend_from_slice(&(value.as_f64().unwrap_or(0.0) as f32).to_le_bytes());
            }
            json!(base64_encode(&bytes))
        } else {
            json!(vector)
        };
        data.push(json!({"object":"embedding","index":index,"embedding":embedding}));
    }
    let total = value["usage"]["total_tokens"]
        .as_f64()
        .filter(|v| v.is_finite())
        .map(|v| v.max(0.0).trunc());
    let mut body = s::scalar_json(&json!({"object":"list","data":data,"usage":{"prompt_tokens":total,"total_tokens":total}}))?;
    s::set(
        &mut body,
        "model",
        s::quoted(plan.model.as_js_string())?,
    )?;
    Ok(body)
}
