use gateway_prompt_policy::{
    admission::{self, PromptProfile},
    payload::{self, PER_MESSAGE_LIMIT, PayloadKind},
    summary,
    tokens::estimate_tokens,
};
use gateway_source::{
    js_string::JsString16, source_json::SourceJson, source_serialization as json,
};

type TestResult = Result<(), String>;
fn source(text: &str) -> Result<SourceJson, String> {
    SourceJson::parse(text).map_err(|error| format!("{error:?}"))
}
fn generated(value: &impl serde::Serialize) -> Result<SourceJson, String> {
    SourceJson::from_serializable(value).map_err(|error| format!("{error:?}"))
}
fn summary_item(text: &str) -> serde_json::Value {
    serde_json::json!({"type":"message","role":"user","content":[{"type":"input_text","text":text}]})
}
fn estimate(
    body: &SourceJson,
    profile: PromptProfile,
) -> Result<admission::PromptAdmission, String> {
    admission::estimate(body, profile).map_err(|error| format!("{error:?}"))
}

#[test]
fn calibrated_estimator_preserves_all_nine_frozen_source_goldens() -> TestResult {
    let fixture = source(include_str!(
        "../../gateway-compaction-test-support/tests/fixtures/source-v1.json"
    ))?;
    let cases = json::member(&fixture, "tokens")
        .and_then(|value| json::array(&value))
        .ok_or("missing frozen tokens")?;
    assert_eq!(cases.len(), 9);
    for case in cases {
        let text = json::member(&case, "text")
            .and_then(|value| json::string(&value))
            .ok_or("missing source text")?;
        let expected = json::member(&case, "expected")
            .ok_or("missing expected count")?
            .deserialize::<usize>()
            .map_err(|error| format!("{error:?}"))?;
        assert_eq!(estimate_tokens(text.as_units()), expected);
    }
    Ok(())
}

#[test]
fn exact_summary_is_a_single_data_envelope_and_does_not_change_input() -> TestResult {
    assert_eq!(summary::PREFIX.len(), summary::PREFIX_BYTES);
    let text = format!("{}\n{}", summary::PREFIX, "s".repeat(70 * 1024));
    let input = generated(
        &serde_json::json!([{"role":"user","content":[{"type":"input_text","text":"retain earlier steer"}]},summary_item(&text)]),
    )?;
    let before = input.as_str().to_owned();
    assert_eq!(summary::inspect_input(&input), Ok(Some(1)));
    assert_eq!(input.as_str(), before);
    let record = summary::exemption("input[1].content", text.len());
    let serialized = serde_json::to_value(record).map_err(|error| format!("{error:?}"))?;
    assert_eq!(serialized["kind"], "compaction_summary");
    assert_eq!(serialized["prefixSha256"], summary::PREFIX_SHA256);
    assert_eq!(serialized["contentParts"], 1);
    assert_eq!(serialized["perMessageLimit"], 65536);
    Ok(())
}

#[test]
fn malformed_and_multiple_summary_envelopes_fail_at_the_source_path() -> TestResult {
    let text = format!("{}\nx", summary::PREFIX);
    let valid = summary_item(&text);
    let malformed = [
        serde_json::json!({"role":"assistant","content":[{"type":"input_text","text":text}]}),
        serde_json::json!({"type":"agent_message","role":"user","content":[{"type":"input_text","text":text}]}),
        serde_json::json!({"content":[{"type":"input_text","text":text}]}),
        serde_json::json!({"role":"user","content":text}),
        serde_json::json!({"role":"user","content":[{"type":"input_text","text":text},{"type":"input_image","image_url":"data:AA"}]}),
        summary_item(&format!("{} without newline", summary::PREFIX)),
        summary_item(&format!("{}\n", summary::PREFIX)),
    ];
    for item in malformed {
        let result = summary::inspect_input(&generated(&vec![item])?);
        let error = result.err().ok_or("malformed envelope admitted")?;
        assert_eq!(error.path, "input[0]");
        assert!(
            error
                .message
                .ends_with("The summary was not reduced or forwarded.")
        );
    }
    let duplicate = summary::inspect_input(&generated(&vec![valid.clone(), valid])?)
        .err()
        .ok_or("second envelope admitted")?;
    assert_eq!(duplicate.path, "input[1]");
    assert!(
        duplicate
            .message
            .starts_with("adds a second summary envelope;")
    );
    for (input, path) in [
        (generated(&text)?, "input"),
        (generated(&vec![text])?, "input[0]"),
    ] {
        assert_eq!(
            summary::inspect_input(&input)
                .err()
                .ok_or("string summary admitted")?
                .path,
            path
        );
    }
    assert_eq!(
        summary::inspect_input(&source(
            r#"[{"role":"user","content":[{"type":"input_text","text":"ordinary"}]}]"#
        )?),
        Ok(None)
    );
    Ok(())
}

#[test]
fn generic_reduction_counts_text_encoder_bytes_and_preserves_utf16_refusals() -> TestResult {
    let value = JsString16::from_utf8(&"🧩a".repeat(18000));
    assert_eq!(payload::byte_len(&value), 90000);
    let bounded = payload::bound(
        &value,
        "input[0].content",
        None,
        PayloadKind::Message,
        false,
        PER_MESSAGE_LIMIT,
    )
    .map_err(|error| format!("{error:?}"))?;
    let record = bounded.elision.ok_or("missing reduction record")?;
    assert!(record.forwarded_bytes <= PER_MESSAGE_LIMIT);
    assert_eq!(record.original_bytes, 90000);
    let text = bounded
        .content
        .try_utf8()
        .map_err(|error| format!("{error:?}"))?;
    assert!(text.contains("deepseek-forwarded-payload/v2 elided"));
    assert!(text.starts_with("🧩a"));
    let call_id = JsString16::from_units(vec![0xd800]);
    let refused = payload::bound(
        &value,
        "input[2].output",
        Some(&call_id),
        PayloadKind::ToolOutput,
        true,
        PER_MESSAGE_LIMIT,
    )
    .err()
    .ok_or("disabled truncation reduced")?;
    assert_eq!(refused.path, "input[2].output");
    assert!(refused.message.as_units().contains(&0xd800));
    assert_eq!(
        payload::byte_len(&JsString16::from_units(vec![0xd800, 65, 0xdfff])),
        7
    );
    Ok(())
}

#[test]
fn argument_elision_is_json_with_exact_full_omission_and_small_limit_refusal() -> TestResult {
    let arguments = JsString16::from_utf8(&"x".repeat(PER_MESSAGE_LIMIT + 1024));
    let call = JsString16::from_utf8("call_args");
    let reduced = payload::bound_tool_arguments(
        &arguments,
        "input[0].arguments",
        &call,
        false,
        PER_MESSAGE_LIMIT,
    )
    .map_err(|error| format!("{error:?}"))?;
    let decoded: serde_json::Value = serde_json::from_str(
        &reduced
            .content
            .try_utf8()
            .map_err(|error| format!("{error:?}"))?,
    )
    .map_err(|error| format!("{error:?}"))?;
    let envelope = &decoded["gateway_elision"];
    assert_eq!(envelope["policy"], "deepseek-forwarded-payload/v2");
    assert_eq!(envelope["kind"], "tool_arguments");
    assert_eq!(envelope["originalBytes"], 66560);
    assert_eq!(envelope["omittedBytes"], 66560);
    assert!(
        payload::bound_tool_arguments(
            &arguments,
            "input[0].arguments",
            &call,
            true,
            PER_MESSAGE_LIMIT
        )
        .is_err()
    );
    assert!(payload::reduce_tool_arguments(&arguments, "input[0].arguments", &call, 1).is_none());
    let image = payload::omitted_image(&arguments, "input[0].content[0]");
    assert_eq!(
        image.elision.ok_or("image record absent")?.omitted_bytes,
        66560
    );
    assert!(
        image
            .content
            .try_utf8()
            .map_err(|error| format!("{error:?}"))?
            .contains("image omitted (66560 bytes)")
    );
    Ok(())
}

#[test]
fn aggregate_admission_retains_all_steer_history_and_distinct_provider_windows() -> TestResult {
    let summary = format!("{}\nsummary", summary::PREFIX);
    let body = generated(
        &serde_json::json!({"messages":[{"role":"user","content":"!".repeat(1100000)},{"role":"user","content":summary},{"role":"user","content":"latest steer"}],"tools":[{"type":"function","function":{"name":"run","parameters":{"type":"object"}}}]}),
    )?;
    let before = body.as_str().to_owned();
    let deepseek = estimate(&body, PromptProfile::Deepseek)?;
    let lithos = estimate(&body, PromptProfile::Lithos)?;
    let cerebras = estimate(&body, PromptProfile::Cerebras)?;
    assert!(!deepseek.admitted);
    assert!(lithos.admitted);
    assert!(!cerebras.admitted);
    assert_eq!(deepseek.structural_overhead_tokens, 336);
    assert_eq!(deepseek.output_reserve_tokens, 16384);
    assert_eq!(deepseek.prompt_budget_tokens, 983616);
    assert_eq!(lithos.prompt_budget_tokens, 1032192);
    assert_eq!(cerebras.prompt_budget_tokens, 114688);
    assert_eq!(body.as_str(), before);
    assert!(
        admission::failure_message(&deepseek, "DeepSeek")
            .contains("chat-prompt-admission/v3. Reduce the prompt; no payload was dispatched.")
    );
    Ok(())
}

#[test]
fn safe_integer_output_reserve_and_canonical_json_match_source_semantics() -> TestResult {
    let body = source(
        r#"{"messages":[],"tools":[],"max_tokens":20000,"dup":7,"dup":8.0,"text":"\ud800","zero":-0}"#,
    )?;
    let expected =
        r#"{"messages":[],"tools":[],"max_tokens":20000,"dup":8,"text":"\ud800","zero":0}"#;
    let observed = estimate(&body, PromptProfile::Deepseek)?;
    assert_eq!(observed.serialized_body_bytes, expected.len() as u64);
    assert_eq!(observed.structural_overhead_tokens, 256);
    assert_eq!(observed.requested_output_tokens, Some(20000));
    assert_eq!(observed.output_reserve_tokens, 20000);
    assert_eq!(observed.prompt_budget_tokens, 980000);
    for value in ["0", "-1", "1.5", "9007199254740992", "null", "\"20000\""] {
        let input = source(&format!("{{\"max_tokens\":{value}}}"))?;
        assert_eq!(
            estimate(&input, PromptProfile::Lithos)?.requested_output_tokens,
            None
        );
    }
    let maximum = estimate(
        &source(r#"{"max_tokens":9007199254740991}"#)?,
        PromptProfile::Cerebras,
    )?;
    assert_eq!(maximum.output_reserve_tokens, 9007199254740991);
    assert_eq!(maximum.prompt_budget_tokens, 0);
    assert!(!maximum.admitted);
    Ok(())
}

#[test]
fn actual_measurement_projection_and_its_error_fallback_remain_caller_owned() -> TestResult {
    let body = generated(
        &serde_json::json!({"messages":[{"role":"assistant","content":"answer","reasoning_content":"!".repeat(200000)}]}),
    )?;
    assert!(!estimate(&body, PromptProfile::Cerebras)?.admitted);
    let projected = admission::estimate_projected(&body, PromptProfile::Cerebras, |_| {
        SourceJson::parse(r#"{"messages":[{"role":"assistant","content":"answer"}]}"#)
    })
    .map_err(|error| format!("{error:?}"))?;
    assert!(projected.admitted);
    let failed = admission::estimate_projected(&body, PromptProfile::Cerebras, |_| {
        Err(gateway_source::error::GatewayError::unavailable(
            "projection failed",
            "fixture",
        ))
    })
    .map_err(|error| format!("{error:?}"))?;
    assert_eq!(failed, estimate(&body, PromptProfile::Cerebras)?);
    Ok(())
}
