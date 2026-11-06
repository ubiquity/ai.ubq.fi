//! Source 959da4a (src/chat/stream-translation.ts): Responses-to-Chat parts keep an item-id
//! alias for positional deltas, and an id-less terminal part that repeats one buffered part or
//! the concatenation of positional parts adds no duplicate text.
use gateway_core::{error::GatewayError, provider::UpstreamProvider, source_json::SourceJson};
use gateway_providers::{
    source_tools::ToolMap,
    sse::StreamDecoder,
    types::{ClientSurface, DecodePlan, ResponseDecode},
};
use serde_json::{Value, json};

type TestResult = Result<(), Box<dyn std::error::Error>>;

fn plan() -> Result<DecodePlan, GatewayError> {
    Ok(DecodePlan {
        provider: UpstreamProvider::Surplus,
        surface: ClientSurface::Chat,
        decode: ResponseDecode::ResponsesToChat,
        model: "fixture-model".into(),
        upstream_model: "fixture-model".into(),
        response_id: "resp_fixture".into(),
        created_at: 1,
        stream: true,
        request: SourceJson::from_serializable(&json!({}))?,
        tool_names: ToolMap::new(),
        reasoning_summary: None,
    })
}
fn delta(kind: &str, item: &str, output: u64, text: &str) -> Value {
    json!({"type":format!("response.{kind}.delta"),"item_id":item,"output_index":output,"content_index":0,"delta":text})
}
fn completed(output: Value) -> Value {
    json!({"type":"response.completed","response":{"id":"resp_fixture","status":"completed","output":output}})
}
/// Concatenated `content` and `refusal` deltas, or the decoder's error.
fn run(events: &[Value]) -> Result<(String, String), GatewayError> {
    let mut decoder = StreamDecoder::new(plan()?);
    let (mut content, mut refusal) = (String::new(), String::new());
    for event in events {
        for chunk in decoder.push(format!("data: {event}\n\n").as_bytes())? {
            let value: Value = chunk.data.deserialize()?;
            let delta = &value["choices"][0]["delta"];
            content.push_str(delta["content"].as_str().unwrap_or_default());
            refusal.push_str(delta["refusal"].as_str().unwrap_or_default());
        }
    }
    Ok((content, refusal))
}

#[test]
fn identified_done_without_output_index_reuses_the_positional_item_alias() -> TestResult {
    let text = run(&[
        delta("output_text", "item-1", 1, "hello"),
        json!({"type":"response.output_text.done","item_id":"item-1","content_index":0,"text":"hello"}),
        json!({"type":"response.output_item.done","item_id":"item-1","item":{"type":"message","content":[{"type":"output_text","text":"hello"}]}}),
    ])?;
    assert_eq!(text.0, "hello");
    let refusal = run(&[
        delta("refusal", "item-1", 1, "no"),
        json!({"type":"response.refusal.done","item_id":"item-1","content_index":0,"refusal":"no"}),
    ])?;
    assert_eq!(refusal, (String::new(), "no".into()));
    Ok(())
}

#[test]
fn id_less_terminal_output_repeating_all_positions_adds_nothing() -> TestResult {
    let parts = [
        delta("output_text", "item-0", 0, "ha"),
        delta("output_text", "item-1", 1, "ha"),
    ];
    let message = json!({"type":"message","content":[{"type":"output_text","text":"haha"}]});
    let mut events = parts.to_vec();
    events.push(json!({"type":"response.output_item.done","item":message}));
    assert_eq!(run(&events)?.0, "haha");
    let mut events = parts.to_vec();
    events.push(completed(json!([message])));
    assert_eq!(run(&events)?.0, "haha");
    let mut events = parts.to_vec();
    events.push(json!({"type":"response.output","output":[message]}));
    events.push(completed(json!([message])));
    assert_eq!(run(&events)?.0, "haha");
    Ok(())
}

#[test]
fn id_less_terminal_repeat_of_one_item_part_or_prefix_adds_nothing() -> TestResult {
    let unpositioned = json!({"type":"response.output_text.delta","item_id":"item-9","content_index":0,"delta":"hello"});
    let message = json!({"type":"message","content":[{"type":"output_text","text":"hell"}]});
    assert_eq!(
        run(&[unpositioned, completed(json!([message]))])?.0,
        "hello"
    );
    Ok(())
}

#[test]
fn identified_or_longer_terminal_content_keeps_the_ordinary_suffix_path() -> TestResult {
    let message =
        json!({"type":"message","id":"item-0","content":[{"type":"output_text","text":"hah"}]});
    let events = [
        delta("output_text", "item-0", 0, "ha"),
        delta("output_text", "item-1", 1, "ha"),
        completed(json!([message])),
    ];
    assert_eq!(run(&events)?.0, "hahah");
    let conflict = json!({"type":"message","content":[{"type":"output_text","text":"other"}]});
    let events = [
        delta("output_text", "item-0", 0, "ha"),
        completed(json!([conflict])),
    ];
    let Err(error) = run(&events) else {
        return Err("a conflicting terminal part must fail closed".into());
    };
    assert_eq!(
        error.envelope.error.message,
        "Completed output disagrees with streamed prefix."
    );
    Ok(())
}
