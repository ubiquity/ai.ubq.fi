//! Frozen writer/SSE lifecycle cases retained from the original conformance target.
use gateway_core::provider::UpstreamProvider;
use gateway_provider_protocol::types::{ClientSurface, DecodePlan, ResponseDecode};
use gateway_provider_source::types::ChatProfile;
use gateway_provider_stream::sse::StreamDecoder;
use gateway_provider_tool_grammar::source_tools::ToolMap;
use serde_json::{Value, json};
fn plan(profile: ChatProfile) -> Result<DecodePlan, String> {
    Ok(DecodePlan {
        provider: match profile {
            ChatProfile::Deepseek => UpstreamProvider::Deepseek,
            ChatProfile::Lithos => UpstreamProvider::Lithos,
            ChatProfile::Cerebras => UpstreamProvider::Cerebras,
            _ => UpstreamProvider::Surplus,
        },
        surface: ClientSurface::Responses,
        decode: ResponseDecode::ChatToResponses(profile),
        model: "deepseek-flash".into(),
        upstream_model: "deepseek-flash".into(),
        response_id: "resp_parity".into(),
        created_at: 1_780_000_000,
        stream: true,
        request: gateway_core::source_json::SourceJson::parse("{}")
            .map_err(|error| format!("{error:?}"))?,
        tool_names: ToolMap::new(),
        reasoning_summary: None,
    })
}
fn chunk(delta: Value, finish: Value) -> Value {
    json!({"id":"chatcmpl-stream","object":"chat.completion.chunk","created":1_780_000_001,"model":"deepseek-flash","choices":[{"index":0,"delta":delta,"finish_reason":finish}]})
}
fn frame(value: &Value) -> Vec<u8> {
    format!("data: {value}\n\n").into_bytes()
}
#[test]
fn reasoning_closes_before_text_and_fragmented_tools_keep_announced_indexes() -> Result<(), String>
{
    let mut writer =
        gateway_provider_writer::writer::ResponseWriter::new(plan(ChatProfile::Deepseek)?);
    let mut events = writer
        .chat_chunk(
            &chunk(json!({"reasoning_content":"think"}), Value::Null),
            ChatProfile::Deepseek,
        )
        .map_err(|error| format!("{error:?}"))?;
    events.extend(
        writer
            .chat_chunk(
                &chunk(json!({"content":"answer"}), Value::Null),
                ChatProfile::Deepseek,
            )
            .map_err(|error| format!("{error:?}"))?,
    );
    events.extend(writer.finish().map_err(|error| format!("{error:?}"))?);
    let kinds = events
        .iter()
        .map(|e| e.type_name().ok_or("missing expected fixture value"))
        .collect::<Result<Vec<_>, _>>()?;
    let done = kinds
        .iter()
        .position(|k| *k == "response.output_item.done")
        .ok_or("missing expected fixture value")?;
    let mut message = None;
    for (index, event) in events.iter().enumerate() {
        let data = scalar(&event.data)?;
        if data["type"] == "response.output_item.added" && data["item"]["type"] == "message" {
            message = Some(index);
            break;
        }
    }
    let message = message.ok_or("missing expected fixture value")?;
    assert!(done < message);
    assert_eq!(
        scalar(&events.last().ok_or("missing expected fixture value")?.data)?["response"]["output"]
            [0]["type"],
        "reasoning"
    );
    assert_eq!(
        scalar(&events.last().ok_or("missing expected fixture value")?.data)?["response"]["output"]
            [1]["type"],
        "message"
    );
    let mut writer =
        gateway_provider_writer::writer::ResponseWriter::new(plan(ChatProfile::Deepseek)?);
    writer
        .tool_delta(1, Some("call_b"), Some("second"), "{\"b\":")
        .map_err(|error| format!("{error:?}"))?;
    writer
        .tool_delta(0, Some("call_a"), Some("first"), "{\"a\":")
        .map_err(|error| format!("{error:?}"))?;
    writer
        .tool_delta(1, None, None, "2}")
        .map_err(|error| format!("{error:?}"))?;
    writer
        .tool_delta(0, None, None, "1}")
        .map_err(|error| format!("{error:?}"))?;
    let events = writer.finish().map_err(|error| format!("{error:?}"))?;
    let output = &scalar(&events.last().ok_or("missing expected fixture value")?.data)?["response"]
        ["output"];
    assert_eq!(output[0]["name"], "second");
    assert_eq!(output[0]["arguments"], "{\"b\":2}");
    assert_eq!(output[1]["name"], "first");
    assert!(
        writer
            .finish()
            .map_err(|error| format!("{error:?}"))?
            .is_empty()
    );

    Ok(())
}
#[test]
fn truthful_terminals_distinguish_explicit_truncation_and_empty_stop() -> Result<(), String> {
    let mut writer =
        gateway_provider_writer::writer::ResponseWriter::new(plan(ChatProfile::Deepseek)?);
    writer
        .reasoning("only reasoning")
        .map_err(|error| format!("{error:?}"))?;
    let events = writer.finish().map_err(|error| format!("{error:?}"))?;
    assert_eq!(
        scalar(&events.last().ok_or("missing expected fixture value")?.data)?["type"],
        "response.failed"
    );
    assert_eq!(
        scalar(&events.last().ok_or("missing expected fixture value")?.data)?["response"]["error"]
            ["code"],
        "empty_upstream_completion"
    );
    let mut writer =
        gateway_provider_writer::writer::ResponseWriter::new(plan(ChatProfile::Deepseek)?);
    writer
        .reasoning("only reasoning")
        .map_err(|error| format!("{error:?}"))?;
    writer.finish_reason = Some("length".into());
    let events = writer.finish().map_err(|error| format!("{error:?}"))?;
    assert_eq!(
        scalar(&events.last().ok_or("missing expected fixture value")?.data)?["type"],
        "response.incomplete"
    );
    assert_eq!(
        scalar(&events.last().ok_or("missing expected fixture value")?.data)?["response"]["incomplete_details"]
            ["reason"],
        "max_output_tokens"
    );

    Ok(())
}
#[test]
fn sse_incremental_utf8_mixed_line_endings_and_eof_fail_closed() -> Result<(), String> {
    let mut parser = gateway_provider_events::framing::SseParser::new(1000);
    let raw = "event: thing\r\ndata: {\"text\":\"😀\"}\r\n\r\n".as_bytes();
    let mut frames = vec![];
    for byte in raw {
        frames.extend(
            parser
                .push(&[*byte])
                .map_err(|error| format!("{error:?}"))?,
        );
    }
    assert_eq!(frames.len(), 1);
    assert_eq!(
        serde_json::from_str::<Value>(&frames[0].data).map_err(|error| format!("{error:?}"))?["text"],
        "😀"
    );
    let mut decoder = StreamDecoder::new(plan(ChatProfile::Deepseek)?);
    decoder
        .push(&frame(&chunk(
            json!({"reasoning_content":"thought"}),
            Value::Null,
        )))
        .map_err(|error| format!("{error:?}"))?;
    assert!(decoder.finish().is_err());
    let mut decoder = StreamDecoder::new(plan(ChatProfile::Deepseek)?);
    let mut events = decoder
        .push(&frame(&chunk(json!({"content":"answer"}), json!("stop"))))
        .map_err(|error| format!("{error:?}"))?;
    events.extend(
        decoder
            .push(b"data: [DONE]\n\n")
            .map_err(|error| format!("{error:?}"))?,
    );
    assert_eq!(
        scalar(&events.last().ok_or("missing expected fixture value")?.data)?["type"],
        "response.completed"
    );
    decoder.finish().map_err(|error| format!("{error:?}"))?;
    Ok(())
}
fn scalar(value: &gateway_core::source_json::SourceJson) -> Result<Value, String> {
    value.deserialize().map_err(|error| format!("{error:?}"))
}
