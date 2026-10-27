use gateway_authority_keys as keys;
use gateway_authority_paid_ledger::PaidLedgerOperations;
use gateway_authority_paid_records::{PaidAttempt, source_usage::surplus_usage_from_source};
use gateway_authority_paid_records::{PaidCorrelation, PaidRollup, PaidWindow};
use gateway_authority_paid_test_support::{NOW, Store, TestResult, decode_json, key, request, run};
use gateway_authority_paid_test_support::{backfill, rollup_key, window};
use gateway_authority_paid_test_support::{live_usage, pending_for_live_settlement};
use gateway_contracts::provider::PaidProvider;
use serde_json::{Value, json};
#[test]
fn pending_backfill_marker_does_not_block_later_real_settlement_or_double_fold() {
    // Actual frozen source oracle under pinned Bun1.4: a pending backfill marker
    // does not suppress paidFallbackSettlementRollupV3's later actual usage fold.
    let store = Store::default();
    let row = pending_for_live_settlement(&store, "model");
    assert_eq!(backfill(&store, 10).requests.rollups_written, 0);
    assert_eq!(store.value(&key("a"))["usage_rollup_at_ms"], NOW);
    let authority = store.authority();
    assert!(run(authority.settle_paid(&row.reservation, &live_usage(), NOW + 1)).unwrap());
    assert_eq!(store.value(&key("a"))["billing_state"], "settled");
    assert_eq!(store.value(&rollup_key("a"))["request_count"], 1);
    assert_eq!(store.value(&rollup_key("a"))["spend_microcredits"], 5);
    assert!(!run(authority.settle_paid(&row.reservation, &live_usage(), NOW + 2)).unwrap());
    assert_eq!(backfill(&store, 10).requests.rollups_written, 0);
    assert_eq!(store.value(&rollup_key("a"))["request_count"], 1);
}
#[test]
fn later_live_settlement_uses_the_same_source_trimmed_rollup_identity() {
    let store = Store::default();
    let row = pending_for_live_settlement(&store, " model ");
    assert!(
        run(store
            .authority()
            .settle_paid(&row.reservation, &live_usage(), NOW))
        .unwrap()
    );
    let rollup = store.value(&rollup_key("a"));
    assert_eq!(rollup["model"], "model");
    assert_eq!(rollup["quota_sum"].as_f64(), Some(5.0));
    assert_eq!(backfill(&store, 10).requests.rollups_written, 0);
    assert_eq!(store.value(&rollup_key("a"))["request_count"], 1);
}
#[test]
fn source_valid_fractional_rollup_remains_settleable_without_fractional_billing() {
    // Actual source validator + prepareSettlementRollupV3 accepted this imported
    // aggregate in the pinned Bun oracle. Only spend/quota accept fractions;
    // request/token counters remain source-safe integers. Request/window bill 5.
    // Oracle result SHA256 c0b184bd0ac26d7ec2261177b5da5464d3161d104cbd51216ad972b90a784134.
    let store = Store::default();
    let row = pending_for_live_settlement(&store, "model");
    let imported = json!({
        "v": 1,
        "bucket_start_at_ms": (NOW - 1000) / 3_600_000 * 3_600_000,
        "model": "model",
        "provider": "metered",
        "request_count": 1,
        "quota_sum": 1.5,
        "input_tokens": 10,
        "cached_input_tokens": 2,
        "output_tokens": 3,
        "spend_microcredits": 0.25,
        "first_request_at_ms": NOW - 1000,
        "last_request_at_ms": NOW - 1000,
        "updated_at_ms": NOW,
    });
    store.put(rollup_key("a"), imported.clone());
    let authority = store.authority();
    let result = run(authority.settle_paid(&row.reservation, &live_usage(), NOW + 1));
    if result.is_err() {
        assert_eq!(store.value(&rollup_key("a")), imported);
        assert_eq!(store.value(&key("a"))["billing_state"], "pending");
        assert_eq!(
            store.value(&keys::paid_window("key", NOW))["settled_microcredits"],
            0
        );
    }
    assert!(
        matches!(result, Ok(true)),
        "Actual source accepts this fractional historical aggregate: {result:?}"
    );
    let rollup = store.value(&rollup_key("a"));
    assert_eq!(rollup["spend_microcredits"].as_f64(), Some(5.25));
    assert_eq!(rollup["request_count"], 2);
    assert_eq!(rollup["quota_sum"].as_f64(), Some(6.5));
    assert_eq!(rollup["input_tokens"], 20);
    assert_eq!(rollup["cached_input_tokens"], 4);
    assert_eq!(rollup["output_tokens"], 6);
    assert_eq!(store.value(&key("a"))["spend_microcredits"], 5);
    let window = store.value(&keys::paid_window("key", NOW));
    assert_eq!(window["settled_microcredits"], 5);
    assert_eq!(window["reserved_microcredits"], 0);
    assert_eq!(window["pending_count"], 0);
    assert!(!run(authority.settle_paid(&row.reservation, &live_usage(), NOW + 2)).unwrap());
    assert_eq!(store.value(&rollup_key("a")), rollup);
}
#[test]
fn shared_surplus_usage_adapter_matches_all_actual_source_pricing_cases() -> TestResult {
    let cases: Vec<Value> = serde_json::from_str(include_str!(
        "../../gateway-authority/src/paid_source_usage_cases.json"
    ))?;
    assert_eq!(cases.len(), 26);
    for case in cases {
        let name = case["name"].as_str().ok_or("missing value")?;
        let mut row: PaidAttempt = decode_json(request("a", false))?;
        row.reservation.provider = PaidProvider::Surplus;
        row.reservation.attempt_id.0 = case["attempt_id"].as_str().ok_or("missing value")?.into();
        row.model = case["model"].as_str().ok_or("missing value")?.into();
        row.quota_per_credit = case["quota_per_credit"].as_i64().ok_or("missing value")?;
        let usage = gateway_contracts::source_usage::extract_usage_tokens(Some(&case["raw"]));
        let pricing = case
            .get("pricing")
            .filter(|value| !value.is_null())
            .map(gateway_source::source_json::SourceJson::from_serializable)
            .transpose()?;
        let actual = surplus_usage_from_source(&row, usage.as_ref(), pricing.as_ref());
        let expected = &case["expected"];
        if expected.is_null() {
            assert!(actual.is_none(), "{name}: unexpected billing input");
            continue;
        }
        let actual = actual.unwrap_or_else(|| panic!("{name}: source expects billing input"));
        assert_eq!(
            Some(actual.provider_quota),
            expected["provider_quota"].as_f64(),
            "{name}"
        );
        assert_eq!(
            json!(actual.input_tokens),
            expected["input_tokens"],
            "{name}"
        );
        assert_eq!(
            json!(actual.cached_input_tokens),
            expected["cached_input_tokens"],
            "{name}"
        );
        assert_eq!(
            json!(actual.output_tokens),
            expected["output_tokens"],
            "{name}"
        );
        let PaidCorrelation::SurplusReservation {
            attempt_id,
            buyer_cost_micro,
        } = actual.correlation
        else {
            panic!("{name}: wrong correlation");
        };
        assert_eq!(attempt_id, row.reservation.attempt_id, "{name}");
        assert_eq!(
            json!(buyer_cost_micro),
            expected["buyer_cost_micro"],
            "{name}"
        );
    }
    Ok(())
}
#[test]
fn fractional_aggregate_spend_does_not_widen_counters_request_or_window_billing() {
    let store = Store::default();
    store.put(key("a"), request("a", true));
    assert_eq!(backfill(&store, 10).requests.rollups_written, 1);
    let raw = store.value(&rollup_key("a"));
    for field in [
        "request_count",
        "input_tokens",
        "cached_input_tokens",
        "output_tokens",
    ] {
        let mut invalid = raw.clone();
        invalid[field] = json!(0.25);
        assert!(decode_json::<PaidRollup>(invalid).is_err(), "{field}");
    }
    for value in [json!(-0.25), json!("0.25"), Value::Null] {
        let mut invalid = raw.clone();
        invalid["spend_microcredits"] = value;
        assert!(decode_json::<PaidRollup>(invalid).is_err());
    }
    let mut invalid_request = request("a", true);
    invalid_request["spend_microcredits"] = json!(0.25);
    assert!(decode_json::<PaidAttempt>(invalid_request).is_err());
    let mut invalid_window = window(NOW);
    invalid_window["settled_microcredits"] = json!(0.25);
    assert!(decode_json::<PaidWindow>(invalid_window).is_err());
}
