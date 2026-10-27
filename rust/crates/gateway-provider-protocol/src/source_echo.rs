use gateway_provider_source::source as s;
use crate::types::DecodePlan;
use gateway_core::source_json::SourceJson;
use gateway_core::source_serialization as json;

pub fn envelope(plan: &DecodePlan, status: &str) -> Result<SourceJson, gateway_core::error::GatewayError> {
    Ok({
    let instructions = match s::present(&plan.request, "instructions") { Some(value) => value, None => s::null()? };
    let choice = match s::present(&plan.request, "tool_choice") { Some(value) => value, None => s::text("auto")? };
    let tools = match s::present(&plan.request, "tools").filter(|value| json::array(value).is_some()) { Some(value) => value, None => s::array(&[])? };
    let parallel = s::present(&plan.request, "parallel_tool_calls").and_then(|observed| s::boolean(&observed)).unwrap_or(true);
    s::object(vec![
        ("id", s::text(&plan.response_id)?),
        ("object", s::text("response")?),
        ("created_at", s::scalar_json(&serde_json::json!(plan.created_at))?),
        ("status", s::text(status)?),
        ("model", s::quoted(plan.model.as_js_string())?),
        ("output", s::array(&[])?),
        ("error", s::null()?),
        ("incomplete_details", s::null()?),
        ("instructions", instructions),
        ("metadata", s::object(vec![])?),
        ("parallel_tool_calls", s::scalar_json(&serde_json::json!(parallel))?),
        ("tool_choice", choice),
        ("tools", tools),
        ("temperature", s::null()?),
        ("top_p", s::null()?),
        ("max_output_tokens", s::null()?),
        ("previous_response_id", s::null()?),
        ("reasoning", s::null()?),
        ("store", s::scalar_json(&serde_json::json!(false))?),
        ("truncation", s::text("disabled")?),
        ("usage", s::null()?),
    ])?

    })
}



pub fn response_event(kind: &str, response: SourceJson) -> Result<crate::types::ProviderEvent, gateway_core::error::GatewayError> {
    Ok({
    crate::types::ProviderEvent::responses_source(s::object(vec![
        ("type", s::text(kind)?),
        ("response", response),
    ])?)

    })
}



pub fn item_event(
    kind: &str,
    index: usize,
    item: SourceJson,
) -> Result<crate::types::ProviderEvent, gateway_core::error::GatewayError> {
    Ok({
    crate::types::ProviderEvent::responses_source(s::object(vec![
        ("type", s::text(kind)?),
        ("output_index", s::scalar_json(&serde_json::json!(index))?),
        ("item", item),
    ])?)

    })
}



#[cfg(test)]
mod tests {
    use super::*;
    use gateway_provider_tool_grammar::source_tools::ToolMap;
    use crate::types::{ChatProfile, ClientSurface, ResponseDecode};
    use gateway_core::{js_string::JsString16, provider::UpstreamProvider};

    #[test]
    fn instructions_tools_and_choice_never_cross_a_scalar_envelope()
    -> Result<(), gateway_core::error::GatewayError> {
        let request = SourceJson::parse(
            r#"{"instructions":"\ud800","tools":[{"type":"function","name":"f\udc00","parameters":{"\ud800":{"type":"string"}}}],"tool_choice":{"type":"function","name":"\udc00"}}"#,
        )?;
        let plan = DecodePlan {
            provider: UpstreamProvider::Lithos,
            surface: ClientSurface::Responses,
            decode: ResponseDecode::ChatToResponses(ChatProfile::Lithos),
            model: "test".into(),
            upstream_model: "test".into(),
            response_id: "resp_test".into(),
            created_at: 1,
            stream: true,
            request: request.clone(),
            tool_names: ToolMap::new(),
            reasoning_summary: None,
        };
        let body = envelope(&plan, "completed")?;
        assert_eq!(
            s::present(&body, "instructions").and_then(|observed| json::string(&observed)),
            Some(JsString16::from_units(vec![0xd800]))
        );
        assert_eq!(s::present(&body, "tools"), s::present(&request, "tools"));
        assert_eq!(
            s::present(&body, "tool_choice"),
            s::present(&request, "tool_choice")
        );
        assert!(body.deserialize::<serde_json::Value>().is_err());
        let event = response_event("response.completed", body.clone())?;
        assert_eq!(event.field("response"), Some(body));
        assert!(event.encode_sse().contains("\\ud800"));
        assert_eq!(plan.request, request);
        Ok(())
    }
}
