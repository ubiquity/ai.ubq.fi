use gateway_provider_source::source as s;
use crate::types::DecodePlan;
use gateway_provider_source::extras::malformed;
use gateway_core::error::GatewayError;
use gateway_core::js_string::JsString16;
use gateway_core::source_json::SourceJson;
use gateway_core::source_serialization as json;

pub fn to_chat(payload: &SourceJson, plan: &DecodePlan) -> Result<SourceJson, GatewayError> {
    if !s::present(payload, "status").is_some_and(|observed| s::is(&observed, "completed")) {
        return Err(malformed(
            "Upstream stream ended without response.completed.",
        ));
    }
    let mut texts = vec![];
    let mut refusals = vec![];
    let mut calls = vec![];
    for item in s::present(payload, "output").and_then(|observed| json::array(&observed)).unwrap_or_default() {
        if s::present(&item, "type").is_some_and(|observed| s::is(&observed, "message")) {
            for part in s::present(&item, "content").and_then(|observed| json::array(&observed)).unwrap_or_default() {
                if let Some(value) = s::present(&part, "text").and_then(|observed| s::string(&observed)) {
                    texts.push(value);
                }
                if let Some(value) = s::present(&part, "refusal").and_then(|observed| s::string(&observed)) {
                    refusals.push(value);
                }
            }
        } else if s::present(&item, "type").is_some_and(|observed| s::is(&observed, "function_call")) {
            let id = s::present(&item, "call_id").filter(|value| s::string(value).is_some()).ok_or_else(|| malformed("Upstream function call has no call_id."))?;
            let name = s::present(&item, "name").filter(|value| s::nonempty(value).is_some()).ok_or_else(|| malformed("Upstream function call has no name."))?;
            let arguments = s::present(&item, "arguments").filter(|value| s::string(value).is_some()).ok_or_else(|| malformed("Upstream function call arguments are invalid."))?;
            calls.push(s::object(vec![
                ("id", id),
                ("type", s::text("function")?),
                (
                    "function",
                    s::object(vec![("name", name), ("arguments", arguments)])?,
                ),
            ])?);
        }
    }
    let text = s::join(&texts, "");
    let refusal = s::join(&refusals, "");
    if text.as_units().is_empty() && refusal.as_units().is_empty() && calls.is_empty() {
        return Err(GatewayError::new(
            502,
            "Upstream response completed with no translated semantic output.",
            Some("empty_upstream_completion"),
        )
        .with_type("server_error"));
    }
    let mut result = completion(plan, text, refusal, calls)?;
    if let Some(id) = s::present(payload, "id") {
        s::set(&mut result, "id", id)?;
    }
    if let Some(created) = s::present(payload, "created_at") {
        s::set(&mut result, "created", created)?;
    }
    if let Some(usage) = s::present(payload, "usage").as_ref().and_then(gateway_provider_source::source_collector::usage)
        .as_ref()
        .and_then(gateway_provider_source::usage::responses_to_chat)
    {
        s::set(&mut result, "usage", s::scalar_json(&usage)?)?;
    }
    Ok(result)
}


pub fn completion(
    plan: &DecodePlan,
    text: JsString16,
    refusal: JsString16,
    calls: Vec<SourceJson>,
) -> Result<SourceJson, gateway_core::error::GatewayError> {
    let mut message = s::object(vec![
        ("role", s::text("assistant")?),
        (
            "content",
            if text.as_units().is_empty() && (!calls.is_empty() || !refusal.as_units().is_empty()) {
                s::null()?
            } else {
                s::quoted(&text)?
            },
        ),
    ])?;
    if !refusal.as_units().is_empty() {
        s::set(&mut message, "refusal", s::quoted(&refusal)?)?;
    }
    let finish = if calls.is_empty() {
        "stop"
    } else {
        "tool_calls"
    };
    if !calls.is_empty() {
        s::set(&mut message, "tool_calls", s::array(&calls)?)?;
    }
    s::object(vec![
        ("id", s::text(&plan.response_id)?),
        ("object", s::text("chat.completion")?),
        ("created", s::scalar_json(&serde_json::json!(plan.created_at))?),
        ("model", s::quoted(plan.model.as_js_string())?),
        (
            "choices",
            s::array(&[s::object(vec![
                ("index", s::scalar_json(&serde_json::json!(0))?),
                ("message", message),
                ("finish_reason", s::text(finish)?),
            ])?])?,
        ),
    ])
}
