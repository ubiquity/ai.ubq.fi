//! Moved from the in-crate `source_echo` test; exercised through the public writer seam.
use gateway_core::{
    js_string::JsString16, provider::UpstreamProvider, source_json::SourceJson,
    source_serialization as json,
};
use gateway_providers::{
    source_tools::ToolMap,
    types::{ChatProfile, ClientSurface, DecodePlan, ResponseDecode},
    writer::{ResponseWriter, envelope},
};

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
    let body = envelope(&plan, "completed");
    assert_eq!(
        json::member(&body, "instructions")
            .as_ref()
            .and_then(json::string),
        Some(JsString16::from_units(vec![0xd800]))
    );
    assert_eq!(
        json::member(&body, "tools"),
        json::member(&request, "tools")
    );
    assert_eq!(
        json::member(&body, "tool_choice"),
        json::member(&request, "tool_choice")
    );
    assert!(body.deserialize::<serde_json::Value>().is_err());
    let created = ResponseWriter::new(plan.clone()).start().remove(0);
    assert_eq!(
        created.field("response"),
        Some(envelope(&plan, "in_progress"))
    );
    assert!(created.encode_sse().contains("\\ud800"));
    assert_eq!(plan.request, request);
    Ok(())
}
