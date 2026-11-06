//! fc42445 header-meter seam: tests/claude-header-meter.test.ts,
//! tests/claude-capacity-analytics.test.ts and tests/claude-usage-meters.test.ts
//! (history, rollup and capacity-view shapes only; no provider is ever read).
mod support;
use gateway_auth::crypto::sha256_hex;
use gateway_core::{
    js_string::JsString16, source_control::encode_source_control_record, source_json::SourceJson,
    source_serialization as source, state::StoredRecord, tuple_key::TupleKey,
};
use gateway_observability::{
    ObservabilityRuntime,
    capacity::claude_source,
    claude_capacity::{point_for_observation, valid_bucket_source, valid_history_point_source},
    host_types::{
        ClaudeAcceptedObservation, ClaudeAccountReadback, CredentialKind, CredentialMetadata,
    },
    types::{HealthProvider, HealthTarget},
};
use serde_json::{Value, json};
use support::{Host, Memory, run};
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

const AT: i64 = 7_200_000;
fn event(source: Option<&str>) -> TestResult<ClaudeAcceptedObservation> {
    let oracle = SourceJson::parse(include_str!("fixtures/claude_capacity_source.json")).must()?;
    let case = member(&oracle, "cases")?.array_items().must()?[0].clone();
    let mut event = ClaudeAcceptedObservation::from_source(&member(&case, "event")?).must()?;
    event.source = source.map(str::to_owned);
    Ok(event)
}
fn member(value: &SourceJson, name: &str) -> TestResult<SourceJson> {
    source::member(value, name).must()
}
fn with(value: &SourceJson, name: &str, replacement: &str) -> TestResult<SourceJson> {
    let mut entries = source::entries(value).must()?;
    let name = JsString16::from_utf8(name);
    let replacement = SourceJson::parse(replacement).must()?;
    match entries.iter_mut().find(|(k, _)| *k == name) {
        Some((_, v)) => *v = replacement,
        None => entries.push((name, replacement)),
    }
    source::object_keys(entries).must()
}
fn key(value: Value) -> TestResult<String> {
    Ok(TupleKey::from_json_parts(value.as_array().must()?)
        .must()?
        .to_record_key()
        .0)
}

#[test]
fn header_points_carry_source_in_their_digest_and_the_reader_rejects_other_sources() -> TestResult {
    let plain = point_for_observation(&event(None)?).must()?.must()?;
    let marked = point_for_observation(&event(Some("response_headers"))?)
        .must()?
        .must()?;
    assert!(source::member(&plain, "source").is_none());
    assert_eq!(member(&marked, "source")?.as_str(), r#""response_headers""#);
    let observation: Value = event(None)?.observation.deserialize().must()?;
    let digest = json!({"account":"synthetic-account","cohort":"synthetic-cohort","kind":"oauth",
        "workspace":"workspace-scalar","state":"available","response_status":200,
        "observation":observation,"source":"response_headers"});
    let digest = source::canonical(&SourceJson::from_serializable(&digest).must()?).must()?;
    assert_eq!(
        member(&marked, "point_id")?.as_str(),
        format!("\"{}\"", sha256_hex(digest.as_bytes()))
    );
    assert_ne!(member(&marked, "point_id")?, member(&plain, "point_id")?);
    let keys: Vec<_> = source::entries(&marked).must()?;
    let names = keys
        .iter()
        .map(|(k, _)| k.try_utf8().must())
        .collect::<TestResult<Vec<_>>>()?;
    let at = |name: &str| names.iter().position(|n| n == name).must();
    assert!(at("response_status")? < at("source")? && at("source")? < at("windows")?);
    assert!(valid_history_point_source(&marked));
    for bad in [r#""oauth_usage""#, "null", "1"] {
        assert!(!valid_history_point_source(&with(&marked, "source", bad)?));
    }
    assert!(
        point_for_observation(&event(Some("oauth_usage"))?)
            .must()?
            .is_none()
    );
    Ok(())
}

#[test]
fn retained_direct_meter_points_and_usage_flags_survive_a_header_write() -> TestResult {
    let snapshot = r#"{"v":1,"observed_at_ms":7100000,"payload":{"five_hour":{"utilization":12,"resets_at":null}},"meters":[]}"#;
    let plain = point_for_observation(&event(None)?).must()?.must()?;
    let direct = with(
        &with(&plain, "usage_snapshot", snapshot)?,
        "point_id",
        r#""direct-point""#,
    )?;
    assert!(valid_history_point_source(&direct));
    let late = snapshot.replace("7100000", "7300000");
    assert!(!valid_history_point_source(&with(
        &direct,
        "usage_snapshot",
        &late
    )?));
    assert!(!valid_history_point_source(&with(
        &direct,
        "usage_snapshot",
        "null"
    )?));
    let bucket = format!(
        r#"{{"v":1,"bucket_start_at_ms":{},"account_id":"synthetic-account","account_cohort_id":"synthetic-cohort","points":[{}],"flags":{{"normal":1,"unavailable":false,"recovery":false,"five_hour":false,"seven_day":false,"usage":true}}}}"#,
        source::number(&member(&plain, "bucket_start_at_ms")?).must()?,
        direct.as_str()
    );
    let bucket = SourceJson::parse(&bucket).must()?;
    assert!(valid_bucket_source(&bucket));
    let memory = Memory::default();
    let host = Host::default();
    host.at.set(AT);
    let history = json!([
        "uos_ai",
        "provider_capacity",
        "claude",
        "v1",
        "history",
        source::number(&member(&plain, "bucket_start_at_ms")?).must()?,
        "synthetic-account",
        "synthetic-cohort"
    ]);
    let record = encode_source_control_record("ts-bucket", &bucket, None).must()?;
    memory
        .rows
        .borrow_mut()
        .insert(key(history.clone())?, record);
    let runtime = ObservabilityRuntime::new(memory.clone(), host);
    assert!(
        run(runtime.record_claude_capacity_observation(&event(Some("response_headers"))?))
            .must()?
    );
    let stored = memory.rows.borrow()[&key(history)?].clone();
    let stored = gateway_core::source_control::decode_source_control_record(&stored).must()?;
    assert_eq!(
        member(&member(&stored, "flags")?, "usage")?.as_str(),
        "true"
    );
    let points = member(&stored, "points")?.array_items().must()?;
    assert_eq!(points.len(), 2);
    assert!(
        points
            .iter()
            .any(|p| source::member(p, "usage_snapshot").is_some())
    );
    assert!(points.iter().any(|p| {
        source::member(p, "source").is_some_and(|s| s.as_str() == r#""response_headers""#)
    }));
    Ok(())
}

fn account(quota: &str) -> TestResult<ClaudeAccountReadback> {
    Ok(ClaudeAccountReadback {
        target: HealthTarget {
            provider: HealthProvider::ClaudeCode,
            identity: "synthetic-account:synthetic-cohort".into(),
            source_checks: vec![],
        },
        account_id: "synthetic-account".into(),
        account_cohort_id: "synthetic-cohort".into(),
        label: None,
        credential: CredentialMetadata {
            kind: CredentialKind::Oauth,
            source: "kv".into(),
            hint: JsString16::from_utf8("sk-ant-oat...tail"),
            label: None,
            updated_at_ms: Some(1),
        },
        workspace: None,
        quota: Some(SourceJson::parse(quota).must()?),
    })
}

#[test]
fn capacity_view_reads_only_response_header_meters_and_never_a_direct_snapshot() -> TestResult {
    let window =
        r#"{"status":"allowed","utilization":0.8,"reset_at_ms":null,"observed_at_ms":7100000}"#;
    let unmarked = format!(
        r#"{{"windows":{{"5h":{window}}},"standard":{{"requests":{{"limit":50,"remaining":49,"reset_at_ms":null,"observed_at_ms":7100000}}}},"usage":{{"source":"oauth_usage","state":"ok","status":200,"message":null,"attempted_at_ms":7000000,"observed_at_ms":7000000}},"usage_snapshot":{{"v":1,"observed_at_ms":7000000,"payload":{{"five_hour":{{"utilization":99,"resets_at":null}}}},"meters":[]}},"representative_claim":null,"blocked_until_ms":null,"block_window":null,"cooldown_until_ms":null,"last_error":null,"updated_at_ms":7100000}}"#
    );
    let view = claude_source(&account(&unmarked)?, &Value::Null, AT).must()?;
    let view: Value = view.deserialize().must()?;
    assert_eq!(
        view["windows"],
        json!({"five_hour": null, "seven_day": null})
    );
    assert_eq!(view["standard"], json!([]));
    assert_eq!(view["usage"], Value::Null);
    assert!(view.get("usage_snapshot").is_none());
    assert_eq!(view["source_observed_at_ms"], Value::Null);
    assert_eq!(view["state"], "available");
    let marked = unmarked.replacen(
        r#"{"windows""#,
        &format!(
            r#"{{"response_headers":{{"source":"response_headers","windows":{{"5h":{window}}},"standard":{{}},"representative_claim":null,"observed_at_ms":7100000}},"windows""#
        ),
        1,
    );
    let view: Value = claude_source(&account(&marked)?, &Value::Null, AT)
        .must()?
        .deserialize()
        .must()?;
    assert_eq!(view["windows"]["five_hour"]["used_percent"], 80.0);
    assert_eq!(view["standard"], json!([]));
    assert_eq!(view["source_observed_at_ms"], 7_100_000.0);
    assert_eq!(view["usage"], Value::Null);
    let keys: Vec<_> = view.as_object().must()?.keys().cloned().collect();
    let at = |name: &str| keys.iter().position(|n| n == name).must();
    assert!(at("standard")? < at("usage")? && at("usage")? < at("blocked_until_ms")?);
    Ok(())
}

#[test]
fn usage_rollups_are_listed_from_their_own_namespace_and_corruption_is_unavailable() -> TestResult {
    let meter = json!({"id":"legacy:five_hour","label":"5-hour window","kind":"five_hour","group":"session",
        "scope":null,"severity":null,"is_active":false,"used_percent":12,"resets_at":null,
        "reset_at_ms":null,"observed_at_ms":7_200_000});
    let rollup = json!({"v":1,"bucket_start_at_ms":7_200_000,"account_id":"synthetic-account",
        "account_cohort_id":"synthetic-cohort","credential_kind":"oauth","workspace_id":null,
        "meter":meter,"samples":1,"first_observed_at_ms":7_200_000,"last_observed_at_ms":7_200_000,
        "min_used_percent":12,"max_used_percent":12});
    let anchor = sha256_hex(br#"["legacy:five_hour",null]"#);
    let row_key = key(json!([
        "uos_ai",
        "provider_capacity",
        "claude",
        "v1",
        "usage_rollup",
        7_200_000,
        "synthetic-account",
        "synthetic-cohort",
        anchor
    ]))?;
    let list = |value: Value, at: &str| {
        let memory = Memory::default();
        let host = Host::default();
        host.at.set(AT);
        let record = StoredRecord::json("ts-usage-rollup", value, None);
        memory.rows.borrow_mut().insert(at.to_owned(), record);
        let runtime = ObservabilityRuntime::new(memory, host);
        let response = run(runtime.capacity_rollups_response(90)).must()?;
        let body = SourceJson::parse_bytes(&response.body.to_bytes().must()?).must()?;
        member(&body, "claude_usage_rollups")?
            .deserialize::<Value>()
            .must()
    };
    let listed = list(rollup.clone(), &row_key)?;
    assert_eq!(listed["scan"], "ok");
    assert_eq!(listed["truncated"], false);
    assert_eq!(listed["points"], json!([rollup.clone()]));
    let wrong_key = key(json!([
        "uos_ai",
        "provider_capacity",
        "claude",
        "v1",
        "usage_rollup",
        7_200_000,
        "synthetic-account",
        "other-cohort",
        anchor
    ]))?;
    assert_eq!(list(rollup.clone(), &wrong_key)?["scan"], "unavailable");
    let mut corrupt = rollup;
    corrupt["min_used_percent"] = json!(50);
    let corrupt = list(corrupt, &row_key)?;
    assert_eq!(
        (corrupt["scan"].clone(), corrupt["points"].clone()),
        (json!("unavailable"), json!([]))
    );
    Ok(())
}
