//! Worker answer for a projection refusal (19d9eec, tests/codex-context-admission.test.ts): a
//! streamed native Codex turn's aggregate overflow is 200 SSE `response.created` then
//! `response.failed`; every other refusal keeps its HTTP 400 source body.
use gateway_core::{error::GatewayError, source_json::SourceJson};
use gateway_prompt_admission::admission::{aggregate_context_overflow, estimate_prompt_admission};
use gateway_routing_runtime::types::RoutingError;
use gateway_schema::source_request::error::SourceRequestError;
use gateway_worker::inference_source::{NativeTurn, Reply, reply};
use serde_json::{Value, json};

type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;
const METADATA: &str = r#"{"session_id":"session","thread_id":"thread","turn_id":"turn"}"#;

fn overflow() -> TestResult<RoutingError> {
    let raw = SourceJson::parse(r#"{"messages":[]}"#).map_err(|error| format!("{error:?}"))?;
    let admission = estimate_prompt_admission(&raw, 1).map_err(|error| format!("{error:?}"))?;
    Ok(aggregate_context_overflow(&admission, "DeepSeek").into())
}
fn turn(stream: Option<&str>, metadata: Option<&str>) -> TestResult<NativeTurn> {
    Ok(NativeTurn {
        stream: stream
            .map(SourceJson::parse)
            .transpose()
            .map_err(|error| format!("{error:?}"))?,
        turn_metadata: metadata.map(str::to_owned),
        early: None,
    })
}
fn answer(error: &RoutingError, turn: &NativeTurn, minted: &mut bool) -> TestResult<Reply> {
    reply(error, turn, || {
        *minted = true;
        Ok(("resp_fixture".into(), 1_800_000_000))
    })
    .map_err(|error| error.envelope.error.message.clone())?
    .ok_or_else(|| "a source refusal always has a reply".into())
}
fn events(body: &[u8]) -> TestResult<Vec<Value>> {
    std::str::from_utf8(body)?
        .lines()
        .filter_map(|line| line.strip_prefix("data: "))
        .map(|line| serde_json::from_str(line).map_err(Into::into))
        .collect()
}

#[test]
fn native_streamed_aggregate_overflow_is_one_failed_sse_terminal() -> TestResult {
    let error = overflow()?;
    let mut minted = false;
    let reply = answer(&error, &turn(Some("true"), Some(METADATA))?, &mut minted)?;
    assert!(minted && reply.native_context_failure);
    assert_eq!(
        (reply.status, reply.content_type),
        (200, "text/event-stream")
    );
    let events = events(&reply.body)?;
    let kinds = events.iter().map(|e| e["type"].clone()).collect::<Vec<_>>();
    assert_eq!(kinds, [json!("response.created"), json!("response.failed")]);
    let failed = &events[1]["response"];
    assert_eq!(events[0]["response"]["id"], "resp_fixture");
    assert_eq!(failed["id"], "resp_fixture");
    assert_eq!(failed["status"], "failed");
    assert_eq!(failed["error"]["code"], "context_length_exceeded");
    assert_eq!(failed["error"]["param"], "input");
    assert_eq!(
        failed["error"]["message"],
        error.error.envelope.error.message
    );
    assert_eq!(
        (failed["output"].clone(), failed["usage"].clone()),
        (json!([]), Value::Null)
    );
    Ok(())
}

#[test]
fn plain_nonstream_and_other_refusals_keep_http_400() -> TestResult {
    let other: RoutingError = SourceRequestError::from_gateway(
        GatewayError::new(
            400,
            "Other context refusal",
            Some("context_length_exceeded"),
        )
        .with_param(Some("input")),
    )
    .into();
    let cases = [
        (overflow()?, turn(Some("true"), None)?),
        (overflow()?, turn(Some("true"), Some("{}"))?),
        (overflow()?, turn(Some("false"), Some(METADATA))?),
        (overflow()?, turn(Some(r#""true""#), Some(METADATA))?),
        (overflow()?, turn(None, Some(METADATA))?),
        (other, turn(Some("true"), Some(METADATA))?),
    ];
    for (error, turn) in &cases {
        let mut minted = false;
        let reply = answer(error, turn, &mut minted)?;
        assert!(!minted && !reply.native_context_failure);
        assert_eq!(
            (reply.status, reply.content_type),
            (400, "application/json")
        );
        let body: Value = serde_json::from_slice(&reply.body)?;
        assert_eq!(body["error"]["code"], "context_length_exceeded");
    }
    Ok(())
}

#[test]
fn refusal_without_a_source_body_keeps_the_gateway_error_path() -> TestResult {
    let error: RoutingError = GatewayError::new(503, "unavailable", Some("x")).into();
    let mut minted = false;
    let result = reply(&error, &turn(Some("true"), Some(METADATA))?, || {
        minted = true;
        Ok((String::new(), 0))
    });
    assert!(matches!(result, Ok(None)) && !minted);
    Ok(())
}
