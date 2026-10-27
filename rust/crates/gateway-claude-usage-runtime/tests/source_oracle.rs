use gateway_claude_usage_protocol::snapshot;
use gateway_claude_usage_runtime::fetch;
use gateway_source::{
    js_string::JsString16, source_json::SourceJson, source_serialization as source,
};
use serde::Deserialize;

type TestResult = Result<(), Box<dyn std::error::Error>>;
#[derive(Deserialize)]
struct Case {
    name: String,
    input: String,
    snapshot: SourceJson,
    observation: SourceJson,
}
#[derive(Deserialize)]
struct Oracle {
    source_sha: String,
    at: i64,
    dates: Vec<(JsString16, Option<i64>)>,
    cases: Vec<Case>,
}
#[test]
fn complete_source_payload_and_canonical_windows_match_frozen_b592() -> TestResult {
    let oracle: Oracle = serde_json::from_str(include_str!("fixtures/source-oracle.json"))?;
    assert_eq!(
        oracle.source_sha,
        "b59243d674567d418d1410f8390fc7fe47a08c92"
    );
    assert!(oracle.cases.len() >= 55);
    let dates = |value: &JsString16| {
        oracle
            .dates
            .iter()
            .find(|(key, _)| key == value)
            .and_then(|(_, v)| *v)
    };
    for case in &oracle.cases {
        let input = SourceJson::parse(&case.input)?;
        let snapshot = snapshot::parse(&input, oracle.at, &dates)
            .as_ref()
            .map(snapshot::serialize)
            .transpose()?
            .unwrap_or(SourceJson::parse("null")?);
        assert_eq!(
            snapshot,
            source::canonical(&case.snapshot)?,
            "snapshot {}",
            case.name
        );
        let observation =
            SourceJson::from_serializable(&fetch::observation(&input, oracle.at, &dates))?;
        assert_eq!(
            source::canonical(&observation)?,
            source::canonical(&case.observation)?,
            "observation {}",
            case.name
        );
    }
    Ok(())
}
#[test]
fn stored_meters_are_recomputed_from_provenance_and_success_replaces_shape() -> TestResult {
    let raw = SourceJson::parse(
        r#"{"v":1,"observed_at_ms":1000,"payload":{"limits":[]},"meters":[{"forged":true}]}"#,
    )?;
    let snapshot = snapshot::read(&raw, &|_| Some(4000)).ok_or("snapshot rejected")?;
    assert!(snapshot.meters.is_empty());
    for raw in [
        r#"{"v":2,"observed_at_ms":1000,"payload":{"limits":[]}}"#,
        r#"{"v":1,"observed_at_ms":0,"payload":{"limits":[]}}"#,
        r#"{"v":1,"observed_at_ms":1000,"payload":{}}"#,
        r#"{"v":1,"observed_at_ms":1000,"payload":{"limits":[{}]}}"#,
    ] {
        assert!(snapshot::read(&SourceJson::parse(raw)?, &|_| None).is_none());
    }
    Ok(())
}

#[test]
fn stored_meter_integer_numbers_accept_source_exponent_spelling_without_losing_utf16() -> TestResult
{
    let meter = SourceJson::parse(
        r#"{"id":"id\ud800","label":"label","kind":"kind","group":"group","scope":null,"severity":null,"is_active":false,"used_percent":-12,"resets_at":null,"reset_at_ms":1e3,"observed_at_ms":2e3}"#,
    )?;
    let parsed = snapshot::read_meter(&meter).ok_or("source numeric spelling rejected")?;
    assert_eq!(parsed.observed_at_ms, 2000);
    assert_eq!(parsed.reset_at_ms, Some(1000));
    assert_eq!(parsed.id.as_units().last(), Some(&0xd800));
    Ok(())
}

#[test]
fn fixed_source_get_headers_timeout_and_api_key_no_request_contract_match() -> TestResult {
    #[derive(Deserialize)]
    struct Request {
        url: String,
        method: String,
        redirect: String,
        headers: Vec<(String, String)>,
        aborted: bool,
    }
    #[derive(Deserialize)]
    struct Wire {
        source_sha: String,
        timeouts: Vec<i32>,
        requests: Vec<Request>,
        unsupported_requests: usize,
    }
    let wire: Wire = serde_json::from_str(include_str!("fixtures/wire-oracle.json"))?;
    assert_eq!(wire.source_sha, "b59243d674567d418d1410f8390fc7fe47a08c92");
    assert_eq!(wire.timeouts, vec![fetch::TIMEOUT_MS]);
    assert_eq!(wire.unsupported_requests, 0);
    assert_eq!(wire.requests.len(), 1);
    let request = wire
        .requests
        .into_iter()
        .next()
        .ok_or("source GET absent")?;
    assert_eq!(request.url, fetch::URL);
    assert_eq!(request.method, "GET");
    assert_eq!(request.redirect, "manual");
    assert!(!request.aborted);
    let headers: std::collections::BTreeMap<_, _> = fetch::headers("public-fixture-only")
        .into_iter()
        .map(|(key, value)| (key.to_ascii_lowercase(), value))
        .collect();
    assert_eq!(headers, request.headers.into_iter().collect());
    Ok(())
}
