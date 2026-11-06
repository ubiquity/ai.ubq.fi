//! Ports tests/codex-responses-error.test.ts (129d8f4) and the encoder and selection vectors of
//! tests/codex-context-admission.test.ts (19d9eec), plus the policy constants they rely on.
use gateway_core::{error::GatewayError, source_json::SourceJson};
use gateway_prompt_admission::{
    admission::{
        AdmissionProfile, PromptAdmission, aggregate_context_overflow, estimate_prompt_admission,
        failure_message, is_aggregate_context_overflow, missing_window,
    },
    codex_context::CodexContextCompat,
    forwarding::oversized,
    native_failure::{
        encode_native_context_failure, is_codex_native_turn, native_context_failure,
        selects_native_encoding,
    },
    summary::{SUMMARY_PREFIX, SUMMARY_PREFIX_BYTES, SUMMARY_PREFIX_SHA256},
};
use gateway_schema::source_request::error::SourceRequestError;
use serde_json::{Value, json};
use sha2::{Digest, Sha256};

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

fn diagnostic(error: SourceRequestError) -> GatewayError {
    error.diagnostic().clone()
}
fn overflow() -> TestResult<GatewayError> {
    let admission =
        estimate_prompt_admission(&SourceJson::parse(r#"{"messages":[]}"#).must()?, 1).must()?;
    Ok(diagnostic(aggregate_context_overflow(
        &admission, "DeepSeek",
    )))
}

#[test]
fn summary_prefix_identity_is_exact() -> TestResult {
    assert_eq!(SUMMARY_PREFIX.len(), SUMMARY_PREFIX_BYTES);
    let digest = Sha256::digest(SUMMARY_PREFIX.as_bytes());
    let hex = digest
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect::<String>();
    assert_eq!(hex, SUMMARY_PREFIX_SHA256);
    Ok(())
}

#[test]
fn provider_windows_stay_distinct_and_verified() -> TestResult {
    let windows = [
        AdmissionProfile::Deepseek,
        AdmissionProfile::Lithos,
        AdmissionProfile::Cerebras,
    ]
    .map(AdmissionProfile::window_tokens);
    assert_eq!(windows, [Some(1_000_000), Some(1_048_576), Some(131_072)]);
    Ok(())
}

#[test]
fn unprojected_reasoning_counts_against_the_cerebras_window() -> TestResult {
    let reasoning = "r".repeat(60 * 1024 * 20);
    let body = json!({"model":"gpt-oss-120b","messages":[{"role":"assistant","content":"answer","reasoning_content":reasoning}],"stream":false});
    let body = SourceJson::from_serializable(&body).must()?;
    let window = AdmissionProfile::Cerebras.window_tokens().must()?;
    let admission = estimate_prompt_admission(&body, window).must()?;
    assert!(!admission.admitted);
    assert_eq!(admission.structural_overhead_tokens, 256 + 16);
    assert_eq!(admission.prompt_budget_tokens, 131_072 - 16_384);
    Ok(())
}

#[test]
fn only_the_gateway_built_aggregate_overflow_is_the_typed_failure() -> TestResult {
    let error = overflow()?;
    assert!(is_aggregate_context_overflow(&error));
    assert_eq!(error.status, 400);
    let body: Value = aggregate_context_overflow(
        &estimate_prompt_admission(&SourceJson::parse("{}").must()?, 1).must()?,
        "Cerebras",
    )
    .body()
    .deserialize()
    .must()?;
    assert_eq!(body["error"]["code"], "context_length_exceeded");
    assert_eq!(body["error"]["param"], "input");
    assert_eq!(body["error"]["type"], "invalid_request_error");
    assert!(
        body["error"]["message"]
            .as_str()
            .must()?
            .starts_with("the complete projected Cerebras prompt")
    );
    for other in [
        diagnostic(oversized("input", 65_537, None)),
        diagnostic(missing_window("Claude")),
        GatewayError::new(
            400,
            "Other context refusal",
            Some("context_length_exceeded"),
        )
        .with_param(Some("input")),
        GatewayError::new(400, "Invalid input", Some("invalid_request_error")),
    ] {
        assert!(!is_aggregate_context_overflow(&other), "{other:?}");
    }
    Ok(())
}

#[test]
fn failure_message_names_every_budget_term() -> TestResult {
    let admission = PromptAdmission {
        policy: "chat-prompt-admission/v3",
        estimated_prompt_tokens: 1_032_954,
        serialized_body_bytes: 3_692_782,
        structural_overhead_tokens: 1_024,
        requested_output_tokens: None,
        output_reserve_tokens: 16_384,
        context_window_tokens: 1_048_576,
        prompt_budget_tokens: 1_032_192,
        admitted: false,
    };
    assert_eq!(
        failure_message(&admission, "LithosAI"),
        "the complete projected LithosAI prompt is an estimated 1032954 tokens (the calibrated estimator over 3692782 serialized UTF-8 bytes plus 1024 structural tokens), above the 1032192-token budget from a 1048576-token window minus a 16384-token output reserve under chat-prompt-admission/v3. Reduce the prompt; no payload was dispatched."
    );
    Ok(())
}

const ERROR: [(&str, &str); 3] = [
    ("code", "context_length_exceeded"),
    ("message", "ISOLATED_FIXTURE_CONTEXT_OVERFLOW"),
    ("param", "input"),
];
fn error_fields(code: &str) -> serde_json::Map<String, Value> {
    ERROR
        .iter()
        .map(|(key, value)| {
            let value = if *key == "code" { code } else { value };
            ((*key).to_owned(), json!(value))
        })
        .collect()
}

#[test]
fn codex_context_error_becomes_one_failed_response() -> TestResult {
    for nested in [false, true] {
        for partial in [false, true] {
            let template = json!({"id":"resp_context","object":"response","status":"in_progress","model":"fixture","output":[]});
            let mut compat = CodexContextCompat::default();
            let mut visible = vec![];
            let mut initial =
                vec![json!({"type":"response.created","sequence_number":0,"response":template})];
            if partial {
                initial.push(json!({"type":"response.output_text.delta","sequence_number":1,"item_id":"msg_context","delta":"preserved partial"}));
            }
            for event in &initial {
                assert_eq!(compat.visible(event), None);
                visible.push(event.clone());
            }
            let mut fields = error_fields("context_length_exceeded");
            let original = if nested {
                fields.insert("type".into(), json!("invalid_request_error"));
                json!({"type":"error","sequence_number":2,"error":fields})
            } else {
                let mut event = fields.clone();
                event.insert("type".into(), json!("error"));
                event.insert("sequence_number".into(), json!(2));
                Value::Object(event)
            };
            let failed = compat.visible(&original).must()?;
            visible.push(failed.clone());
            assert_eq!(failed["type"], "response.failed");
            assert_eq!(failed["sequence_number"], 2);
            assert_eq!(
                visible
                    .iter()
                    .filter(|e| e["type"] == "response.failed")
                    .count(),
                1
            );
            assert!(
                !visible
                    .iter()
                    .any(|e| e["type"] == "error" || e["type"] == "response.completed")
            );
            let response = &failed["response"];
            assert_eq!(response["id"], "resp_context");
            assert_eq!(response["model"], "fixture");
            assert_eq!(response["object"], "response");
            assert_eq!(response["status"], "failed");
            assert_eq!(response["error"], Value::Object(fields));
            assert_eq!(
                response["output"].to_string().contains("preserved partial"),
                partial,
                "nested={nested} partial={partial}"
            );
            if partial {
                assert_eq!(
                    response["output"],
                    json!([{"id":"msg_context","type":"message","status":"incomplete","role":"assistant","content":[{"type":"output_text","text":"preserved partial","annotations":[]}]}])
                );
            }
        }
    }
    Ok(())
}

#[test]
fn unchanged_terminal_without_identity_or_exact_context_code() -> TestResult {
    let mut fields = error_fields("context_length_exceeded");
    fields.insert("type".into(), json!("error"));
    fields.insert("sequence_number".into(), json!(0));
    let original = Value::Object(fields);
    assert_eq!(CodexContextCompat::default().visible(&original), None);
    let created = json!({"type":"response.created","response":{"id":"resp_context"}});
    let mut tracked = CodexContextCompat::default();
    assert_eq!(tracked.visible(&created), None);
    let mut other = original.clone();
    other["code"] = json!("provider_stream_error");
    assert_eq!(tracked.visible(&other), None);
    assert!(tracked.visible(&original).is_some());
    Ok(())
}

#[test]
fn observed_output_items_and_hosted_tools_replace_by_id() -> TestResult {
    let mut compat = CodexContextCompat::default();
    for event in [
        json!({"type":"response.created","response":{"id":"resp_x","output":[{"id":"keep","type":"message"}]}}),
        json!({"type":"response.output_item.added","item":{"id":"call_a","type":"function_call","status":"in_progress"}}),
        json!({"type":"response.output_item.done","item":{"id":"call_a","type":"function_call","status":"completed"}}),
        json!({"type":"response.web_search_call.in_progress","item_id":"ws_1"}),
        json!({"type":"response.web_search_call.completed","item_id":"ws_1"}),
        json!({"type":"response.refusal.done","item_id":"msg_r","refusal":"no"}),
    ] {
        compat.observe(&event);
    }
    let failed = compat
        .failure_event(&json!({"type":"error","error":{"code":"context_length_exceeded"}}))
        .must()?;
    assert_eq!(
        failed["response"]["output"],
        json!([
            {"id":"keep","type":"message"},
            {"id":"call_a","type":"function_call","status":"completed"},
            {"id":"ws_1","type":"web_search_call","status":"completed"},
            {"id":"msg_r","type":"message","status":"completed","role":"assistant","content":[{"type":"refusal","refusal":"no"}]}
        ])
    );
    assert!(failed.get("sequence_number").is_none());
    Ok(())
}

fn encoded_events(body: &str) -> TestResult<Vec<Value>> {
    body.lines()
        .filter_map(|line| line.strip_prefix("data: "))
        .map(serde_json::from_str)
        .collect::<Result<_, _>>()
        .map_err(Into::into)
}

const METADATA: &str = r#"{"session_id":"session","thread_id":"thread","turn_id":"turn"}"#;

#[test]
fn aggregate_failure_emits_created_then_one_failed_terminal_with_missing_usage() -> TestResult {
    let error = overflow()?;
    let body = encode_native_context_failure("resp_fixture", 1_800_000_000, &error);
    assert!(body.starts_with("event: response.created\ndata: "));
    let events = encoded_events(&body)?;
    let kinds = events.iter().map(|e| e["type"].clone()).collect::<Vec<_>>();
    assert_eq!(kinds, [json!("response.created"), json!("response.failed")]);
    let (created, failed) = (&events[0]["response"], &events[1]["response"]);
    assert_eq!(created["id"], failed["id"]);
    assert_eq!(created["status"], "in_progress");
    assert_eq!(failed["status"], "failed");
    assert_eq!(
        failed["error"],
        json!({"code":"context_length_exceeded","message":error.envelope.error.message,"param":"input"})
    );
    assert_eq!(failed["output"], json!([]));
    assert_eq!(failed["usage"], Value::Null);
    assert_eq!(
        (
            events[0]["sequence_number"].clone(),
            events[1]["sequence_number"].clone()
        ),
        (json!(0), json!(1))
    );
    Ok(())
}

#[test]
fn malformed_or_missing_native_identity_retains_http_400() -> TestResult {
    assert!(is_codex_native_turn(Some(METADATA)));
    for header in [
        None,
        Some("null"),
        Some("not-json"),
        Some("[]"),
        Some("{}"),
        Some(r#"{"session_id":" ","thread_id":"thread","turn_id":"turn"}"#),
        Some(r#"{"session_id":"session","thread_id":1,"turn_id":"turn"}"#),
        Some(r#"{"session_id":"session","thread_id":"thread","turn_id":""}"#),
    ] {
        assert!(!is_codex_native_turn(header), "{header:?}");
        assert!(
            native_context_failure(
                Some(&SourceJson::parse("true").must()?),
                header,
                &overflow()?,
                "resp_x",
                0
            )
            .is_none()
        );
    }
    Ok(())
}

#[test]
fn plain_nonstream_and_other_projection_failures_retain_http_400() -> TestResult {
    let stream = SourceJson::parse("true").must()?;
    assert!(selects_native_encoding(
        Some(&stream),
        Some(METADATA),
        &overflow()?
    ));
    assert!(!selects_native_encoding(Some(&stream), None, &overflow()?));
    for value in ["false", r#""true""#] {
        let value = SourceJson::parse(value).must()?;
        assert!(!selects_native_encoding(
            Some(&value),
            Some(METADATA),
            &overflow()?
        ));
    }
    assert!(!selects_native_encoding(None, Some(METADATA), &overflow()?));
    for error in [
        GatewayError::new(
            400,
            "Other context refusal",
            Some("context_length_exceeded"),
        )
        .with_param(Some("input")),
        GatewayError::new(400, "Invalid input", Some("invalid_request_error"))
            .with_param(Some("input")),
        diagnostic(oversized("input[0].content", 65_537, None)),
    ] {
        assert!(!selects_native_encoding(
            Some(&stream),
            Some(METADATA),
            &error
        ));
    }
    Ok(())
}
