//! Mirrors tests/claude-usage-meters.test.ts schema vectors.
use gateway_claude_usage::{
    rollup::read_usage_rollup,
    snapshot::{LEGACY_WINDOWS, parse_snapshot, read_meter, read_snapshot},
};
use gateway_core::source_json::SourceJson;
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

const AT: f64 = 1_800_000_000_000.0;
fn row(patch: Value) -> TestResult<Value> {
    let mut row = json!({
        "kind": "future_weekly", "group": "weekly", "percent": 121.5,
        "resets_at": "2027-01-20T00:00:00Z", "severity": "vendor_future", "is_active": true,
        "scope": {"model": {"display_name": "Future model", "vendor": 7}, "surface": {"display_name": "OAuth apps"}}
    });
    for (key, value) in patch.as_object().must()? {
        row[key] = value.clone();
    }
    Ok(row)
}
fn snapshot(payload: Value) -> TestResult<Option<Value>> {
    let payload = SourceJson::from_serializable(&payload).must()?;
    parse_snapshot(&payload, AT)
        .map(|s| s.deserialize().must())
        .transpose()
}

#[test]
fn independent_nullable_legacy_usage_fields_retain_their_exact_source_shape() -> TestResult {
    for key in LEGACY_WINDOWS {
        let input = json!({ key: {"utilization": null, "resets_at": null} });
        let value = snapshot(input.clone())?.must()?;
        assert_eq!(value["payload"], input);
        assert_eq!(value["meters"].as_array().must()?.len(), 1);
        assert_eq!(value["meters"][0]["used_percent"], Value::Null);
        assert_eq!(value["meters"][0]["reset_at_ms"], Value::Null);
        assert_eq!(snapshot(json!({ key: null }))?.must()?["meters"], json!([]));
    }
    assert!(snapshot(json!({}))?.is_none());
    assert!(snapshot(json!({"seven_day": {"resets_at": null}}))?.is_none());
    assert!(snapshot(json!({"limits": [row(json!({"percent": "1"}))?]}))?.is_none());
    assert!(snapshot(json!({"limits": [row(json!({"scope": {"model": {}}}))?]}))?.is_none());
    let odd = snapshot(
        json!({"five_hour": {"utilization": 125, "resets_at": "unparseable-provider-text"}}),
    )?
    .must()?;
    assert_eq!(odd["meters"][0]["used_percent"], 125);
    assert_eq!(odd["meters"][0]["reset_at_ms"], Value::Null);
    assert_eq!(odd["meters"][0]["label"], "5-hour window");
    assert_eq!(odd["meters"][0]["group"], "session");
    Ok(())
}

#[test]
fn dynamic_meters_preserve_server_order_scope_severity_headline_and_over_100_values() -> TestResult
{
    let payload = json!({
        "limits": [row(json!({}))?, row(json!({"kind": "future_session", "group": "session", "percent": 2, "is_active": false}))?, row(json!({}))?],
        "seven_day": {"utilization": 10, "resets_at": null},
        "vendor": {"retained": true}
    });
    let value = snapshot(payload.clone())?.must()?;
    assert_eq!(value["payload"], payload);
    let meters = value["meters"].as_array().must()?;
    let kinds =
        (meters.iter().map(|m| m["kind"].as_str().must())).collect::<TestResult<Vec<_>>>()?;
    assert_eq!(kinds, ["future_weekly", "future_session", "future_weekly"]);
    let ids = (meters.iter().map(|m| m["id"].as_str().must()))
        .collect::<TestResult<std::collections::BTreeSet<_>>>()?;
    assert_eq!(ids.len(), 3);
    assert_eq!(
        meters[0]["id"],
        r#"limits:["future_weekly","weekly",{"model":{"display_name":"Future model","vendor":7},"surface":{"display_name":"OAuth apps"}}]:0"#
    );
    assert_eq!(
        meters[0]["label"],
        "weekly · future_weekly · Future model · OAuth apps"
    );
    assert_eq!(meters[0]["used_percent"], 121.5);
    assert_eq!(meters[0]["severity"], "vendor_future");
    assert_eq!(
        (
            meters[0]["is_active"].clone(),
            meters[1]["is_active"].clone()
        ),
        (json!(true), json!(false))
    );
    assert_eq!(meters[0]["reset_at_ms"], 1_800_403_200_000_i64);
    let stored = SourceJson::from_serializable(&value).must()?;
    assert_eq!(
        read_snapshot(&stored)
            .map(|s| s.deserialize::<Value>().must())
            .transpose()?,
        Some(value)
    );
    let mut limits_empty = payload.clone();
    limits_empty["limits"] = json!([]);
    assert_eq!(snapshot(limits_empty)?.must()?["meters"], json!([]));
    let mut limits_null = payload.clone();
    limits_null["limits"] = Value::Null;
    assert_eq!(
        snapshot(limits_null)?.must()?["meters"][0]["kind"],
        "seven_day"
    );
    let extra = json!({"extra_usage": {"is_enabled": false, "monthly_limit": null, "used_credits": 12, "utilization": null, "currency": "USD"}});
    assert_eq!(snapshot(extra)?.must()?["meters"], json!([]));
    assert!(snapshot(json!({"extra_usage": {"is_enabled": "no"}}))?.is_none());
    assert!(snapshot(json!({"wattle_ember": {"utilization": null, "resets_at": null, "periods": [{"starts_at": 1}]}}))?.is_none());
    Ok(())
}

#[test]
fn stored_snapshots_recompute_meters_and_reject_malformed_provenance() -> TestResult {
    let value =
        snapshot(json!({"seven_day_opus": {"utilization": 3, "resets_at": null}}))?.must()?;
    let mut tampered = value.clone();
    tampered["meters"] = json!([{"id": "forged"}]);
    let stored = SourceJson::from_serializable(&tampered).must()?;
    assert_eq!(
        read_snapshot(&stored)
            .must()?
            .deserialize::<Value>()
            .must()?,
        value
    );
    for bad in [
        json!({"v": 2}),
        json!({"v": 1, "observed_at_ms": 0}),
        json!({"v": 1, "observed_at_ms": "1"}),
    ] {
        let mut broken = value.clone();
        for (key, field) in bad.as_object().must()? {
            broken[key] = field.clone();
        }
        assert!(read_snapshot(&SourceJson::from_serializable(&broken).must()?).is_none());
    }
    let unpaired =
        SourceJson::parse(r#"{"five_hour":{"utilization":1,"resets_at":"\ud800"}}"#).must()?;
    let kept = parse_snapshot(&unpaired, AT).must()?;
    assert!(kept.as_str().contains(r#""resets_at":"\ud800""#));
    Ok(())
}

#[test]
fn usage_rollups_validate_meter_bounds_and_identity() -> TestResult {
    let meter = value_meter()?;
    let rollup = json!({
        "v": 1, "bucket_start_at_ms": 1_800_000_000_000_i64 / 3_600_000 * 3_600_000,
        "account_id": "acct", "account_cohort_id": "cohort", "credential_kind": "oauth",
        "workspace_id": null, "meter": meter, "samples": 2,
        "first_observed_at_ms": 1_800_000_000_000_i64, "last_observed_at_ms": 1_800_000_000_000_i64,
        "min_used_percent": 1, "max_used_percent": 5
    });
    let read = read_usage_rollup(&SourceJson::from_serializable(&rollup).must()?).must()?;
    assert_eq!(read.account_id, "acct");
    assert_eq!(read.meter.anchor.as_str(), r#"["legacy:five_hour",null]"#);
    assert_eq!(
        read.key_bucket(),
        1_800_000_000_000_f64 - 1_800_000_000_000_f64 % 3_600_000.0
    );
    for (field, bad) in [
        ("samples", json!(0)),
        ("credential_kind", json!("cookie")),
        ("min_used_percent", json!(4)),
        ("max_used_percent", Value::Null),
        ("last_observed_at_ms", json!(1_799_999_999_999_i64)),
    ] {
        let mut broken = rollup.clone();
        broken[field] = bad;
        assert!(
            read_usage_rollup(&SourceJson::from_serializable(&broken).must()?).is_none(),
            "{field}"
        );
    }
    let mut no_reset = meter.clone();
    no_reset.as_object_mut().must()?.remove("reset_at_ms");
    assert!(read_meter(&SourceJson::from_serializable(&no_reset).must()?).is_none());
    Ok(())
}
fn value_meter() -> TestResult<Value> {
    Ok(
        snapshot(json!({"five_hour": {"utilization": 3, "resets_at": null}}))?.must()?["meters"][0]
            .clone(),
    )
}
