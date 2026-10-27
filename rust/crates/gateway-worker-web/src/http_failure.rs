//! Selected HTTP failure DATA and Responses encoding; no capture or transport authority.
use gateway_core::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use gateway_worker_platform::random;
use gateway_worker_storage::store::now_ms;

fn scalar(value: &serde_json::Value) -> Result<SourceJson, GatewayError> {
    SourceJson::from_serializable(value)
}
fn object(fields: Vec<(&str, SourceJson)>) -> Result<SourceJson, GatewayError> {
    source::object_keys(
        fields
            .into_iter()
            .map(|(name, value)| (JsString16::from_utf8(name), value))
            .collect(),
    )
}
fn string_field(value: &SourceJson, name: &str) -> Option<JsString16> {
    source::member(value, name)
        .as_ref()
        .and_then(source::string)
}
pub fn error_detail(bytes: &[u8], complete: bool, status: u16) -> Result<SourceJson, GatewayError> {
    let decoded = String::from_utf8_lossy(bytes);
    let text = if complete {
        decoded
            .strip_prefix('\u{feff}')
            .unwrap_or(&decoded)
            .to_owned()
    } else {
        String::new()
    };
    if let Ok(parsed) = SourceJson::parse(&text)
        && let Some(error) = source::member(&parsed, "error")
        && error.object_entries().is_ok()
    {
        return Ok(error);
    }
    scalar(
        &serde_json::json!({"message":if text.is_empty(){format!("Upstream request failed with HTTP {status}.")}else{text}}),
    )
}
pub fn frames(error: &SourceJson, status: u16) -> Result<Vec<u8>, GatewayError> {
    let code = string_field(error, "code").unwrap_or_else(|| "server_error".into());
    let message = string_field(error, "message").unwrap_or_else(|| {
        JsString16::from_utf8(&format!("Upstream request failed with HTTP {status}."))
    });
    let id = format!("resp_{}", random::random_uuid()?.replace('-', ""));
    let base = scalar(
        &serde_json::json!({"id":id,"object":"response","created_at":now_ms()? / 1000,"status":"in_progress","error":null,"output":[],"usage":null}),
    )?;
    let mut failed = base.object_entries()?;
    for (name, value) in &mut failed {
        if name.as_units() == JsString16::from_utf8("status").as_units() {
            *value = scalar(&serde_json::json!("failed"))?;
        }
        if name.as_units() == JsString16::from_utf8("error").as_units() {
            *value = object(vec![
                ("code", SourceJson::from_serializable(&code)?),
                ("message", SourceJson::from_serializable(&message)?),
            ])?;
        }
    }
    let created = object(vec![
        ("type", scalar(&serde_json::json!("response.created"))?),
        ("sequence_number", scalar(&serde_json::json!(0))?),
        ("response", base),
    ])?;
    let failed = object(vec![
        ("type", scalar(&serde_json::json!("response.failed"))?),
        ("sequence_number", scalar(&serde_json::json!(1))?),
        ("response", source::object_keys(failed)?),
    ])?;
    Ok(format!(
        "event: response.created\ndata: {}\n\nevent: response.failed\ndata: {}\n\n",
        created.as_str(),
        failed.as_str()
    )
    .into_bytes())
}
