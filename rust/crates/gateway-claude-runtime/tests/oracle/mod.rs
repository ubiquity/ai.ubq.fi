#![cfg(not(target_arch = "wasm32"))]
use gateway_claude_runtime_test_support::fixture::{
    Host, NOW, Store, account, head, raw, run, seed, selected,
};

use gateway_claude_runtime::{persistence::quota_key, pool::ClaudePoolSession, response};
use gateway_core::claude::ClaudeErrorEvidence as ErrorEvidence;
use gateway_providers::types::Headers;
use serde_json::Value;

fn fixture() -> Value {
    let value: Value =
        serde_json::from_str(include_str!("../fixtures/source-golden.json")).unwrap();
    assert_eq!(
        value["source_sha"],
        "6d4d4790a7c06514b92938ff677dbfd52764461d"
    );
    assert_eq!(value["now"], NOW);
    value
}
fn headers(value: &Value) -> Headers {
    let headers: Headers = serde_json::from_value(value.clone()).unwrap();
    // The Bun oracle constructs Fetch Headers, which normalizes HTTP OWS.
    headers
        .into_iter()
        .map(|(key, value)| (key, value.trim_matches([' ', '\t']).into()))
        .collect()
}
fn evidence(case: &Value) -> crate::support::TestResult<ErrorEvidence> {
    Ok(ErrorEvidence::from_prefix(
        serde_json::from_value(case["body_units"].clone())?,
        case["complete"].as_bool().unwrap_or(true),
    )?)
}

#[test]
fn exact_bun_source_classification_vectors_preserve_all_detail_units() -> crate::support::TestResult
{
    let fixture = fixture();
    let cases = crate::support::required(fixture["classes"].as_array())?;
    assert_eq!(cases.len(), 83);
    for (index, case) in cases.iter().enumerate() {
        let classified = response::classify_headers(
            crate::support::required(case["status"].as_u64())? as u16,
            &headers(&case["headers"]),
            NOW,
            &|date| (date == "Wed, 18 Nov 2026 00:00:00 GMT").then_some(1_794_960_000_000),
        );
        let actual = response::refine_error(classified, &evidence(case)?)?;
        let expected = &case["expected"];
        assert_eq!(
            actual.kind.id(),
            crate::support::required(expected["kind"].as_str())?,
            "classification {index}"
        );
        assert_eq!(
            actual.retry_after_ms,
            expected["retry_after_ms"].as_i64(),
            "retry {index}"
        );
        assert_eq!(
            actual.exhaustion.as_ref().map(|e| e.window.as_str()),
            expected["exhaustion"]["window"].as_str(),
            "window {index}"
        );
        assert_eq!(
            actual.exhaustion.as_ref().and_then(|e| e.reset_at_ms),
            expected["exhaustion"]["reset_at_ms"].as_f64(),
            "reset {index}"
        );
        let expected_units: Option<Vec<u16>> =
            serde_json::from_value(expected["detail_units"].clone())?;
        assert_eq!(
            actual.detail.as_ref().map(|s| s.as_units().to_vec()),
            expected_units,
            "detail {index}"
        );
    }
    Ok(())
}

#[test]
fn exact_bun_source_observation_vectors_preserve_missing_and_numeric_headers()
-> crate::support::TestResult {
    let fixture = fixture();
    let cases = crate::support::required(fixture["observations"].as_array())?;
    assert_eq!(cases.len(), 8);
    for (index, case) in cases.iter().enumerate() {
        let expected: Option<gateway_claude_usage_quota::QuotaObservation> =
            serde_json::from_str(&case["expected"].to_string())?;
        assert_eq!(
            response::observe_headers(&headers(&case["headers"]), NOW),
            expected,
            "observation {index}"
        );
    }
    Ok(())
}

#[test]
fn exact_bun_source_state_vectors_preserve_malformed_optional_record_behavior()
-> crate::support::TestResult {
    let fixture = fixture();
    let cases = crate::support::required(fixture["states"].as_array())?;
    assert_eq!(cases.len(), 7);
    for (index, case) in cases.iter().enumerate() {
        let expected: Option<gateway_claude_usage_quota::QuotaState> =
            serde_json::from_str(&case["expected"].to_string())?;
        assert_eq!(
            gateway_claude_usage_quota::parse_state(&case["raw"]),
            expected,
            "state {index}"
        );
    }
    Ok(())
}

#[test]
fn exact_bun_source_outcomes_match_real_same_store_publication() -> crate::support::TestResult {
    let fixture = fixture();
    let cases = crate::support::required(fixture["outcomes"].as_array())?;
    assert_eq!(cases.len(), 4);
    for (index, case) in cases.iter().enumerate() {
        let store = Store::new();
        let account = account(1);
        let config = seed(&store, vec![account.clone()]);
        let host = Host::new(&store);
        let session = ClaudePoolSession::new(config);
        let selected = selected(&session, &store, &host, &raw());
        store.put(
            &quota_key(&account),
            "source-previous",
            case["previous"].clone(),
        );
        let head = run(session.observe_response(
            &store,
            &host,
            &selected,
            head(
                crate::support::required(case["status"].as_u64())? as u16,
                headers(&case["headers"]),
                NOW,
            ),
        ))?;
        run(session.after_outcome(&store, &host, head, evidence(case)?))?;
        let expected: gateway_claude_usage_quota::QuotaState =
            serde_json::from_str(crate::support::required(case["expected_raw"].as_str())?)?;
        assert_eq!(
            crate::support::required(store.quota(&account))?,
            expected,
            "persisted outcome {index}"
        );
    }
    Ok(())
}
