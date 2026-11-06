//! Chat Completions usage and provider correlation reach the same settlement records as Responses.
//! Mirrors src/deepseek/handlers.ts (buffered `extractChatUsageTokens`, `providerRequestId ??=`),
//! src/provider/stream-relay.ts (chunk usage) and src/provider/openrouter-handlers.ts (stream usage frame).
use gateway_core::{
    provider::UpstreamProvider,
    source_json::SourceJson,
    source_usage::{UsageStatus, extract_usage_tokens},
};
use gateway_providers::{
    response::{completion_provider_request_id, decode_response},
    source_request::prepare_source_request,
    source_tools::ToolMap,
    sse::StreamDecoder,
    types::{ChatProfile, ClientSurface, DecodePlan, Headers, PrepareOptions, ResponseDecode},
};
use serde_json::{Value, json};

type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;

fn plan(
    provider: UpstreamProvider,
    decode: ResponseDecode,
    stream: bool,
) -> TestResult<DecodePlan> {
    Ok(DecodePlan {
        provider,
        surface: ClientSurface::Chat,
        decode,
        model: "deepseek-v4-flash".into(),
        upstream_model: "deepseek-v4-flash".into(),
        response_id: "chatcmpl_fixture".into(),
        created_at: 1_780_000_000,
        stream,
        request: SourceJson::from_serializable(&json!({"stream":stream}))?,
        tool_names: ToolMap::new(),
        reasoning_summary: None,
    })
}
fn deepseek_usage() -> Value {
    json!({"prompt_tokens":11,"completion_tokens":7,"total_tokens":18,"prompt_tokens_details":{"cached_tokens":0},"prompt_cache_hit_tokens":0,"prompt_cache_miss_tokens":11})
}
fn completion(id: &str) -> Value {
    json!({"id":id,"object":"chat.completion","created":1_780_000_000,"model":"deepseek-v4-flash","choices":[{"index":0,"message":{"role":"assistant","content":"Deployment is green."},"finish_reason":"stop"}],"usage":deepseek_usage()})
}
fn chunk(delta: Value, finish: Value, usage: Value) -> Value {
    json!({"id":"chunk-1","object":"chat.completion.chunk","created":1_780_000_000,"model":"deepseek-v4-flash","choices":[{"index":0,"delta":delta,"finish_reason":finish}],"usage":usage})
}
fn assert_measured(usage: Option<&Value>) -> TestResult {
    let usage =
        extract_usage_tokens(usage).ok_or("Chat usage must reach the settlement extractor")?;
    assert_eq!(
        (usage.input_tokens, usage.output_tokens),
        (Some(11), Some(7))
    );
    assert_eq!(
        (usage.total_tokens, usage.cached_input_tokens),
        (Some(18), Some(0))
    );
    assert_eq!(usage.status, UsageStatus::Reported);
    Ok(())
}

#[test]
fn buffered_chat_completion_settles_reported_usage_and_keeps_the_client_body() -> TestResult {
    let plan = plan(UpstreamProvider::Deepseek, ResponseDecode::Chat, false)?;
    let bytes = serde_json::to_vec(&completion("deepseek-completion-1"))?;
    let decoded = decode_response(&plan, 200, &Headers::new(), &bytes)?;
    assert_measured(decoded.usage.as_ref())?;
    let body: Value = decoded.body.deserialize()?;
    assert_eq!(body["usage"]["prompt_tokens"], 11);
    assert_eq!(body["usage"]["completion_tokens"], 7);
    Ok(())
}

#[test]
fn streamed_chat_usage_on_the_final_chunk_settles_and_null_usage_never_clears_it() -> TestResult {
    let mut decoder = StreamDecoder::new(plan(
        UpstreamProvider::Deepseek,
        ResponseDecode::Chat,
        true,
    )?);
    let frames = [
        chunk(
            json!({"role":"assistant","content":"Deploy"}),
            Value::Null,
            Value::Null,
        ),
        // DeepSeek reports usage on the final content chunk rather than a separate frame.
        chunk(
            json!({"content":"ment is green."}),
            json!("stop"),
            deepseek_usage(),
        ),
        chunk(json!({}), Value::Null, Value::Null),
    ];
    let mut bytes = frames
        .iter()
        .map(|frame| format!("data: {frame}\n\n"))
        .collect::<String>();
    bytes.push_str("data: [DONE]\n\n");
    let mut events = decoder.push(bytes.as_bytes())?;
    events.extend(decoder.finish()?);
    // The Worker relay keeps the latest present usage, like `usage = extract(frame.usage) ?? usage`.
    let mut settled = decoder.observed_usage().cloned();
    for event in &events {
        settled = event.observed_usage().or(settled);
    }
    assert_measured(settled.as_ref())
}

#[test]
fn responses_surface_usage_is_not_reprojected() -> TestResult {
    let mut plan = plan(
        UpstreamProvider::Deepseek,
        ResponseDecode::ChatToResponses(ChatProfile::Deepseek),
        false,
    )?;
    plan.surface = ClientSurface::Responses;
    let bytes = serde_json::to_vec(&completion("deepseek-completion-2"))?;
    let decoded = decode_response(&plan, 200, &Headers::new(), &bytes)?;
    assert_measured(decoded.usage.as_ref())
}

#[test]
fn header_less_buffered_completion_ids_correlate_only_direct_chat_providers() -> TestResult {
    let bytes = serde_json::to_vec(&completion("deepseek-completion-1"))?;
    for (provider, decode) in [
        (UpstreamProvider::Deepseek, ResponseDecode::Chat),
        (
            UpstreamProvider::Deepseek,
            ResponseDecode::ChatToResponses(ChatProfile::Deepseek),
        ),
        (UpstreamProvider::Cerebras, ResponseDecode::Chat),
        (
            UpstreamProvider::Cerebras,
            ResponseDecode::ChatToResponses(ChatProfile::Cerebras),
        ),
    ] {
        let id = completion_provider_request_id(&plan(provider, decode, false)?, &bytes);
        assert_eq!(id.as_deref(), Some("deepseek-completion-1"), "{provider:?}");
    }
    // Lithos and OpenRouter read response headers only; native Responses keep their own ids.
    for (provider, decode) in [
        (UpstreamProvider::Lithos, ResponseDecode::Chat),
        (UpstreamProvider::Openrouter, ResponseDecode::Chat),
        (UpstreamProvider::Deepseek, ResponseDecode::Responses),
    ] {
        assert_eq!(
            completion_provider_request_id(&plan(provider, decode, false)?, &bytes),
            None
        );
    }
    let deepseek = plan(UpstreamProvider::Deepseek, ResponseDecode::Chat, false)?;
    for id in [
        json!(""),
        json!(" padded"),
        json!("x".repeat(257)),
        json!("tab\tid"),
        json!(7),
    ] {
        let bytes = serde_json::to_vec(&json!({"id":id}))?;
        assert_eq!(
            completion_provider_request_id(&deepseek, &bytes),
            None,
            "{id}"
        );
    }
    assert_eq!(completion_provider_request_id(&deepseek, b"not json"), None);
    Ok(())
}

fn openrouter(surface: ClientSurface, raw: &Value) -> TestResult<Value> {
    let mut options = PrepareOptions {
        response_id: "chatcmpl_fixture".into(),
        ..Default::default()
    };
    options.auth.secret = "fixture-only".into();
    options.allowed_models = vec!["meta/test".into()];
    let raw = SourceJson::from_serializable(raw)?;
    let prepared = prepare_source_request(
        UpstreamProvider::Openrouter,
        surface,
        &raw,
        &"meta/test".into(),
        &options,
    )
    .map_err(|error| format!("{error:?}"))?;
    Ok(prepared.body.deserialize()?)
}

#[test]
fn openrouter_chat_requests_the_upstream_usage_frame_only_for_streams() -> TestResult {
    let messages = json!([{"role":"user","content":"hi"}]);
    let streamed = openrouter(
        ClientSurface::Chat,
        &json!({"model":"meta/test","messages":messages,"stream":true}),
    )?;
    assert_eq!(streamed["stream_options"], json!({"include_usage":true}));
    let own = json!({"include_usage":false});
    let kept = openrouter(
        ClientSurface::Chat,
        &json!({"model":"meta/test","messages":messages,"stream":true,"stream_options":own}),
    )?;
    assert_eq!(kept["stream_options"], own);
    let buffered = openrouter(
        ClientSurface::Chat,
        &json!({"model":"meta/test","messages":messages,"stream_options":own}),
    )?;
    assert!(buffered.get("stream_options").is_none());
    let responses = openrouter(
        ClientSurface::Responses,
        &json!({"model":"meta/test","input":"hi","stream":true}),
    )?;
    assert!(responses.get("stream_options").is_none());
    Ok(())
}
