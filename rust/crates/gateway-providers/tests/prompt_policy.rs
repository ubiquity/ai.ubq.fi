//! Current source policy is exercised through the actual Responses projection.
use gateway_core::{model::ModelId, source_json::SourceJson, source_serialization as source};
use gateway_prompt_policy::{admission, payload, summary};
use gateway_provider_chat::source_chat::project;
use gateway_provider_source::types::ChatProfile;
use serde_json::{Value, json};
type TestResult = Result<(), Box<dyn std::error::Error>>;
fn envelope(text: &str) -> Value {
    json!({"type":"message","role":"user","content":[{"type":"input_text","text":text}]})
}
fn raw(input: Value) -> Value {
    json!({"input":input,"reasoning":{"effort":"none"}})
}
fn projection(
    value: &Value,
    profile: ChatProfile,
) -> Result<gateway_provider_protocol::source_chat::Projection, gateway_core::error::GatewayError> {
    project(
        &SourceJson::from_serializable(value)?,
        &ModelId::from_scalar("deepseek-flash"),
        false,
        profile,
    )
    .map_err(|error| error.into_scalar())
}
fn profiles() -> [ChatProfile; 3] {
    [
        ChatProfile::Deepseek,
        ChatProfile::Lithos,
        ChatProfile::Cerebras,
    ]
}
#[test]
fn exact_summary_is_whole_unicode_and_all_earlier_steers_survive_every_mode() -> TestResult {
    let text = format!("{}\n{}", summary::PREFIX, "é😀".repeat(20_000));
    for profile in profiles() {
        for truncation in [None, Some("auto"), Some("disabled")] {
            let mut request = raw(
                json!([{"role":"developer","content":"keep the early steer"}, envelope(&text), {"role":"user","content":"next input"}]),
            );
            if let Some(mode) = truncation {
                request["truncation"] = json!(mode);
            }
            let before = request.clone();
            let projected = projection(&request, profile)?;
            let body: Value = projected.body.deserialize()?;
            assert_eq!(body["messages"][0]["content"], "keep the early steer");
            assert_eq!(body["messages"][1]["content"], text);
            assert_eq!(body["messages"][2]["content"], "next input");
            assert_eq!(projected.exemptions.len(), 1);
            assert_eq!(
                projected.exemptions[0].prefix_sha256,
                summary::PREFIX_SHA256
            );
            assert_eq!(projected.exemptions[0].bytes, text.len());
            assert!(projected.admission.ok_or("missing admission")?.admitted);
            assert!(projected.elisions.is_empty());
            assert_eq!(request, before);
        }
    }
    Ok(())
}
#[test]
fn malformed_and_multiple_summary_envelopes_fail_before_item_projection() -> TestResult {
    let text = format!("{}\nsummary", summary::PREFIX);
    let mut wrong_role = envelope(&text);
    wrong_role["role"] = json!("assistant");
    let mut extra = envelope(&text);
    extra["content"]
        .as_array_mut()
        .ok_or("content")?
        .push(json!({"type":"input_text","text":"extra"}));
    let mut media = envelope(&text);
    media["content"]
        .as_array_mut()
        .ok_or("content")?
        .push(json!({"type":"input_image","image_url":"https://example.test/image"}));
    let mut wrong_part = envelope(&text);
    wrong_part["content"][0]["type"] = json!("output_text");
    let mut tool = envelope(&text);
    tool["type"] = json!("function_call");
    let cases = [
        json!(text),
        json!([text]),
        json!([wrong_role]),
        json!([extra]),
        json!([media]),
        json!([wrong_part]),
        json!([tool]),
        json!([envelope(summary::PREFIX)]),
        json!([envelope(&format!("{}\n", summary::PREFIX))]),
        json!([envelope(&text), envelope(&text)]),
        json!([17, wrong_role]),
    ];
    for input in cases {
        let request = SourceJson::from_serializable(&raw(input))?;
        let error = match project(
            &request,
            &"deepseek-flash".into(),
            false,
            ChatProfile::Deepseek,
        ) {
            Err(error) => error,
            Ok(_) => return Err("malformed summary was admitted".into()),
        };
        assert_eq!(error.status, 400);
        assert!(!error.is_local_aggregate_context_overflow());
        let body: Value = error.body().deserialize()?;
        assert_eq!(body["error"]["code"], "context_length_exceeded");
        assert!(
            body["error"]["message"]
                .as_str()
                .ok_or("message")?
                .contains(summary::VERSION)
        );
    }
    Ok(())
}
#[test]
fn parseable_argument_elision_accounts_for_every_omitted_byte() -> TestResult {
    let arguments = format!("{{\"payload\":\"{}\"}}", "x".repeat(90_000));
    let mut request = raw(
        json!([{"type":"function_call","name":"lookup","call_id":"call-1","arguments":arguments}]),
    );
    let projected = projection(&request, ChatProfile::Deepseek)?;
    let body: Value = projected.body.deserialize()?;
    let encoded = body["messages"][0]["tool_calls"][0]["function"]["arguments"]
        .as_str()
        .ok_or("arguments")?;
    let replacement: Value = serde_json::from_str(encoded)?;
    assert_eq!(replacement["gateway_elision"]["policy"], payload::VERSION);
    assert_eq!(
        replacement["gateway_elision"]["omittedBytes"],
        arguments.len()
    );
    assert_eq!(projected.elisions[0].omitted_bytes, arguments.len());
    assert_eq!(projected.elisions[0].original_bytes, arguments.len());
    assert!(encoded.len() <= payload::PER_MESSAGE_LIMIT);
    request["truncation"] = json!("disabled");
    assert!(projection(&request, ChatProfile::Deepseek).is_err());
    Ok(())
}
#[test]
fn all_profile_windows_reserves_and_safe_integer_validation_are_real() -> TestResult {
    for (profile, window) in profiles().into_iter().zip([1_000_000, 1_048_576, 131_072]) {
        let mut request = raw(json!("hello"));
        let admitted = projection(&request, profile)?
            .admission
            .ok_or("admission")?;
        assert_eq!(admitted.context_window_tokens, window);
        assert_eq!(admitted.output_reserve_tokens, 16_384);
        request["max_output_tokens"] = json!(20_000);
        assert_eq!(
            projection(&request, profile)?
                .admission
                .ok_or("admission")?
                .output_reserve_tokens,
            20_000
        );
        for invalid in [
            json!(0),
            json!(-1),
            json!(1.5),
            json!(9_007_199_254_740_992_u64),
        ] {
            request["max_output_tokens"] = invalid;
            let source = SourceJson::from_serializable(&request)?;
            let error = match project(&source, &"deepseek-flash".into(), false, profile) {
                Err(error) => error,
                Ok(_) => return Err("invalid output reserve admitted".into()),
            };
            assert!(!error.is_local_aggregate_context_overflow());
        }
        request["max_output_tokens"] = json!(9_007_199_254_740_991_u64);
        let source = SourceJson::from_serializable(&request)?;
        let error = match project(&source, &"deepseek-flash".into(), false, profile) {
            Err(error) => error,
            Ok(_) => return Err("zero budget admitted".into()),
        };
        assert!(error.is_local_aggregate_context_overflow());
    }
    Ok(())
}
#[test]
fn complete_body_counts_and_cerebras_real_dropped_projection_are_bound() -> TestResult {
    let request = json!({"input":[{"role":"developer","content":"first steer"},{"type":"reasoning","summary":[{"text":"private reasoning"}]},{"role":"assistant","content":"answer"},{"role":"user","content":[{"type":"input_text","text":"image question"},{"type":"input_image","image_url":"https://example.test/image"}]}],"instructions":"original instructions","reasoning":{"effort":"none"},"tools":[{"type":"function","name":"lookup","parameters":{"type":"object","properties":{"needle":{"type":"string"}}}}]});
    let projected = projection(&request, ChatProfile::Cerebras)?;
    let mut actual = projected.body.clone();
    gateway_provider_chat_controls::projection::prepare(&mut actual)
        .map_err(|error| error.into_scalar())?;
    let body: Value = actual.deserialize()?;
    assert_eq!(body["stream"], false);
    assert!(body["messages"][1].get("reasoning_content").is_none());
    let measured = admission::estimate(&actual, admission::PromptProfile::Cerebras)?;
    assert_eq!(projected.admission.ok_or("admission")?, measured);
    assert_eq!(
        measured.structural_overhead_tokens,
        256 + 16 * body["messages"].as_array().ok_or("messages")?.len() as u64 + 32
    );
    assert!(
        body["messages"][0]["content"]
            .as_str()
            .ok_or("instructions")?
            .contains(gateway_provider_history::chat::CONTINUATION)
    );
    assert_eq!(
        measured.serialized_body_bytes,
        source::canonical(&actual)?.as_bytes().len() as u64
    );
    Ok(())
}
#[test]
fn aggregate_overflow_is_typed_for_all_profiles_and_all_history_modes() -> TestResult {
    let text = format!("{}\n{}", summary::PREFIX, "x ".repeat(1_100_000));
    for profile in profiles() {
        for mode in [None, Some("auto"), Some("disabled")] {
            let mut value =
                raw(json!([{"role":"developer","content":"retain this steer"}, envelope(&text)]));
            if let Some(mode) = mode {
                value["truncation"] = json!(mode);
            }
            let request = SourceJson::from_serializable(&value)?;
            let before = request.clone();
            let error = match project(&request, &"deepseek-flash".into(), true, profile) {
                Err(error) => error,
                Ok(_) => return Err("overflow admitted".into()),
            };
            assert!(error.is_local_aggregate_context_overflow());
            let routing = gateway_local_refusal::routing_error::RoutingError::from(error.clone());
            assert!(routing.clone().is_local_aggregate_context_overflow());
            assert_eq!(
                routing.source_body.ok_or("body")?.as_bytes(),
                error.body().as_bytes()
            );
            assert_eq!(request, before);
        }
    }
    Ok(())
}
#[test]
fn native_metadata_never_exempts_ordinary_messages_and_lone_summary_units_survive() -> TestResult {
    let request = json!({"input":[{"role":"user","content":"z".repeat(90_000)}],"metadata":{"summary":true},"session_id":"session","thread_id":"thread","turn_id":"turn"});
    let projected = projection(&request, ChatProfile::Deepseek)?;
    assert!(projected.exemptions.is_empty());
    assert_eq!(projected.elisions.len(), 1);
    let source = SourceJson::parse(&format!(
        r#"{{"input":[{{"role":"user","content":[{{"type":"input_text","text":"{}\n\ud800"}}]}}]}}"#,
        summary::PREFIX
    ))?;
    let projected = project(
        &source,
        &"deepseek-flash".into(),
        false,
        ChatProfile::Deepseek,
    )
    .map_err(|error| error.into_scalar())?;
    let messages = source::member(&projected.body, "messages")
        .and_then(|value| source::array(&value))
        .ok_or("messages")?;
    let text = source::member(&messages[0], "content")
        .and_then(|value| source::string(&value))
        .ok_or("text")?;
    assert_eq!(text.as_units().last(), Some(&0xd800));
    Ok(())
}
#[test]
fn nonempty_reasoning_tiers_remain_verbatim_and_policy_advertises_v2() -> TestResult {
    // Advertised DATA keeps the original tier; source wire projection is a later boundary.
    let record =
        SourceJson::parse(r#"{"supported_reasoning_levels":["ultra","provider-future-tier"]}"#)?;
    let advertised = gateway_schema::reasoning::ModelReasoning::from_record(Some(&record));
    assert_eq!(
        advertised.levels,
        vec!["ultra".into(), "provider-future-tier".into()]
    );
    assert_eq!(
        gateway_schema::reasoning::effort_for_codex(&"none".into(), &advertised),
        "none".into()
    );
    for profile in profiles() {
        for (tier, deepseek_wire) in [
            ("none", "none"),
            ("ultra", "max"),
            (" ULTRA ", "max"),
            ("provider-future-tier", "provider-future-tier"),
            (" HIGH ", " HIGH "),
        ] {
            let mut value = raw(json!("hello"));
            value["reasoning"]["effort"] = json!(tier);
            let original = value.clone();
            let expected = if profile == ChatProfile::Deepseek {
                deepseek_wire
            } else {
                tier
            };
            assert_eq!(
                projection(&value, profile)?.body.deserialize::<Value>()?["reasoning_effort"],
                expected
            );
            assert_eq!(value, original);
        }
    }
    let literal = gateway_core::js_string::JsString16::from_units(vec![117, 108, 116, 114, 0xd800]);
    let quoted = gateway_provider_chat::source_chat::effort(
        &literal,
        ChatProfile::Deepseek,
        "reasoning.effort",
    )
    .map_err(|error| error.into_scalar())?;
    assert_eq!(source::string(&quoted), Some(literal));
    let policy: Value =
        gateway_provider_policy::policy::forwarded_payload_policy()?.deserialize()?;
    assert_eq!(policy["version"], "deepseek-forwarded-payload/v2");
    assert_eq!(policy["mode"], "bytes");
    assert_eq!(policy["perMessageLimit"], 65_536);
    Ok(())
}
