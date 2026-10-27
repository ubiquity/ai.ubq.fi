use gateway_core::{error::GatewayError, source_json::SourceJson, source_serialization as source};
use serde_json::Value;

pub fn decode(raw: &SourceJson) -> Result<SourceJson, GatewayError> {
    if source::member(raw, "answers")
        .as_ref()
        .and_then(source::entries)
        .is_none()
    {
        return Err(GatewayError::new(
            502,
            "Decisions upstream invalid response",
            Some("openrouter_upstream_invalid_response"),
        ));
    }
    Ok(raw.clone())
}
pub fn usage(raw: &SourceJson) -> Option<Value> {
    let value = source::member(raw, "usage")?;
    let input = source::member(&value, "input_tokens")
        .as_ref()
        .and_then(source::number)?;
    let output = source::member(&value, "output_tokens")
        .as_ref()
        .and_then(source::number)?;
    let mut result = serde_json::json!({"input_tokens":input,"output_tokens":output,"total_tokens":input+output,"input_tokens_details":{"cached_tokens":0}});
    if let Some(cost) = source::member(&value, "cost")
        .as_ref()
        .and_then(source::number)
    {
        result["cost"] = serde_json::json!(cost);
    }
    Some(result)
}
