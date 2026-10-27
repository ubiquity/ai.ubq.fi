use gateway_claude_usage_protocol::state::UsageState;
use gateway_claude_usage_runtime::{
    admin,
    collect::Refresh,
    fetch::{Failure, ResultData},
};
use gateway_core::{
    js_string::JsString16, source_json::SourceJson, source_serialization as source,
};
use serde::Deserialize;
use std::collections::BTreeMap;

type TestResult = Result<(), Box<dyn std::error::Error>>;
#[derive(Deserialize)]
struct Expected {
    status: u16,
    headers: Vec<(String, String)>,
    body: String,
    block_reads: usize,
}
#[derive(Deserialize)]
struct Case {
    kind: String,
    retry: Option<String>,
    refresh: SourceJson,
    expected: Expected,
}
#[derive(Deserialize)]
struct Oracle {
    source_sha: String,
    cases: Vec<Case>,
}
fn result(case: &Case) -> Result<Refresh, Box<dyn std::error::Error>> {
    let get = |value: &SourceJson, name| source::member(value, name).ok_or("source member absent");
    if case.kind == "changed" {
        return Ok(Refresh::Changed);
    }
    if case.kind == "busy" {
        return Ok(Refresh::Busy);
    }
    let usage: UsageState = get(&case.refresh, "usage")?.deserialize()?;
    if case.kind == "fresh" {
        return Ok(Refresh::Fresh(usage));
    }
    if case.kind == "success" {
        return Ok(Refresh::Completed {
            result: ResultData::Success {
                status: 200,
                observed_at_ms: usage.attempted_at_ms,
                observation: gateway_claude_usage_quota::QuotaObservation::empty(
                    usage.attempted_at_ms,
                ),
            },
            usage,
        });
    }
    let raw = get(&case.refresh, "result")?;
    let message: Option<JsString16> = get(&raw, "message")?.deserialize()?;
    let kind = match case.kind.as_str() {
        "scope_missing" => "scope_missing",
        "unsupported_credential" => "unsupported_credential",
        "auth_invalid" => "auth_invalid",
        "rate_limited" => "rate_limited",
        "upstream_error" => "upstream_error",
        "http_error" => "http_error",
        "unreachable" => "unreachable",
        "invalid_response" => "invalid_response",
        _ => return Err("unrecognized source failure".into()),
    };
    let retry_after_ms = case
        .retry
        .as_ref()
        .map(|value| value.parse::<f64>())
        .transpose()?;
    Ok(Refresh::Completed {
        result: ResultData::Failure(Failure {
            kind,
            status: usage.status,
            observed_at_ms: usage.attempted_at_ms,
            message,
            retry_after_ms,
        }),
        usage,
    })
}
#[test]
fn exact_frozen_admin_body_control_headers_utf16_and_extreme_retry_spelling_match() -> TestResult {
    let oracle: Oracle = serde_json::from_str(include_str!("fixtures/admin-oracle.json"))?;
    assert_eq!(
        oracle.source_sha,
        "b59243d674567d418d1410f8390fc7fe47a08c92"
    );
    assert_eq!(oracle.cases.len(), 16);
    for case in oracle.cases {
        let data = SourceJson::parse(r#"{"accepted":"source","label":"\ud800"}"#)?;
        let response = admin::response(result(&case)?, data)
            .unwrap_or_else(gateway_auth::types::AuthResponse::error);
        assert_eq!(response.status, case.expected.status, "{}", case.kind);
        let headers: BTreeMap<_, _> = response
            .headers
            .into_iter()
            .map(|(key, value)| (key.to_ascii_lowercase(), value))
            .collect();
        assert_eq!(
            headers,
            case.expected.headers.into_iter().collect(),
            "{}",
            case.kind
        );
        assert_eq!(
            response.body.to_bytes()?,
            case.expected.body.as_bytes(),
            "{}",
            case.kind
        );
        assert_eq!(
            case.expected.block_reads,
            usize::from(case.kind != "changed")
        );
    }
    Ok(())
}
