//! OpenRouter Chat stream tail and provider request ids, mirroring
//! tests/openrouter-serving.test.ts ("openrouter chat relays validated stream frames and records
//! usage"), src/provider/stream-relay.ts `finishStream` (usage is recorded and `[DONE]` is always
//! the last frame), src/provider/openrouter-streams.ts (EOF before `[DONE]` is a premature EOF)
//! and tests/transport-normalization-coverage.test.ts (`providerRequestIdFromResponse`).
use gateway_core::{
    lifecycle::SemanticTerminal, provider::UpstreamProvider, source_json::SourceJson,
    source_usage::extract_usage_tokens,
};
use gateway_providers::{
    source_tools::ToolMap,
    sse::StreamDecoder,
    types::{ClientSurface, DecodePlan, Headers, ResponseDecode},
};
use gateway_worker::inference_wire::{chat_wire_terminal, provider_request_id};
use serde_json::{Value, json};

type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;

/// The Worker relay's per-event Chat handling: usage, terminal and client-visible bytes.
struct Tail {
    decoder: StreamDecoder,
    pending: Option<SemanticTerminal>,
    terminal: Option<SemanticTerminal>,
    usage: Option<Value>,
    sse: String,
}

impl Tail {
    fn new() -> TestResult<Self> {
        let plan = DecodePlan {
            provider: UpstreamProvider::Openrouter,
            surface: ClientSurface::Chat,
            decode: ResponseDecode::Chat,
            model: "meta-llama/llama-3.1-8b-instruct".into(),
            upstream_model: "meta-llama/llama-3.1-8b-instruct".into(),
            response_id: "chatcmpl_fixture".into(),
            created_at: 1_780_000_000,
            stream: true,
            request: SourceJson::from_serializable(&json!({"stream":true}))?,
            tool_names: ToolMap::new(),
            reasoning_summary: None,
        };
        Ok(Self {
            decoder: StreamDecoder::new(plan),
            pending: None,
            terminal: None,
            usage: None,
            sse: String::new(),
        })
    }
    /// One upstream read; returns whether the relay would end the downstream body after it.
    fn read(&mut self, bytes: &str) -> TestResult<bool> {
        for event in self.decoder.push(bytes.as_bytes())? {
            if let Some(usage) = event.observed_usage() {
                self.usage = Some(usage);
            }
            if let Some(terminal) = chat_wire_terminal(&mut self.pending, &event) {
                self.terminal = Some(terminal);
            }
            self.sse
                .push_str(&String::from_utf8(self.decoder.visible_sse(event))?);
        }
        Ok(self.terminal.is_some())
    }
}

fn frame(value: &Value) -> String {
    format!("data: {value}\n\n")
}
fn content() -> String {
    frame(
        &json!({"id":"gen-1","provider":"Groq","object":"chat.completion.chunk","created":1,"choices":[{"index":0,"delta":{"role":"assistant","content":"OK"},"finish_reason":null,"native_finish_reason":null}]}),
    )
}
fn finish() -> String {
    frame(
        &json!({"id":"gen-1","object":"chat.completion.chunk","created":1,"choices":[{"index":0,"delta":{"role":"assistant","content":""},"finish_reason":"stop","native_finish_reason":"stop"}]}),
    )
}
fn usage_chunk() -> String {
    frame(
        &json!({"id":"gen-1","object":"chat.completion.chunk","created":1,"choices":[],"usage":{"prompt_tokens":17,"completion_tokens":2,"total_tokens":19,"cost":4.158e-7,"is_byok":false}}),
    )
}

#[test]
fn finish_chunk_keeps_the_relay_open_for_the_usage_chunk_and_done() -> TestResult {
    let mut tail = Tail::new()?;
    assert!(!tail.read(": OPENROUTER PROCESSING\n\n")?);
    assert!(!tail.read(&content())?);
    assert!(
        !tail.read(&finish())?,
        "a finish chunk is not the Chat wire terminal"
    );
    assert!(!tail.read(&usage_chunk())?);
    assert!(tail.read("data: [DONE]\n\n")?);
    assert_eq!(tail.terminal, Some(SemanticTerminal::Completed));
    let usage = extract_usage_tokens(tail.usage.as_ref()).ok_or("usage was not recorded")?;
    assert_eq!(
        (usage.input_tokens, usage.output_tokens),
        (Some(17), Some(2))
    );
    assert!(tail.sse.contains("\"finish_reason\":\"stop\""));
    assert!(tail.sse.contains("\"prompt_tokens\":17"));
    assert!(tail.sse.ends_with("data: [DONE]\n\n"));
    assert_eq!(tail.sse.matches("data: [DONE]").count(), 1);
    Ok(())
}

#[test]
fn a_stream_without_usage_still_ends_with_done() -> TestResult {
    let mut tail = Tail::new()?;
    assert!(!tail.read(&format!("{}{}", content(), finish()))?);
    assert!(tail.read("data: [DONE]\n\n")?);
    assert_eq!(tail.terminal, Some(SemanticTerminal::Completed));
    assert!(tail.usage.is_none(), "missing usage stays unknown");
    assert!(tail.sse.ends_with("data: [DONE]\n\n"));
    Ok(())
}

#[test]
fn one_read_carrying_finish_usage_and_done_settles_once_with_usage() -> TestResult {
    let mut tail = Tail::new()?;
    let read = format!("{}{}{}data: [DONE]\n\n", content(), finish(), usage_chunk());
    assert!(tail.read(&read)?);
    assert!(tail.usage.is_some());
    assert!(tail.sse.ends_with("data: [DONE]\n\n"));
    Ok(())
}

#[test]
fn done_without_a_finish_chunk_is_completed_and_length_stays_incomplete() -> TestResult {
    let mut tail = Tail::new()?;
    assert!(tail.read(&format!("{}data: [DONE]\n\n", content()))?);
    assert_eq!(tail.terminal, Some(SemanticTerminal::Completed));
    let mut tail = Tail::new()?;
    let length = finish().replace("\"stop\"", "\"length\"");
    assert!(!tail.read(&length)?);
    assert!(tail.read("data: [DONE]\n\n")?);
    assert_eq!(tail.terminal, Some(SemanticTerminal::Incomplete));
    Ok(())
}

#[test]
fn eof_after_the_finish_chunk_without_done_is_a_premature_eof() -> TestResult {
    let mut tail = Tail::new()?;
    assert!(!tail.read(&format!("{}{}", content(), finish()))?);
    assert!(
        tail.decoder.finish().is_err(),
        "the relay must write its failure frame, not end cleanly"
    );
    assert!(tail.terminal.is_none());
    Ok(())
}

fn headers(entries: &[(&str, &str)]) -> Headers {
    entries
        .iter()
        .map(|(name, value)| ((*name).to_owned(), (*value).to_owned()))
        .collect()
}

#[test]
fn openrouter_request_ids_come_from_the_source_header_set_only() {
    let id = |entries: &[(&str, &str)]| {
        provider_request_id(UpstreamProvider::Openrouter, &headers(entries)).map(str::to_owned)
    };
    assert_eq!(
        id(&[("X-Oneapi-Request-Id", "req-2")]).as_deref(),
        Some("req-2")
    );
    assert_eq!(
        id(&[("x-request-id", "  req-1  ")]).as_deref(),
        Some("req-1")
    );
    assert_eq!(id(&[]), None);
    // Source has no body `gen-…` fallback and reads no other spelling.
    assert_eq!(
        id(&[("x-generation-id", "gen-1"), ("request-id", "r")]),
        None
    );
    assert_eq!(id(&[("x-request-id", "")]), None);
    assert_eq!(id(&[("x-request-id", &"x".repeat(257))]), None);
    assert_eq!(
        id(&[("x-request-id", &"x".repeat(256))]).map(|v| v.len()),
        Some(256)
    );
    assert_eq!(id(&[("x-request-id", "bad\u{7f}id")]), None);
    // The first present header is normalized; a malformed one never selects a later header.
    assert_eq!(
        id(&[("x-request-id", "bad\nid"), ("x-api-request-id", "req-3")]),
        None
    );
    assert_eq!(
        id(&[("x-request-id", "a"), ("x-api-request-id", "b")]).as_deref(),
        Some("a")
    );
}

#[test]
fn request_id_bounds_count_utf16_units_and_trim_js_whitespace() {
    let id = |value: &str| {
        let headers = headers(&[("x-request-id", value)]);
        provider_request_id(UpstreamProvider::Openrouter, &headers).map(str::to_owned)
    };
    assert_eq!(id(&"é".repeat(256)).map(|v| v.chars().count()), Some(256));
    assert_eq!(id(&"😀".repeat(129)), None);
    assert_eq!(id("\u{feff}req-4\u{feff}").as_deref(), Some("req-4"));
}
