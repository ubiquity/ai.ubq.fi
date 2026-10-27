use super as values;
use gateway_authority_paid_records::PaidAttempt;
use gateway_authority_paid_test_support::{NOW, Store, decode_json, key, request, rollup_key};
use gateway_authority_paid_test_support::{backfill, family};
use gateway_core::provider::PaidProvider;
use serde_json::json;
#[test]
fn rollup_race_stops_before_failed_row_and_later_success_cannot_skip_it() {
    let store = Store::default();
    for id in ["a", "b", "c"] {
        store.put(key(id), request(id, true));
    }
    let row: PaidAttempt = decode_json(request("b", true)).unwrap();
    let mut racing = values::merge_rollup(
        None,
        &row,
        "model",
        "metered",
        (NOW - 1000) / 3_600_000 * 3_600_000,
        NOW,
    )
    .unwrap();
    racing["request_count"] = json!(5);
    *store.race.borrow_mut() = Some((rollup_key("b"), racing));
    let first = backfill(&store, 10);
    assert_eq!(
        (
            first.requests.processed,
            first.requests.failed,
            first.requests.truncated
        ),
        (1, 1, true)
    );
    assert!(store.value(&key("b"))["usage_rollup_at_ms"].is_null());
    assert!(store.value(&key("c"))["usage_rollup_at_ms"].is_null());
    assert_eq!(
        store.value(&family("rollup_backfill_cursor"))["request_key"][5],
        "a"
    );
    let next = backfill(&store, 10);
    assert_eq!(next.requests.processed, 2);
    assert_eq!(store.value(&rollup_key("a"))["request_count"], 1);
    assert_eq!(store.value(&rollup_key("b"))["request_count"], 6);
}
#[test]
fn source_valid_rollup_preserves_identity_and_accumulates_exact_fields() {
    let row: PaidAttempt = decode_json(request("a", true)).unwrap();
    assert_eq!(row.reservation.provider, PaidProvider::Metered);
    let first = values::merge_rollup(None, &row, "model", "metered", 100, NOW).unwrap();
    let second =
        values::merge_rollup(Some(&first), &row, "other", "surplus", 200, NOW + 1).unwrap();
    assert_eq!(second["model"], "model");
    assert_eq!(second["provider"], "metered");
    assert_eq!(second["bucket_start_at_ms"], 100);
    assert_eq!(second["request_count"], 2);
    assert_eq!(second["input_tokens"], 20);
    assert_eq!(second["cached_input_tokens"], 4);
    assert_eq!(second["output_tokens"], 6);
    assert_eq!(second["quota_sum"].as_f64(), Some(3.0));
    assert_eq!(second["spend_microcredits"].as_f64(), Some(10.0));
    let invalid = json!({"v":1,"quota_sum":"unknown"});
    assert_eq!(
        values::merge_rollup(Some(&invalid), &row, "model", "metered", 100, NOW).unwrap(),
        first
    );
}
#[test]
fn genuine_fractional_source_rollup_spend_is_not_rounded_to_fit_the_live_dto() {
    let row: PaidAttempt = decode_json(request("a", true)).unwrap();
    let mut existing = values::merge_rollup(None, &row, "model", "metered", 100, NOW).unwrap();
    existing["spend_microcredits"] = json!(0.25);
    let merged = values::merge_rollup(Some(&existing), &row, "model", "metered", 100, NOW).unwrap();
    assert_eq!(merged["spend_microcredits"].as_f64(), Some(5.25));
    assert_eq!(merged["quota_sum"].as_f64(), Some(3.0));
}
