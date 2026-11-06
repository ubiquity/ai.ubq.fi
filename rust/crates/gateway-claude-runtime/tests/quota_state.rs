#![cfg(not(target_arch = "wasm32"))]
//! Source quota state custody: TS-written `usage`, `usage_snapshot` and
//! `response_headers` survive every Rust quota write (tests/claude-header-meter,
//! tests/claude-usage-meters and tests/claude-usage-retry state round trips).
use gateway_claude_runtime::host::AcceptedObservation;
use gateway_claude_runtime::persistence::{clear_guards, header_observed_at, quota_key};
use gateway_claude_runtime::pool::ClaudePoolSession;
use gateway_claude_runtime::quota::{
    HEADER_SOURCE, QuotaObservation, STANDARD_FAMILIES, StandardWindow, parse_record,
    parse_source_state, parse_state, stored_state,
};
use gateway_claude_runtime_test_support::fixture::{
    Host, NOW, Store, account, apply, raw, run, seed, selected,
};
use gateway_core::{
    js_string::JsString16, model::ModelId, source_control::decode_source_control_record,
    source_json::SourceJson, source_serialization as source, value::ValueEncoding,
};
use gateway_providers::types::Headers;
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
impl<T, E: std::fmt::Debug> Must<T> for Result<T, E> {
    fn must(self) -> TestResult<T> {
        self.map_err(|error| format!("{error:?}").into())
    }
}

fn headers(pairs: &[(&str, &str)]) -> Headers {
    pairs
        .iter()
        .map(|(k, v)| ((*k).into(), (*v).into()))
        .collect()
}
fn ts_state() -> Value {
    json!({
        "usage_snapshot": {"v": 1, "observed_at_ms": NOW - 60_000, "payload": {"limits": [{
            "kind": "future_weekly", "group": "weekly", "percent": 121.5,
            "resets_at": "2027-01-20T00:00:00Z", "severity": "vendor_future", "is_active": true,
            "scope": {"model": {"display_name": "Future model", "vendor": 7}}}], "vendor": {"kept": true}},
            "meters": [{"id": "stale-derived"}]},
        "response_headers": {"source": "response_headers", "windows": {
            "7d": {"status": null, "utilization": 0.2, "reset_at_ms": null, "observed_at_ms": NOW - 1_000}},
            "standard": {}, "representative_claim": "seven_day", "observed_at_ms": NOW - 1_000},
        "windows": {}, "standard": {},
        "usage": {"source": "oauth_usage", "state": "rate_limited", "status": 429, "message": " slow   down ",
            "attempted_at_ms": NOW - 60_000, "observed_at_ms": null, "retry_at_ms": NOW + 60_000},
        "representative_claim": null, "blocked_until_ms": null, "block_window": null,
        "cooldown_until_ms": null, "last_error": null, "updated_at_ms": NOW - 60_000
    })
}

#[test]
fn rust_header_write_preserves_ts_usage_snapshot_and_header_meter_state() -> TestResult {
    let store = Store::new();
    let a = account(1);
    let config = seed(&store, vec![a.clone()]);
    store.put(&quota_key(&a), "ts-written", ts_state());
    let host = Host::new(&store);
    let session = ClaudePoolSession::new(config);
    let first = selected(&session, &store, &host, &raw());
    let five = headers(&[("anthropic-ratelimit-unified-5h-utilization", "0.4")]);
    apply(&session, &store, &host, &first, 200, five, NOW);
    let state = store.quota(&a).must()?;
    let usage = state.usage.as_ref().must()?;
    assert_eq!(
        (usage.state.as_str(), usage.retry_at_ms),
        ("rate_limited", Some((NOW + 60_000) as f64))
    );
    assert_eq!(usage.message, Some(JsString16::from_utf8("slow down")));
    let snapshot = state.usage_snapshot.as_ref().must()?;
    let meters = source::member(snapshot, "meters").and_then(|m| source::array(&m));
    assert_eq!(meters.map(|m| m.len()), Some(1));
    assert!(snapshot.as_str().contains(r#""vendor":{"kept":true}"#));
    assert!(!snapshot.as_str().contains("stale-derived"));
    let headers = &state.response_headers.as_ref().must()?.observation;
    assert_eq!(headers.windows["5h"].utilization, Some(0.4));
    assert_eq!(headers.windows["7d"].utilization, Some(0.2));
    assert_eq!(headers.representative_claim.as_deref(), Some("seven_day"));
    assert_eq!(headers.observed_at_ms, NOW as f64);
    assert_eq!(state.windows["5h"].utilization, Some(0.4));
    assert!(!state.windows.contains_key("7d"));
    let accepted = host.accepted.borrow();
    assert_eq!(accepted[0].source.as_deref(), Some(HEADER_SOURCE));
    let event = accepted[0].source_value();
    assert_eq!(
        source::member(&event, "source").and_then(|v| source::string(&v)),
        Some(JsString16::from_utf8(HEADER_SOURCE))
    );
    let row = decode_source_control_record(&store.get(&quota_key(&a)).must()?).must()?;
    let stored: Value = row.deserialize().must()?;
    assert_eq!(
        stored["usage"]["retry_at_ms"].as_f64(),
        Some((NOW + 60_000) as f64)
    );
    assert_eq!(
        stored["usage_snapshot"]["payload"],
        ts_state()["usage_snapshot"]["payload"]
    );
    assert_eq!(stored["response_headers"]["source"], "response_headers");
    assert_eq!(
        run(header_observed_at(&store, &host, &a)).must()?,
        Some(NOW as f64)
    );
    drop(accepted);
    let cleared = run(clear_guards(&store, &host, &a)).must()?.must()?;
    assert_eq!(cleared.usage, state.usage);
    assert_eq!(cleared.usage_snapshot, state.usage_snapshot);
    assert_eq!(cleared.response_headers, state.response_headers);
    Ok(())
}

#[test]
fn header_family_timestamps_merge_independently_and_older_values_never_win() -> TestResult {
    let store = Store::new();
    let a = account(1);
    let config = seed(&store, vec![a.clone()]);
    let host = Host::new(&store);
    let session = ClaudePoolSession::new(config);
    let first = selected(&session, &store, &host, &raw());
    let newer = headers(&[("anthropic-ratelimit-unified-5h-utilization", "0.8")]);
    apply(&session, &store, &host, &first, 200, newer, NOW);
    let older = headers(&[
        ("anthropic-ratelimit-unified-5h-utilization", "0.1"),
        ("anthropic-ratelimit-requests-remaining", "49"),
    ]);
    apply(&session, &store, &host, &first, 200, older, NOW - 10);
    let state = store.quota(&a).must()?;
    let meter = &state.response_headers.must()?.observation;
    assert_eq!(meter.windows["5h"].utilization, Some(0.8));
    assert_eq!(meter.standard["requests"].remaining, Some(49.0));
    assert!(!meter.windows.contains_key("7d"));
    assert_eq!(meter.observed_at_ms, NOW as f64);
    Ok(())
}

#[test]
fn unmarked_canonical_values_are_never_relabeled_as_header_meter_evidence() -> TestResult {
    let mut value = ts_state();
    value["response_headers"] = Value::Null;
    value["windows"] = json!({"5h": {"status": null, "utilization": 0.8, "reset_at_ms": null, "observed_at_ms": NOW}});
    let state = parse_state(&value).must()?;
    assert!(state.response_headers.is_none());
    for bad in [
        json!({"source": "oauth_usage", "windows": {}, "standard": {}, "observed_at_ms": NOW}),
        json!({"source": "response_headers", "windows": {}, "standard": {}, "observed_at_ms": 0}),
        json!({"source": "response_headers", "usage_snapshot": null, "observed_at_ms": NOW}),
    ] {
        value["response_headers"] = bad;
        assert!(parse_state(&value).must()?.response_headers.is_none());
    }
    Ok(())
}

#[test]
fn usage_state_follows_the_source_reader_and_retry_deadline_rules() -> TestResult {
    let mut value = ts_state();
    for (state, retry, kept) in [
        ("rate_limited", NOW + 1, true),
        ("rate_limited", NOW - 60_000, false),
        ("scope_missing", NOW + 1, false),
    ] {
        value["usage"]["state"] = json!(state);
        value["usage"]["retry_at_ms"] = json!(retry);
        let usage = parse_state(&value).must()?.usage.must()?;
        assert_eq!(usage.retry_at_ms.is_some(), kept, "{state} {retry}");
    }
    for (field, bad) in [
        ("state", json!("future_state")),
        ("source", json!("response_headers")),
        ("status", json!(42)),
        ("message", json!(7)),
        ("attempted_at_ms", json!(0)),
        ("observed_at_ms", json!(-1)),
    ] {
        let mut broken = ts_state();
        broken["usage"][field] = bad;
        assert!(parse_state(&broken).must()?.usage.is_none(), "{field}");
    }
    let mut missing = ts_state();
    missing["usage"].as_object_mut().must()?.remove("status");
    assert!(parse_state(&missing).must()?.usage.is_none());
    let mut invalid = ts_state();
    invalid["usage_snapshot"]["payload"] = json!({});
    assert!(parse_state(&invalid).must()?.usage_snapshot.is_none());
    Ok(())
}

#[test]
fn profile_codec_retains_unpaired_units_in_usage_snapshot_and_header_meter() -> TestResult {
    let text = r#"{"updated_at_ms":1800000000000,"usage":{"source":"oauth_usage","state":"http_error","status":500,"message":"bad\ud800","attempted_at_ms":1799999990000,"observed_at_ms":1799999990000},"usage_snapshot":{"v":1,"observed_at_ms":1799999990000,"payload":{"five_hour":{"utilization":12,"resets_at":"2027-01-20T00:00:00.000Z","label":"x\udc00"}}},"response_headers":{"source":"response_headers","windows":{"5h":{"status":"allowed","utilization":0.5,"reset_at_ms":null,"observed_at_ms":1800000000000}},"standard":{},"representative_claim":null,"observed_at_ms":1800000000000}}"#;
    let state = parse_source_state(&SourceJson::parse(text).must()?).must()?;
    let usage = state.usage.as_ref().must()?;
    assert_eq!(
        usage.message.as_ref().must()?.as_units().last(),
        Some(&0xd800)
    );
    let snapshot = state.usage_snapshot.as_ref().must()?;
    assert!(snapshot.as_str().contains(r#""label":"x\udc00""#));
    assert!(snapshot.as_str().contains(r#""reset_at_ms":1800403200000"#));
    assert_eq!(
        state.response_headers.as_ref().must()?.observation.windows["5h"].utilization,
        Some(0.5)
    );
    let record = stored_state(&state, "profile-fixture".into()).must()?;
    assert_eq!(record.value_encoding, ValueEncoding::ProfileV2);
    assert_eq!(parse_record(&record, NOW).must()?, Some(state));
    Ok(())
}

#[test]
fn real_quota_writer_and_reader_retain_model_keys_and_reject_escape_text_identity() -> TestResult {
    let source = SourceJson::parse(r#"{"updated_at_ms":1000,"pool_proofs":{"model:\ud800":{"kind":"quota_exhausted","model_id":"\ud800","observed_at_ms":1000,"expires_at_ms":3000,"evidence_generation":"fixture-generation"}},"last_error":{"kind":"forbidden","message":"credit \udc00","status":403,"at_ms":1000}}"#).must()?;
    let state = parse_source_state(&source).must()?;
    let model = ModelId::from_source(JsString16::from_units(vec![0xd800]));
    let (key, proof) = state
        .pool_proofs
        .as_ref()
        .must()?
        .first_key_value()
        .must()?;
    assert_eq!(key.as_units(), &[109, 111, 100, 101, 108, 58, 0xd800]);
    assert_eq!(proof.model_id.as_ref(), Some(&model));
    assert_ne!(
        proof.model_id.as_ref(),
        Some(&ModelId::from_scalar(r"\ud800"))
    );
    let record = stored_state(&state, "writer-fixture-revision".into()).must()?;
    assert_eq!(record.value_encoding, ValueEncoding::ProfileV2);
    assert_eq!(record.revision, "writer-fixture-revision");
    assert_eq!(record.expires_at_ms, None);
    assert_eq!(parse_record(&record, 1001).must()?, Some(state));
    let encoded = decode_source_control_record(&record).must()?;
    assert!(encoded.as_str().contains(r"\ud800"));
    assert!(encoded.as_str().contains(r"\udc00"));
    assert!(encoded.deserialize::<Value>().is_err());
    println!(
        "M35_CLAUDE_WRITER_FIXTURE={}",
        serde_json::to_string(&record).must()?
    );
    let mut expired = record.clone();
    expired.expires_at_ms = Some(1001);
    assert!(parse_record(&expired, 1001).must()?.is_none());
    Ok(())
}

#[test]
fn scalar_quota_writer_remains_json_and_malformed_optional_proofs_stay_absent() -> TestResult {
    let source = SourceJson::parse(r#"{"updated_at_ms":1000,"pool_proofs":{"bad":{"kind":"quota_exhausted","model_id":null,"observed_at_ms":1000,"expires_at_ms":3000,"evidence_generation":"fixture"},"good":{"kind":"auth_invalid","model_id":null,"observed_at_ms":1000,"expires_at_ms":3000,"evidence_generation":"fixture"}}}"#).must()?;
    let state = parse_source_state(&source).must()?;
    assert_eq!(state.pool_proofs.as_ref().must()?.len(), 1);
    let record = stored_state(&state, "scalar-fixture".into()).must()?;
    assert_eq!(record.value_encoding, ValueEncoding::Json);
    assert_eq!(parse_record(&record, 1000).must()?, Some(state));
    Ok(())
}

#[test]
fn accepted_standard_families_follow_actual_source_producer_order() -> TestResult {
    let mut quota = QuotaObservation::empty(1_800_000_000_000);
    for (index, family) in STANDARD_FAMILIES.into_iter().enumerate() {
        quota.standard.insert(
            family.into(),
            StandardWindow {
                limit: Some((index + 1) as f64),
                remaining: None,
                reset_at_ms: None,
                observed_at_ms: quota.observed_at_ms,
            },
        );
    }
    let event = AcceptedObservation {
        v: 1,
        observation_id: "fixture-observation".into(),
        account_id: "fixture-account".into(),
        account_cohort_id: "fixture-cohort".into(),
        credential_kind: "api_key".into(),
        workspace_id: Some(JsString16::from_units(vec![119, 0xd800])),
        source_observed_at_ms: Some(1_800_000_000_000),
        sampled_at_ms: 1_800_000_000_000,
        response_status: Some(200),
        state: "available".into(),
        observation: quota,
        source: None,
    };
    let document = event.source_value();
    assert!(source::member(&document, "source").is_none());
    let observation = source::member(&document, "observation").must()?;
    let standard = source::member(&observation, "standard").must()?;
    let fields = source::entries(&standard).must()?;
    assert_eq!(
        fields
            .iter()
            .map(|(name, _)| name.try_utf8().must())
            .collect::<TestResult<Vec<_>>>()?,
        STANDARD_FAMILIES
    );
    for (index, (_, value)) in fields.iter().enumerate() {
        assert_eq!(
            source::number(&source::member(value, "limit").must()?),
            Some((index + 1) as f64)
        );
    }
    assert_eq!(
        source::string(&source::member(&document, "workspace_id").must()?),
        event.workspace_id
    );
    // Serialization must not mutate the in-memory map or any consumer's source JSON.
    assert_eq!(
        event
            .observation
            .standard
            .keys()
            .map(String::as_str)
            .collect::<Vec<_>>(),
        vec!["input-tokens", "output-tokens", "requests"]
    );
    Ok(())
}
