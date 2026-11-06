//! Port of tests/responses-native-headers.test.ts (e9b1b90): after admitted native SSE headers, a
//! selected late HTTP failure is `response.created` then `response.failed` with only its original
//! code and message, while status, safe headers and the bounded original error stay internal.
use gateway_prompt_admission::native_http_failure::{
    LateFailure, MAX_HEADER_VALUE_UNITS, SelectedAnswer, is_event_stream, late_failure, reencodes,
    safe_headers,
};
use serde_json::{Value, json};
use std::collections::BTreeMap;

type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;
/// Source `SENTINEL_UPSTREAM_SAFE_RESPONSE_HEADER_NAMES`.
const SAFE: &[&str] = &[
    "cache-control",
    "content-length",
    "content-type",
    "openai-processing-ms",
    "retry-after",
    "x-ratelimit-limit-requests",
    "x-ratelimit-limit-tokens",
    "x-ratelimit-remaining-requests",
    "x-ratelimit-remaining-tokens",
    "x-ratelimit-reset-requests",
    "x-ratelimit-reset-tokens",
];

fn headers(entries: &[(&str, &str)]) -> BTreeMap<String, String> {
    entries
        .iter()
        .map(|(name, value)| ((*name).to_owned(), (*value).to_owned()))
        .collect()
}
fn original(code: &str) -> String {
    json!({"error":{"code":code,"message":format!("original {code}"),"param":"input","type":"original_type"}}).to_string()
}
fn encode(
    status: u16,
    entries: &[(&str, &str)],
    body: &[u8],
    complete: bool,
) -> TestResult<LateFailure> {
    let headers = headers(entries);
    let answer = SelectedAnswer {
        status,
        headers: &headers,
        body,
        complete,
    };
    late_failure(&answer, SAFE, "resp_fixture", 1_800_000_000)
        .map_err(|error| error.envelope.error.message.into())
}
fn events(body: &str) -> TestResult<Vec<Value>> {
    let mut events = Vec::new();
    for frame in body.split_terminator("\n\n") {
        let (event, data) = frame
            .split_once('\n')
            .ok_or("every frame has an event and a data line")?;
        let data: Value = serde_json::from_str(data.strip_prefix("data: ").ok_or("data line")?)?;
        assert_eq!(event.strip_prefix("event: "), data["type"].as_str());
        events.push(data);
    }
    Ok(events)
}
fn evidence(failure: &LateFailure) -> TestResult<Value> {
    Ok(serde_json::from_str(&serde_json::to_string(
        &failure.evidence,
    )?)?)
}

#[test]
fn selected_native_http_failures_emit_only_truthful_standard_failure_events() -> TestResult {
    for (status, code) in [
        (401, "authentication_error"),
        (429, "rate_limit_exceeded"),
        (503, "server_error"),
    ] {
        let entries = [("content-type", "application/json"), ("retry-after", "17")];
        let failure = encode(status, &entries, original(code).as_bytes(), true)?;
        let events = events(&failure.body)?;
        let kinds = events.iter().map(|e| e["type"].clone()).collect::<Vec<_>>();
        assert_eq!(kinds, [json!("response.created"), json!("response.failed")]);
        assert_eq!(events[0]["sequence_number"], 0);
        assert_eq!(events[1]["sequence_number"], 1);
        assert_eq!(events[0]["response"]["id"], events[1]["response"]["id"]);
        assert_eq!(events[0]["response"]["status"], "in_progress");
        assert_eq!(events[0]["response"]["error"], Value::Null);
        let failed = &events[1]["response"];
        assert_eq!(failed["status"], "failed");
        assert_eq!(
            failed["error"],
            json!({"code":code,"message":format!("original {code}")})
        );
        assert_eq!(failed["usage"], Value::Null);
        assert_eq!(failed["output"], json!([]));
        assert_eq!(failure.code, code);
        let evidence = evidence(&failure)?;
        assert_eq!(evidence["status"], status);
        assert_eq!(evidence["headers"]["retry-after"], "17");
        assert_eq!(
            evidence["error"],
            json!({"code":code,"message":format!("original {code}"),"param":"input","type":"original_type"})
        );
        assert_eq!(evidence["body_complete"], true);
    }
    Ok(())
}

#[test]
fn bounded_native_error_evidence_keeps_the_source_shape_without_private_headers() -> TestResult {
    let entries = [
        ("content-type", "application/json"),
        ("retry-after", "17"),
        ("set-cookie", "private-cookie"),
        ("authorization", "private-auth"),
    ];
    let failure = encode(503, &entries, original("server_error").as_bytes(), true)?;
    assert_eq!(
        serde_json::to_string(&failure.evidence)?,
        r#"{"status":503,"headers":{"content-type":"application/json","retry-after":"17"},"headers_truncated":false,"error":{"code":"server_error","message":"original server_error","param":"input","type":"original_type"},"body_complete":true}"#
    );
    let all = format!(
        "{}{}",
        failure.body,
        serde_json::to_string(&failure.evidence)?
    );
    assert!(!all.contains("private-cookie") && !all.contains("private-auth"));
    Ok(())
}

#[test]
fn oversized_selected_error_body_is_never_presented_as_complete_original_evidence() -> TestResult {
    let partial = br#"{"error":{"code":"not_complete","message":"xxxx"#;
    let failure = encode(502, &[("content-type", "application/json")], partial, false)?;
    let evidence = evidence(&failure)?;
    assert_eq!(evidence["status"], 502);
    assert_eq!(evidence["body_complete"], false);
    assert_eq!(evidence["error"], Value::Null);
    assert!(!failure.body.contains("not_complete"));
    let events = events(&failure.body)?;
    assert_eq!(
        events[1]["response"]["error"],
        json!({"code":"server_error","message":"Upstream request failed with HTTP 502."})
    );
    assert_eq!(failure.code, "server_error");
    Ok(())
}

#[test]
fn non_json_and_empty_bodies_use_the_source_message_fallbacks() -> TestResult {
    let text = encode(500, &[], b"\xEF\xBB\xBFupstream exploded", true)?;
    assert_eq!(
        events(&text.body)?[1]["response"]["error"],
        json!({"code":"server_error","message":"upstream exploded"})
    );
    assert_eq!(
        evidence(&text)?["error"],
        json!({"message":"upstream exploded"})
    );
    let empty = encode(504, &[], b"", true)?;
    assert_eq!(
        evidence(&empty)?["error"],
        json!({"message":"Upstream request failed with HTTP 504."})
    );
    // A non-record `error` keeps the whole text; a record without strings keeps the defaults.
    let scalar = encode(400, &[], br#"{"error":"flat"}"#, true)?;
    assert_eq!(
        evidence(&scalar)?["error"],
        json!({"message":r#"{"error":"flat"}"#})
    );
    let listed = encode(400, &[], br#"{"error":[1]}"#, true)?;
    assert_eq!(evidence(&listed)?["error"], json!([1]));
    assert_eq!(
        events(&listed.body)?[1]["response"]["error"],
        json!({"code":"server_error","message":"Upstream request failed with HTTP 400."})
    );
    Ok(())
}

#[test]
fn only_non_2xx_non_sse_answers_change_encoding() {
    assert!(reencodes(429, Some("application/json")));
    assert!(reencodes(503, None));
    assert!(!reencodes(200, Some("application/json")));
    assert!(!reencodes(503, Some("text/event-stream")));
    for sse in [
        "TEXT/Event-Stream",
        "text/event-stream; charset=utf-8",
        "text/event-stream ;x",
    ] {
        assert!(is_event_stream(Some(sse)), "{sse}");
    }
    for other in ["text/event-streams", " text/event-stream", "text/event", ""] {
        assert!(!is_event_stream(Some(other)), "{other}");
    }
}

#[test]
fn safe_header_values_keep_512_utf16_units_and_report_truncation() {
    let long = "a".repeat(MAX_HEADER_VALUE_UNITS + 1);
    let (kept, truncated) = safe_headers(&headers(&[("Retry-After", &long)]), SAFE);
    assert!(truncated);
    assert_eq!(kept["retry-after"].len(), MAX_HEADER_VALUE_UNITS);
    let exact = "b".repeat(MAX_HEADER_VALUE_UNITS);
    let (kept, truncated) = safe_headers(&headers(&[("retry-after", &exact)]), SAFE);
    assert!(!truncated);
    assert_eq!(kept["retry-after"], exact);
}
