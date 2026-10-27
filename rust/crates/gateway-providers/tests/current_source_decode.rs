//! b592 source deltas at the actual decoder/collector boundary.
use gateway_core::{lifecycle::SemanticTerminal, provider::UpstreamProvider, source_json::SourceJson};
use gateway_provider_events::event::ProviderEvent;
use gateway_provider_protocol::types::{ClientSurface, DecodePlan, ResponseDecode};
use gateway_provider_stream::sse::StreamDecoder;
use serde_json::{Value, json};
type TestResult = Result<(), Box<dyn std::error::Error>>;
fn plan(provider: UpstreamProvider, chat: bool) -> Result<DecodePlan, gateway_core::error::GatewayError> {
    Ok(DecodePlan { provider, surface: if chat { ClientSurface::Chat } else { ClientSurface::Responses }, decode: if chat { ResponseDecode::ResponsesToChat } else { ResponseDecode::Responses }, model: "gpt-test".into(), upstream_model: "gpt-test".into(), response_id: "resp_fallback".into(), created_at: 7, stream: true, request: SourceJson::parse("{}")?, tool_names: Default::default(), reasoning_summary: None })
}
fn decode(plan: DecodePlan, rows: &[Value]) -> Result<Vec<ProviderEvent>, gateway_core::error::GatewayError> {
    let mut decoder = StreamDecoder::new(plan);
    let bytes = rows.iter().map(|row| format!("data: {row}\n\n")).collect::<String>();
    let mut events = Vec::new();
    for chunk in bytes.as_bytes().chunks(11) { events.extend(decoder.push(chunk)?); }
    events.extend(decoder.finish()?);
    Ok(events)
}
fn text(events: &[ProviderEvent]) -> Result<String, gateway_core::error::GatewayError> {
    let mut text = String::new();
    for event in events {
        let value: Value = event.data.deserialize()?;
        if let Some(delta) = value["choices"][0]["delta"]["content"].as_str() { text.push_str(delta); }
    }
    Ok(text)
}
#[test]
fn item_alias_done_without_output_index_does_not_duplicate_text_or_refusal() -> TestResult {
    for refusal in [false, true] {
        let delta = if refusal { "response.refusal.delta" } else { "response.output_text.delta" };
        let done = if refusal { "response.refusal.done" } else { "response.output_text.done" };
        let part = if refusal { json!({"type":"refusal","refusal":"hello"}) } else { json!({"type":"output_text","text":"hello"}) };
        let mut terminal = json!({"type":done,"item_id":"message-one","content_index":0});
        terminal[if refusal { "refusal" } else { "text" }] = json!("hello");
        let events = decode(plan(UpstreamProvider::Surplus, true)?, &[
            json!({"type":"response.created","response":{"id":"resp_actual"}}),
            json!({"type":delta,"output_index":0,"content_index":0,"item_id":"message-one","delta":"hello"}), terminal,
            json!({"type":"response.completed","response":{"id":"resp_actual","output":[{"id":"message-one","type":"message","content":[part]}]}}),
        ])?;
        let mut observed = String::new();
        for event in events { let body: Value = event.data.deserialize()?; if let Some(value) = body["choices"][0]["delta"][if refusal { "refusal" } else { "content" }].as_str() { observed.push_str(value); } }
        assert_eq!(observed, "hello");
    }
    Ok(())
}
#[test]
fn identity_less_aggregate_terminal_uses_insertion_order_and_ignores_alias_duplicates() -> TestResult {
    let events = decode(plan(UpstreamProvider::Surplus, true)?, &[
        json!({"type":"response.created","response":{"id":"resp_actual"}}),
        json!({"type":"response.output_text.delta","output_index":10,"item_id":"message-ten","delta":"first"}),
        json!({"type":"response.output_text.delta","output_index":2,"item_id":"message-two","delta":"second"}),
        json!({"type":"response.completed","response":{"id":"resp_actual","output":[{"type":"message","content":[{"type":"output_text","text":"firstsecond"}]}]}}),
    ])?;
    assert_eq!(text(&events)?, "firstsecond");
    assert_eq!(events.iter().filter(|event| event.is_done()).count(), 1);
    Ok(())
}
#[test]
fn conflicting_terminal_still_errors_and_eof_is_not_a_terminal() -> TestResult {
    let rows = [json!({"type":"response.output_text.delta","output_index":0,"delta":"first"}), json!({"type":"response.output_text.done","output_index":0,"text":"conflict"})];
    assert!(decode(plan(UpstreamProvider::Surplus, true)?, &rows).is_err());
    assert!(decode(plan(UpstreamProvider::Surplus, true)?, &rows[..1]).is_err());
    Ok(())
}
#[test]
fn codex_exact_context_terminal_preserves_template_output_sequence_and_original_observation() -> TestResult {
    let original = json!({"type":"error","sequence_number":4,"error":{"code":"context_length_exceeded","message":"context exhausted","param":"input","detail":"retained"}});
    let events = decode(plan(UpstreamProvider::ChatgptCodex, false)?, &[
        json!({"type":"response.created","response":{"id":"resp_actual","object":"response","model":"gpt-test","metadata":{"kept":true}}}),
        json!({"type":"response.output_item.added","item":{"id":"message-one","type":"message","content":[]}}),
        json!({"type":"response.output_text.delta","item_id":"message-one","delta":"visible text"}), original.clone(),
    ])?;
    let last = events.last().ok_or("terminal")?;
    let terminal: Value = last.data.deserialize()?;
    assert_eq!(terminal["type"], "response.failed"); assert_eq!(terminal["sequence_number"], 4);
    assert_eq!(terminal["response"]["id"], "resp_actual"); assert_eq!(terminal["response"]["metadata"]["kept"], true);
    assert_eq!(terminal["response"]["error"], original["error"]);
    assert_eq!(terminal["response"]["output"][0]["content"][0]["text"], "visible text");
    assert_eq!(last.observation().deserialize::<Value>()?, original);
    assert_eq!(last.terminal(), Some(SemanticTerminal::Error)); assert!(last.observed_usage().is_none()); assert!(!last.semantic_event());
    assert!(last.encode_sse().starts_with("event: response.failed\n"));
    assert_eq!(events.iter().filter(|event| event.terminal().is_some()).count(), 1);
    let serialized = serde_json::to_value(last)?; assert!(serialized.get("original_observation").is_none());
    Ok(())
}
#[test]
fn codex_normalization_requires_exact_code_provider_and_observed_identity() -> TestResult {
    for (provider, code, created, expected) in [
        (UpstreamProvider::ChatgptCodex, "context_length_exceeded", false, "error"),
        (UpstreamProvider::ChatgptCodex, "CONTEXT_LENGTH_EXCEEDED", true, "error"),
        (UpstreamProvider::Surplus, "context_length_exceeded", true, "error"),
        (UpstreamProvider::ChatgptCodex, "context_length_exceeded", true, "response.failed"),
    ] {
        let mut rows = Vec::new();
        if created { rows.push(json!({"type":"response.created","response":{"id":"resp_observed"}})); }
        rows.push(json!({"type":"error","code":code,"message":"error"}));
        let events = decode(plan(provider, false)?, &rows)?;
        assert_eq!(events.last().ok_or("terminal")?.type_name().as_deref(), Some(expected));
    }
    Ok(())
}
#[test]
fn native_responses_decoder_preserves_lone_utf16_error_fields() -> TestResult {
    let mut decoder = StreamDecoder::new(plan(UpstreamProvider::ChatgptCodex, false)?);
    decoder.push(b"data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_observed\"}}\n\n")?;
    let events = decoder.push(br#"data: {"type":"error","code":"context_length_exceeded","message":"\ud800","param":"input"}

"#)?;
    decoder.finish()?;
    let event = events.last().ok_or("terminal")?;
    let error = gateway_core::source_serialization::member(&event.data, "response").and_then(|value| gateway_core::source_serialization::member(&value, "error")).ok_or("error")?;
    let message = gateway_core::source_serialization::member(&error, "message").and_then(|value| gateway_core::source_serialization::string(&value)).ok_or("message")?;
    assert_eq!(message.as_units(), &[0xd800]);
    Ok(())
}
