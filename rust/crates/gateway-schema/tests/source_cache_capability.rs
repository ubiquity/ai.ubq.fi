//! Selected metadata and request/model/error strings remain UTF16 SourceJson.
use gateway_chat_normalization::normalized::normalize_chat_messages;
use gateway_local_refusal::source_error::SourceRequestError;
use gateway_prompt_cache::capability::validate_known_unsupported_prompt_cache;
use gateway_responses_normalization::normalized::normalize_responses_input;
use gateway_source::{
    js_string::JsString16, source_json::SourceJson, source_serialization as source,
};
use serde_json::{Value, json};
type TestResult = Result<(), Box<dyn std::error::Error>>;
fn raw(value: &str) -> SourceJson {
    SourceJson::parse(value).unwrap()
}
fn capabilities(controls: Value) -> Value {
    let mut record = json!({"source":"catalog","verified_at_ms":1});
    for (key, value) in controls.as_object().unwrap() {
        record[key] = value.clone();
    }
    json!({"version":1,"providers":[{"id":prompt_cache::CODEX_PROVIDER,"controls":record}]})
}
fn param(error: &SourceRequestError) -> String {
    source::string(
        &source::member(&source::member(error.body(), "error").unwrap(), "param").unwrap(),
    )
    .unwrap()
    .try_utf8()
    .unwrap()
}
fn check(
    model: &JsString16,
    cap: Option<&Value>,
    body: &SourceJson,
    input: &[SourceJson],
    key: &str,
) -> Result<(), SourceRequestError> {
    let cap = cap
        .map(SourceJson::from_serializable)
        .transpose()
        .map_err(SourceRequestError::from_gateway)?;
    validate_known_unsupported_prompt_cache(model, cap.as_ref(), body, input, key)
}
#[test]
fn false_capability_preserves_lone_model_and_original_payload() -> TestResult {
    let model = JsString16::from_units(vec![0xd800]);
    let body = raw(r#"{"input":"\udfff","prompt_cache_key":"cache\ud800"}"#);
    let before = body.clone();
    let input = normalize_responses_input(Some(&body.object_entries()?[0].1))
        .map_err(|error| error.diagnostic().clone())?;
    let error = check(&model, Some(&Value::Bool(false)), &body, &input, "input")
        .err()
        .ok_or("expected fixture error")?;
    assert_eq!(param(&error), "prompt_cache_key");
    assert_eq!(body, before);
    let actual = source::member(error.body(), "error").ok_or("missing fixture value")?;
    let message =
        source::string(&source::member(&actual, "message").ok_or("missing fixture value")?)
            .ok_or("missing fixture value")?;
    let mut expected = "Prompt caching is not supported for model '"
        .encode_utf16()
        .collect::<Vec<_>>();
    expected.push(0xd800);
    expected.extend("'.".encode_utf16());
    assert_eq!(message.as_units(), expected);
    assert_eq!(error.status, 400);
    Ok(())
}
#[test]
fn missing_other_provider_and_omitted_controls_remain_unknown() {
    let model = JsString16::from_utf8("model");
    let body = raw(r#"{"prompt_cache_options":{"mode":"explicit"},"instructions":"\ud800"}"#);
    for metadata in [
        None,
        Some(
            json!({"version":1,"providers":[{"id":"other","controls":{"source":"catalog","verified_at_ms":1,"explicit_breakpoints":false}}]}),
        ),
        Some(capabilities(json!({}))),
        Some(json!({"version":1,"providers":[{"id":prompt_cache::CODEX_PROVIDER}]})),
    ] {
        check(&model, metadata.as_ref(), &body, &[], "input").unwrap();
    }
}
#[test]
fn selected_control_refusals_follow_source_precedence() {
    let model = JsString16::from_utf8("model");
    let cap = capabilities(
        json!({"key":false,"implicit":false,"explicit_breakpoints":false,"legacy_retentions":["24h"]}),
    );
    for (body, expected) in [
        (
            r#"{"prompt_cache_key":"key","prompt_cache_options":{"mode":"implicit"},"prompt_cache_retention":"in_memory"}"#,
            "prompt_cache_key",
        ),
        (
            r#"{"prompt_cache_options":{"mode":"implicit"},"prompt_cache_retention":"in_memory"}"#,
            "prompt_cache_options.mode",
        ),
        (
            r#"{"prompt_cache_options":{"mode":"explicit"},"prompt_cache_retention":"in_memory"}"#,
            "prompt_cache_retention",
        ),
        (
            r#"{"prompt_cache_options":{"mode":"explicit"}}"#,
            "prompt_cache_options.mode",
        ),
    ] {
        assert_eq!(
            param(&check(&model, Some(&cap), &raw(body), &[], "input").unwrap_err()),
            expected
        );
    }
}
#[test]
fn normalized_breakpoint_detection_keeps_original_response_and_chat_paths() -> TestResult {
    let model = JsString16::from_utf8("model");
    let cap = capabilities(json!({"explicit_breakpoints":false}));
    let body = raw(
        r#"{"input":["prefix",{"type":"input_text","text":"\ud800","prompt_cache_breakpoint":{"mode":"explicit"}}]}"#,
    );
    let input = normalize_responses_input(source::member(&body, "input").as_ref())
        .map_err(|error| error.diagnostic().clone())?;
    assert_eq!(
        param(
            &check(&model, Some(&cap), &body, &input, "input")
                .err()
                .ok_or("expected fixture error")?
        ),
        "input[1].prompt_cache_breakpoint"
    );
    // A raw marker is ignored unless the actual normalized input retains it.
    check(&model, Some(&cap), &body, &[], "input").map_err(|error| error.diagnostic().clone())?;
    let body = raw(
        r#"{"messages":[{"role":"developer","content":[{"type":"text","text":"\udfff","prompt_cache_breakpoint":{"mode":"explicit"}}]}]}"#,
    );
    let input =
        normalize_chat_messages(&source::member(&body, "messages").ok_or("missing fixture value")?)
            .map_err(|error| error.diagnostic().clone())?;
    assert_eq!(
        param(
            &check(&model, Some(&cap), &body, &input.input, "messages")
                .err()
                .ok_or("expected fixture error")?
        ),
        "messages[0].content[0].prompt_cache_breakpoint"
    );
    Ok(())
}
#[test]
fn block_type_and_function_output_paths_use_the_same_source_policy() -> TestResult {
    let model = JsString16::from_utf8("model");
    let cap = capabilities(
        json!({"explicit_breakpoints":true,"breakpoint_block_types":{"responses":["input_image"]}}),
    );
    let body = raw(
        r#"{"input":[{"type":"function_call_output","call_id":"\ud800","output":[{"type":"input_text","text":"\udfff","prompt_cache_breakpoint":{"mode":"explicit"}}]}]}"#,
    );
    let input = normalize_responses_input(source::member(&body, "input").as_ref())
        .map_err(|error| error.diagnostic().clone())?;
    assert_eq!(
        param(
            &check(&model, Some(&cap), &body, &input, "input")
                .err()
                .ok_or("expected fixture error")?
        ),
        "input[0].output[0].prompt_cache_breakpoint"
    );
    Ok(())
}
#[test]
fn legacy_scalar_wrapper_delegates_without_error_envelope_change() {
    let record = json!({"prompt_cache":capabilities(json!({"implicit":false}))});
    let body = json!({"input":"original","prompt_cache_options":{"mode":"implicit"}});
    let scalar = gateway_prompt_cache::compat::validate_known_unsupported(
        "model",
        Some(&record),
        &body,
        &[],
        "input",
    )
    .unwrap_err();
    let source = check(
        &JsString16::from_utf8("model"),
        record.get("prompt_cache"),
        &SourceJson::from_serializable(&body).unwrap(),
        &[],
        "input",
    )
    .unwrap_err();
    assert_eq!(
        source.body(),
        &SourceJson::from_serializable(&scalar.envelope).unwrap()
    );
}

#[test]
fn opaque_other_provider_does_not_hide_current_provider_capabilities() -> TestResult {
    let cap = raw(
        r#"{"version":1,"providers":[{"id":"\ud800","controls":{"source":"catalog","verified_at_ms":1,"key":true}},{"id":"codex_chatgpt","controls":{"source":"catalog","verified_at_ms":1,"key":false}}]}"#,
    );
    let normalized = gateway_prompt_cache::metadata::normalize_capabilities_source(Some(&cap))
        .ok_or("missing fixture value")?;
    let providers =
        source::array(&source::member(&normalized, "providers").ok_or("missing fixture value")?)
            .ok_or("missing fixture value")?;
    assert_eq!(
        source::string(&source::member(&providers[0], "id").ok_or("missing fixture value")?)
            .ok_or("missing fixture value")?
            .as_units(),
        &[0xd800]
    );
    let error = validate_known_unsupported_prompt_cache(
        &JsString16::from_utf8("model"),
        Some(&cap),
        &raw(r#"{"prompt_cache_key":"key"}"#),
        &[],
        "input",
    )
    .err()
    .ok_or("expected fixture error")?;
    assert_eq!(param(&error), "prompt_cache_key");
    Ok(())
}
