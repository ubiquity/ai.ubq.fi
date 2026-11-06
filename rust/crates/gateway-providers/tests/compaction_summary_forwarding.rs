//! Ports tests/compaction-summary-forwarding.test.ts at 129d8f4 onto the Rust projection.
use gateway_compaction::token_estimate::estimate_tokens;
use gateway_core::{error::GatewayError, js_string::JsString16};
use gateway_prompt_admission::{
    admission::{MINIMUM_OUTPUT_RESERVE_TOKENS, PROMPT_ADMISSION_VERSION},
    forwarding::{FORWARD_LIMIT, FORWARD_VERSION},
    summary::{
        Exemption, FORWARDING_EXEMPTION_LOG, SUMMARY_PREFIX, SUMMARY_PREFIX_BYTES,
        SUMMARY_PREFIX_SHA256, SUMMARY_VERSION, exemption_log_line,
    },
};
use gateway_providers::{
    chat::{ChatProjection, project_responses},
    sse::StreamDecoder,
    types::{ChatProfile, ClientSurface, DecodePlan, ResponseDecode},
};
use serde_json::{Value, json};

type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;
/// Converts a missing value or a failed result into the test's error instead of a panic.
trait Must<T> {
    fn must(self) -> TestResult<T>;
}
impl<T> Must<T> for Option<T> {
    fn must(self) -> TestResult<T> {
        self.ok_or_else(|| "missing value".into())
    }
}
impl<T, E: std::error::Error + 'static> Must<T> for Result<T, E> {
    fn must(self) -> TestResult<T> {
        self.map_err(Into::into)
    }
}

const SUMMARY_TARGET_BYTES: usize = 70 * 1024;
const FLASH: &str = "deepseek-flash";
const GPT_OSS: &str = "gpt-oss-120b";

fn summary_text(bytes: usize) -> String {
    let head = format!("{SUMMARY_PREFIX}\n");
    let fill = bytes - head.len();
    head + &"s".repeat(fill)
}
fn summary_message(text: &str, role: &str) -> Value {
    json!({"type":"message","role":role,"content":[{"type":"input_text","text":text}]})
}
fn user(text: &str) -> Value {
    summary_message(text, "user")
}
fn request(input: Vec<Value>, extra: Value) -> Value {
    let mut value = json!({"input": input});
    for (key, field) in extra.as_object().into_iter().flatten() {
        value[key] = field.clone();
    }
    value
}
fn project(raw: &Value, model: &str, profile: ChatProfile) -> Result<ChatProjection, GatewayError> {
    project_responses(raw, model, false, profile)
}
fn ok(raw: &Value, model: &str, profile: ChatProfile) -> TestResult<(ChatProjection, Value)> {
    let result = project(raw, model, profile).must()?;
    let body = result.body.deserialize::<Value>().must()?;
    Ok((result, body))
}
fn refused(raw: &Value, model: &str, profile: ChatProfile) -> TestResult<(String, String, String)> {
    let error = project(raw, model, profile).err().must()?;
    let fields = error.envelope.error;
    assert_eq!(error.status, 400);
    Ok((
        fields.code.unwrap_or_default(),
        fields.param.flatten().unwrap_or_default(),
        fields.message,
    ))
}

#[test]
fn exact_prefix_and_70_kib_envelope_are_forwarded_byte_identically() -> TestResult {
    assert_eq!(SUMMARY_PREFIX.len(), SUMMARY_PREFIX_BYTES);
    let summary = summary_text(SUMMARY_TARGET_BYTES);
    assert_eq!(summary.len(), SUMMARY_TARGET_BYTES);
    let raw = request(vec![user(&summary)], json!({}));
    let (result, body) = ok(&raw, FLASH, ChatProfile::Deepseek)?;
    assert_eq!(body["messages"], json!([{"role":"user","content":summary}]));
    assert!(result.elisions.is_empty());
    assert_eq!(
        result.exemptions,
        [Exemption::compaction_summary(
            "input[0].content".into(),
            SUMMARY_TARGET_BYTES
        )]
    );
    let exemption = &result.exemptions[0];
    assert_eq!(exemption.summary_policy, SUMMARY_VERSION);
    assert_eq!(exemption.prefix_sha256, SUMMARY_PREFIX_SHA256);
    assert_eq!(exemption.per_message_limit, FORWARD_LIMIT);
    let admission = result.admission.clone().must()?;
    assert_eq!(admission.context_window_tokens, 1_000_000);
    assert_eq!(
        admission.output_reserve_tokens,
        MINIMUM_OUTPUT_RESERVE_TOKENS
    );
    assert_eq!(
        admission.prompt_budget_tokens,
        1_000_000 - MINIMUM_OUTPUT_RESERVE_TOKENS
    );
    let serialized = JsString16::from_utf8(result.body.as_str());
    assert_eq!(
        admission.estimated_prompt_tokens,
        estimate_tokens(&serialized) + admission.structural_overhead_tokens
    );
    assert!(admission.estimated_prompt_tokens < admission.serialized_body_bytes);
    assert!(admission.admitted);

    let line = exemption_log_line(exemption, Some("req_test"), Some(FLASH), Some(&admission));
    assert_eq!(FORWARDING_EXEMPTION_LOG, "[ai.ubq.fi] forwarding_exemption");
    let logged: Value = serde_json::from_str(&line).must()?;
    let mut expected = json!({
        "policy": FORWARD_VERSION, "request_id": "req_test", "model": FLASH,
        "prompt_admission": admission,
    });
    for (key, value) in serde_json::to_value(exemption).must()?.as_object().must()? {
        expected[key] = value.clone();
    }
    expected["prefixBytes"] = json!(SUMMARY_PREFIX_BYTES);
    assert_eq!(logged, expected);
    assert_eq!(
        logged["prompt_admission"]["policy"],
        "chat-prompt-admission/v3"
    );
    Ok(())
}

#[test]
fn unprefixed_oversized_message_keeps_the_generic_reduction() -> TestResult {
    let raw = request(vec![user(&"x".repeat(SUMMARY_TARGET_BYTES))], json!({}));
    let (result, body) = ok(&raw, FLASH, ChatProfile::Deepseek)?;
    let content = body["messages"][0]["content"].as_str().must()?;
    assert!(content.len() <= FORWARD_LIMIT);
    assert!(content.contains("bytes omitted"));
    assert!(content.contains(FORWARD_VERSION));
    assert!(result.exemptions.is_empty());
    assert_eq!(result.elisions.len(), 1);
    assert_eq!(result.elisions[0].kind, "message");
    Ok(())
}

#[test]
fn wrong_role_extra_parts_missing_newline_and_media_or_tool_parts_fail_closed() -> TestResult {
    let summary = summary_text(SUMMARY_TARGET_BYTES);
    let cases = [
        summary_message(&summary, "assistant"),
        json!({"type":"agent_message","role":"user","author":"a","recipient":"b","content":[{"type":"input_text","text":summary}]}),
        json!({"type":"message","role":"user","content":[{"type":"input_text","text":summary},{"type":"input_text","text":"extra"}]}),
        user(&format!("{SUMMARY_PREFIX} without the required newline")),
        json!({"type":"message","role":"user","content":[{"type":"input_text","text":summary},{"type":"input_image","image_url":"data:image/png;base64,AQID"}]}),
        json!({"type":"message","role":"user","content":[{"type":"input_text","text":summary},{"type":"input_audio","input_audio":"AA=="}]}),
        json!({"type":"message","role":"user","content":[{"type":"input_text","text":summary},{"type":"function_call_output","output":"{}"}]}),
    ];
    for (index, item) in cases.into_iter().enumerate() {
        let (code, param, message) = refused(
            &request(vec![item], json!({})),
            FLASH,
            ChatProfile::Deepseek,
        )?;
        assert_eq!(code, "context_length_exceeded", "case {index}");
        assert_eq!(param, "input[0]", "case {index}");
        assert!(
            message.contains("The summary was not reduced or forwarded"),
            "case {index}"
        );
    }
    Ok(())
}

#[test]
fn two_exact_envelopes_fail_closed_instead_of_choosing_one() -> TestResult {
    let summary = summary_text(SUMMARY_TARGET_BYTES);
    let raw = request(vec![user(&summary), user(&summary)], json!({}));
    let (code, param, message) = refused(&raw, FLASH, ChatProfile::Deepseek)?;
    assert_eq!(code, "context_length_exceeded");
    assert_eq!(param, "input[1]");
    assert!(message.contains("adds a second summary envelope"));
    Ok(())
}

#[test]
fn prefix_bearing_string_inputs_fail_closed() -> TestResult {
    let summary = summary_text(SUMMARY_TARGET_BYTES);
    let (code, param, _) = refused(&json!({"input": summary}), FLASH, ChatProfile::Deepseek)?;
    assert_eq!(
        (code.as_str(), param.as_str()),
        ("context_length_exceeded", "input")
    );
    let raw = request(vec![json!(summary)], json!({}));
    let (code, param, _) = refused(&raw, FLASH, ChatProfile::Deepseek)?;
    assert_eq!(
        (code.as_str(), param.as_str()),
        ("context_length_exceeded", "input[0]")
    );
    Ok(())
}

#[test]
fn summary_exemption_does_not_relax_tool_output_bounds() -> TestResult {
    let summary = summary_text(SUMMARY_TARGET_BYTES);
    let raw = request(
        vec![
            user(&summary),
            json!({"type":"function_call","name":"read_file","arguments":"{}","call_id":"call_after_summary"}),
            json!({"type":"function_call_output","call_id":"call_after_summary","output":"o".repeat(SUMMARY_TARGET_BYTES)}),
        ],
        json!({}),
    );
    let (result, body) = ok(&raw, FLASH, ChatProfile::Deepseek)?;
    let messages = body["messages"].as_array().must()?;
    assert_eq!(messages[0]["content"], json!(summary));
    let tool = messages.last().must()?["content"].as_str().must()?;
    assert!(tool.len() <= FORWARD_LIMIT);
    assert!(tool.contains("bytes omitted"));
    assert_eq!(result.exemptions.len(), 1);
    assert_eq!(result.elisions.len(), 1);
    assert_eq!(result.elisions[0].kind, "tool_output");
    Ok(())
}

#[test]
fn tool_arguments_and_reasoning_retain_their_generic_bounds() -> TestResult {
    let oversized = "x".repeat(FORWARD_LIMIT + 1_024);
    let call = json!({"type":"function_call","name":"read_file","arguments":oversized,"call_id":"call_args"});
    let (result, body) = ok(
        &request(vec![call.clone()], json!({})),
        FLASH,
        ChatProfile::Deepseek,
    )?;
    let arguments = body["messages"][0]["tool_calls"][0]["function"]["arguments"]
        .as_str()
        .must()?;
    assert!(arguments.len() <= FORWARD_LIMIT);
    assert!(arguments.contains("bytes omitted"));
    let parsed: Value = serde_json::from_str(arguments).must()?;
    assert_eq!(parsed["gateway_elision"]["kind"], "tool_arguments");
    assert_eq!(parsed["gateway_elision"]["policy"], FORWARD_VERSION);
    assert_eq!(parsed["gateway_elision"]["originalBytes"], oversized.len());
    assert_eq!(parsed["gateway_elision"]["omittedBytes"], oversized.len());
    let kinds = result
        .elisions
        .iter()
        .map(|e| e.kind.as_str())
        .collect::<Vec<_>>();
    assert_eq!(kinds, ["tool_arguments"]);
    assert_eq!(result.elisions[0].path, "input[0].arguments");

    let custom = json!({"type":"custom_tool_call","name":"apply_patch","input":oversized,"call_id":"call_custom"});
    let (_, body) = ok(
        &request(vec![custom], json!({})),
        FLASH,
        ChatProfile::Deepseek,
    )?;
    let arguments = body["messages"][0]["tool_calls"][0]["function"]["arguments"]
        .as_str()
        .must()?;
    let parsed: Value = serde_json::from_str(arguments).must()?;
    assert_eq!(parsed["gateway_elision"]["kind"], "tool_arguments");

    let reasoning = request(
        vec![
            json!({"type":"reasoning","summary":[{"type":"summary_text","text":oversized}]}),
            json!({"type":"message","role":"assistant","content":[{"type":"output_text","text":"answer"}]}),
        ],
        json!({}),
    );
    let (result, body) = ok(&reasoning, FLASH, ChatProfile::Deepseek)?;
    let replay = body["messages"][0]["reasoning_content"].as_str().must()?;
    assert!(replay.len() <= FORWARD_LIMIT);
    assert!(replay.contains("bytes omitted"));
    let kinds = result
        .elisions
        .iter()
        .map(|e| e.kind.as_str())
        .collect::<Vec<_>>();
    assert_eq!(kinds, ["reasoning"]);
    assert_eq!(result.elisions[0].path, "input[0]");

    let raw = request(vec![call], json!({"truncation":"disabled"}));
    let (code, param, message) = refused(&raw, FLASH, ChatProfile::Deepseek)?;
    assert_eq!(param, "input[0].arguments");
    assert_eq!(code, "context_length_exceeded");
    assert!(message.contains("(tool call call_args)"));
    Ok(())
}

#[test]
fn truncation_disabled_accepts_an_exempt_summary_only_inside_aggregate_admission() -> TestResult {
    let summary = summary_text(SUMMARY_TARGET_BYTES);
    let raw = request(vec![user(&summary)], json!({"truncation":"disabled"}));
    let (result, body) = ok(&raw, GPT_OSS, ChatProfile::Cerebras)?;
    assert_eq!(body["messages"][0]["content"], json!(summary));
    assert_eq!(result.exemptions.len(), 1);
    let raw = request(
        vec![user(&summary_text(720 * 1024))],
        json!({"truncation":"disabled"}),
    );
    let (code, param, message) = refused(&raw, GPT_OSS, ChatProfile::Cerebras)?;
    assert_eq!(code, "context_length_exceeded");
    assert_eq!(param, "input");
    assert!(message.contains("no payload was dispatched"));
    Ok(())
}

fn originals(count: usize) -> Vec<Value> {
    (0..count)
        .map(|index| {
            user(&format!(
                "original message {index} {}",
                "x".repeat(60 * 1024)
            ))
        })
        .collect()
}

#[test]
fn a_steer_preserves_original_users_before_an_adopted_summary() -> TestResult {
    let old = originals(2);
    let summary = summary_text(720 * 1024);
    let mut input = old.clone();
    input.extend([user(&summary), user("steer now")]);
    let raw = request(input, json!({"instructions":"keep these instructions"}));
    let before = raw.clone();
    let result = project_responses(&raw, FLASH, true, ChatProfile::Deepseek).must()?;
    let body = result.body.deserialize::<Value>().must()?;
    let mut expected = vec![json!({"role":"system","content":"keep these instructions"})];
    expected.extend(
        old.iter()
            .map(|message| json!({"role":"user","content":message["content"][0]["text"]})),
    );
    expected.extend([
        json!({"role":"user","content":summary}),
        json!({"role":"user","content":"steer now"}),
    ]);
    assert_eq!(body["messages"], json!(expected));
    assert_eq!(raw, before);
    assert!(result.elisions.is_empty());
    assert_eq!(result.admission.must()?.policy, PROMPT_ADMISSION_VERSION);
    Ok(())
}

#[test]
fn an_overflowing_steer_refuses_every_truncation_mode_without_deleting_history() -> TestResult {
    let mut input = originals(100);
    input.extend([user(&summary_text(720 * 1024)), user("steer now")]);
    for truncation in [None, Some("auto"), Some("disabled")] {
        let raw = match truncation {
            Some(value) => request(input.clone(), json!({"truncation": value})),
            None => request(input.clone(), json!({})),
        };
        let before = raw.clone();
        let error = project_responses(&raw, FLASH, true, ChatProfile::Deepseek)
            .err()
            .must()?;
        assert_eq!(error.status, 400);
        let fields = &error.envelope.error;
        assert_eq!(fields.code.as_deref(), Some("context_length_exceeded"));
        assert_eq!(fields.param, Some(Some("input".into())));
        assert!(fields.message.contains("no payload was dispatched"));
        assert_eq!(raw, before);
    }
    Ok(())
}

#[test]
fn instructions_tools_and_images_count_beyond_the_summary_envelope() -> TestResult {
    let summary = summary_text(SUMMARY_TARGET_BYTES);
    let image = format!("data:image/png;base64,{}", "A".repeat(55 * 1024));
    let mut images = vec![user(&summary)];
    images.extend(
        (0..12).map(|_| json!({"type":"message","role":"user","content":[{"type":"input_image","image_url":image}]})),
    );
    let cases = [
        request(
            vec![user(&summary_text(620 * 1024))],
            json!({"instructions":"i".repeat(65_536)}),
        ),
        request(
            vec![user(&summary)],
            json!({"tools":[{"type":"function","name":"large","description":"t".repeat(700 * 1024),"parameters":{"type":"object","properties":{}}}]}),
        ),
        json!({"input": images}),
    ];
    for (index, raw) in cases.iter().enumerate() {
        let (code, param, _) = refused(raw, GPT_OSS, ChatProfile::Cerebras)?;
        assert_eq!(code, "context_length_exceeded", "aggregate case {index}");
        assert_eq!(param, "input", "aggregate case {index}");
    }
    Ok(())
}

#[test]
fn deepseek_lithos_and_cerebras_windows_stay_distinct() -> TestResult {
    let raw = request(vec![user(&summary_text(720 * 1024))], json!({}));
    assert!(project(&raw, FLASH, ChatProfile::Deepseek).is_ok());
    assert!(project(&raw, "deepseek-ai/DeepSeek-V4.1-Flash", ChatProfile::Lithos).is_ok());
    let (code, _, _) = refused(&raw, GPT_OSS, ChatProfile::Cerebras)?;
    assert_eq!(code, "context_length_exceeded");
    let windows = [
        (FLASH, ChatProfile::Deepseek, 1_000_000),
        (
            "deepseek-ai/DeepSeek-V4.1-Flash",
            ChatProfile::Lithos,
            1_048_576,
        ),
        (GPT_OSS, ChatProfile::Cerebras, 131_072),
    ];
    let small = request(vec![user("hello")], json!({}));
    for (model, profile, window) in windows {
        let admission = project(&small, model, profile).must()?.admission.must()?;
        assert_eq!(admission.context_window_tokens, window, "{profile:?}");
    }
    Ok(())
}

#[test]
fn max_output_tokens_above_the_floor_becomes_the_output_reserve() -> TestResult {
    let raw = request(vec![user("hello")], json!({"max_output_tokens": 40_000}));
    let admission = project(&raw, FLASH, ChatProfile::Deepseek)
        .must()?
        .admission
        .must()?;
    assert_eq!(admission.requested_output_tokens, Some(40_000));
    assert_eq!(admission.output_reserve_tokens, 40_000);
    assert_eq!(admission.prompt_budget_tokens, 960_000);
    Ok(())
}

#[test]
fn the_calibrated_estimator_admits_a_1_5_mib_normal_transcript() -> TestResult {
    let base = "alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima mike november oscar papa quebec romeo sierra tango uniform victor whiskey xray yankee zulu ";
    let chunk = base
        .repeat((64 * 1024usize).div_ceil(base.len()))
        .chars()
        .take(64 * 1024)
        .collect::<String>();
    let raw = request((0..24).map(|_| user(&chunk)).collect(), json!({}));
    let (result, _) = ok(&raw, FLASH, ChatProfile::Deepseek)?;
    assert!(result.elisions.is_empty());
    let admission = result.admission.must()?;
    assert!(admission.serialized_body_bytes > 1_500_000);
    assert!(admission.estimated_prompt_tokens < 1_000_000);
    Ok(())
}

#[test]
fn cerebras_measures_the_body_after_its_projection_drops_reasoning() -> TestResult {
    let reasoning = json!({"type":"reasoning","summary":[{"type":"summary_text","text":"r".repeat(60 * 1024)}]});
    let mut input = vec![user(&summary_text(SUMMARY_TARGET_BYTES))];
    input.extend((0..20).map(|_| reasoning.clone()));
    input.push(json!({"type":"message","role":"assistant","content":[{"type":"output_text","text":"answer"}]}));
    let raw = request(input, json!({}));
    let (result, body) = ok(&raw, GPT_OSS, ChatProfile::Cerebras)?;
    let admission = result.admission.must()?;
    assert!(admission.admitted);
    assert!(admission.serialized_body_bytes < 200_000);
    // The unprojected Chat body keeps the replayed reasoning and would overflow the window.
    assert!(body.to_string().len() > 1_000_000);
    Ok(())
}

#[test]
fn the_744586_byte_image_incident_is_reduced_before_aggregate_admission() -> TestResult {
    let prefix = "data:image/png;base64,";
    let image = format!("{prefix}{}", "A".repeat(744_586 - prefix.len()));
    assert_eq!(image.len(), 744_586);
    let raw = request(
        vec![
            json!({"type":"message","role":"user","content":[{"type":"input_text","text":"inspect the image"},{"type":"input_image","image_url":image}]}),
        ],
        json!({}),
    );
    let (result, _) = ok(&raw, FLASH, ChatProfile::Deepseek)?;
    assert!(!result.body.as_str().contains(&image));
    assert_eq!(result.elisions.len(), 1);
    assert_eq!(result.elisions[0].kind, "image");
    assert_eq!(result.elisions[0].original_bytes, 744_586);
    assert_eq!(result.elisions[0].omitted_bytes, 744_586);
    assert!(result.admission.must()?.admitted);
    Ok(())
}

fn codex_plan(provider: gateway_core::provider::UpstreamProvider) -> TestResult<DecodePlan> {
    Ok(DecodePlan {
        provider,
        surface: ClientSurface::Responses,
        decode: ResponseDecode::Responses,
        model: "gpt-test".into(),
        upstream_model: "gpt-test".into(),
        response_id: "resp_gateway".into(),
        created_at: 1_780_000_000,
        stream: true,
        request: gateway_core::source_json::SourceJson::from_serializable(&json!({})).must()?,
        tool_names: Default::default(),
        reasoning_summary: None,
    })
}

/// tests/codex-responses-error.test.ts at the decoder seam: lifecycle keeps the original error.
#[test]
fn codex_subscription_stream_reencodes_only_the_client_visible_context_error() -> TestResult {
    use gateway_core::provider::UpstreamProvider;
    let stream = concat!(
        "event: response.created\ndata: {\"type\":\"response.created\",\"sequence_number\":0,\"response\":{\"id\":\"resp_up\",\"object\":\"response\",\"status\":\"in_progress\",\"model\":\"gpt-test\",\"output\":[]}}\n\n",
        "data: {\"type\":\"response.output_text.delta\",\"sequence_number\":1,\"item_id\":\"msg_1\",\"delta\":\"partial\"}\n\n",
        "data: {\"type\":\"error\",\"sequence_number\":2,\"code\":\"context_length_exceeded\",\"message\":\"full\",\"param\":\"input\"}\n\n",
    );
    for (provider, expected) in [
        (UpstreamProvider::ChatgptCodex, "response.failed"),
        (UpstreamProvider::Openrouter, "error"),
    ] {
        let mut decoder = StreamDecoder::new(codex_plan(provider)?);
        let events = decoder.push(stream.as_bytes()).must()?;
        assert_eq!(events.last().must()?.type_name().as_deref(), Some("error"));
        let visible = events
            .into_iter()
            .map(|event| decoder.client_visible(event))
            .collect::<Vec<_>>();
        let last = visible.last().must()?;
        assert_eq!(last.type_name().as_deref(), Some(expected), "{provider:?}");
        if provider == UpstreamProvider::ChatgptCodex {
            let value = last.data.deserialize::<Value>().must()?;
            assert_eq!(value["response"]["id"], "resp_up");
            assert_eq!(
                value["response"]["error"]["code"],
                "context_length_exceeded"
            );
            assert_eq!(
                value["response"]["output"][0]["content"][0]["text"],
                "partial"
            );
            assert!(
                last.encode_sse()
                    .starts_with("event: response.failed\ndata: ")
            );
        }
    }
    Ok(())
}
