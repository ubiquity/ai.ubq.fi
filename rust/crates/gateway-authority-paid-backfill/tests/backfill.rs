use gateway_authority_contracts::PAID_RETENTION_MS;
use gateway_authority_keys as keys;
use gateway_authority_paid_backfill::PaidBackfillOperations;
use gateway_authority_paid_records::PaidRollup;
use gateway_authority_paid_test_support::{NOW, Store, TestResult, decode_json, key, request, run};
use gateway_authority_paid_test_support::{backfill, family, rollup_key, window};
use serde_json::json;
#[test]
fn historical_fold_is_atomic_and_marked_restart_only_restores_anchored_ttl() {
    let store = Store::default();
    store.put(key("a"), request("a", true));
    let first = backfill(&store, 10);
    assert_eq!(
        (
            first.requests.processed,
            first.requests.rollups_written,
            first.requests.failed,
            first.requests.truncated
        ),
        (1, 1, 0, false)
    );
    let original = store.value(&key("a"));
    assert_eq!(original["usage_rollup_at_ms"], NOW);
    assert_eq!(original["source_extra"], json!({"preserved":true}));
    let rollup = store.value(&rollup_key("a"));
    assert_eq!(rollup["model"], "model");
    assert_eq!(rollup["quota_sum"].as_f64(), Some(1.5));
    assert_eq!(rollup["spend_microcredits"].as_f64(), Some(5.0));
    assert_eq!(rollup["cached_input_tokens"], 2);
    assert_eq!(
        store.row(&key("a")).expires_at_ms,
        Some(NOW - 1000 + crate::PAID_RETENTION_MS)
    );
    // Simulate export/import losing expiry, then construct a fresh authority over the same store.
    store.put(key("a"), original.clone());
    let repeated = backfill(&store, 10);
    assert_eq!(
        (
            repeated.requests.processed,
            repeated.requests.rollups_written
        ),
        (0, 0)
    );
    assert_eq!(store.value(&key("a")), original);
    assert_eq!(store.value(&rollup_key("a")), rollup);
    assert_eq!(
        store.row(&key("a")).expires_at_ms,
        Some(NOW - 1000 + crate::PAID_RETENTION_MS)
    );
}
#[test]
fn inclusive_request_cursor_restarts_before_unprocessed_row_without_double_count() {
    let store = Store::default();
    for id in ["a", "b", "c"] {
        store.put(key(id), request(id, true));
    }
    let first = backfill(&store, 1);
    assert_eq!(
        (
            first.requests.scanned,
            first.requests.processed,
            first.requests.truncated
        ),
        (2, 1, true)
    );
    assert_eq!(
        store.value(&family("rollup_backfill_cursor"))["request_key"],
        json!(["uos_ai", "paid_fallback", "v3", "request", "key", "a"])
    );
    let second = backfill(&store, 1);
    assert_eq!(
        (
            second.requests.scanned,
            second.requests.processed,
            second.requests.truncated
        ),
        (3, 1, true)
    );
    let third = backfill(&store, 1);
    assert_eq!(
        (third.requests.processed, third.requests.truncated),
        (1, false)
    );
    assert!(
        !store
            .rows
            .borrow()
            .contains_key(&family("rollup_backfill_cursor").0)
    );
    for id in ["a", "b", "c"] {
        assert_eq!(store.value(&rollup_key(id))["request_count"], 1);
    }
}
#[test]
fn marked_rows_consume_scan_budget_but_not_fold_budget_and_resume_is_finite() {
    let store = Store::default();
    for index in 0..12 {
        let id = format!("a{index:02}");
        let mut value = request(&id, true);
        value["usage_rollup_at_ms"] = json!(NOW - 1);
        store.put(key(&id), value);
    }
    store.put(key("z"), request("z", true));
    let first = backfill(&store, 1);
    assert_eq!(
        (
            first.requests.scanned,
            first.requests.processed,
            first.requests.truncated
        ),
        (11, 0, true)
    );
    assert_eq!(
        store.value(&family("rollup_backfill_cursor"))["request_key"][5],
        "a09"
    );
    let next = backfill(&store, 1);
    assert_eq!(
        (
            next.requests.processed,
            next.requests.rollups_written,
            next.requests.truncated
        ),
        (1, 1, false)
    );
}
#[test]
fn thrown_fold_commit_keeps_prior_marker_and_retry_cannot_duplicate_it() {
    let store = Store::default();
    for id in ["a", "b"] {
        store.put(key(id), request(id, true));
    }
    store.fail(key("b"), true);
    assert!(run(store.authority().backfill_paid_usage(10, NOW)).is_err());
    assert_eq!(store.value(&key("a"))["usage_rollup_at_ms"], NOW);
    assert!(store.value(&key("b"))["usage_rollup_at_ms"].is_null());
    assert_eq!(backfill(&store, 10).requests.processed, 1);
    assert_eq!(store.value(&rollup_key("a"))["request_count"], 1);
}
#[test]
fn pending_row_gets_marker_and_retention_but_no_inferred_spend_or_settlement() {
    let store = Store::default();
    let original = request("a", false);
    store.put(key("a"), original.clone());
    let result = backfill(&store, 10);
    assert_eq!(
        (result.requests.processed, result.requests.rollups_written),
        (1, 0)
    );
    assert_eq!(
        store.value(&key("a"))["billing_state"],
        original["billing_state"]
    );
    assert!(store.value(&key("a"))["spend_microcredits"].is_null());
    assert!(!store.rows.borrow().contains_key(&rollup_key("a").0));
}
#[test]
fn window_batch_skips_inclusive_cursor_and_reports_the_real_final_batch() {
    let store = Store::default();
    for reset in [NOW - 1, NOW, NOW + 1] {
        store.put(keys::paid_window("key", reset), window(reset));
    }
    let first = backfill(&store, 1);
    assert_eq!(
        (first.windows.rewritten, first.windows.truncated),
        (1, true)
    );
    let second = backfill(&store, 1);
    assert_eq!(
        (
            second.windows.scanned,
            second.windows.rewritten,
            second.windows.truncated
        ),
        (2, 1, true)
    );
    let third = backfill(&store, 1);
    assert_eq!(
        (
            third.windows.scanned,
            third.windows.rewritten,
            third.windows.truncated
        ),
        (2, 1, false)
    );
    assert!(
        !store
            .rows
            .borrow()
            .contains_key(&family("rollup_backfill_window_cursor").0)
    );
    for reset in [NOW - 1, NOW, NOW + 1] {
        let key = keys::paid_window("key", reset);
        assert_eq!(store.value(&key), window(reset));
        assert_eq!(
            store.row(&key).expires_at_ms,
            Some(reset + crate::PAID_RETENTION_MS)
        );
    }
}
#[test]
fn failed_window_cas_or_throw_resumes_before_the_unwritten_ttl() {
    for error in [false, true] {
        let store = Store::default();
        for reset in [NOW - 1, NOW, NOW + 1] {
            store.put(keys::paid_window("key", reset), window(reset));
        }
        store.fail(keys::paid_window("key", NOW), error);
        let first = backfill(&store, 10);
        assert_eq!(
            (first.windows.rewritten, first.windows.truncated),
            (1, true)
        );
        assert_eq!(
            store.row(&keys::paid_window("key", NOW)).expires_at_ms,
            None
        );
        assert_eq!(
            store.row(&keys::paid_window("key", NOW + 1)).expires_at_ms,
            None
        );
        let next = backfill(&store, 10);
        assert_eq!((next.windows.rewritten, next.windows.truncated), (2, false));
    }
}
#[test]
fn invalid_windows_skip_and_expired_anchors_get_one_millisecond_not_a_fresh_year() {
    let store = Store::default();
    let mut invalid = window(1);
    invalid["pending_count"] = json!(-1);
    store.put(keys::paid_window("key", 1), invalid.clone());
    store.put(keys::paid_window("key", 2), window(2));
    let result = backfill(&store, 10);
    assert_eq!(
        (
            result.windows.scanned,
            result.windows.rewritten,
            result.windows.truncated
        ),
        (2, 1, false)
    );
    assert_eq!(store.value(&keys::paid_window("key", 1)), invalid);
    assert_eq!(store.row(&keys::paid_window("key", 1)).expires_at_ms, None);
    assert_eq!(
        store.row(&keys::paid_window("key", 2)).expires_at_ms,
        Some(NOW + 1)
    );
}
#[test]
fn marked_ttl_conflict_stops_without_changing_marker_or_following_row() {
    let store = Store::default();
    let mut marked = request("a", true);
    marked["usage_rollup_at_ms"] = json!(NOW - 5);
    store.put(key("a"), marked.clone());
    store.put(key("b"), request("b", true));
    store.fail(key("a"), false);
    let first = backfill(&store, 10);
    assert_eq!(
        (
            first.requests.failed,
            first.requests.processed,
            first.requests.truncated
        ),
        (1, 0, true)
    );
    assert_eq!(store.value(&key("a")), marked);
    assert!(store.value(&key("b"))["usage_rollup_at_ms"].is_null());
    assert_eq!(backfill(&store, 10).requests.rollups_written, 1);
}
#[test]
fn invalid_clock_and_request_identity_never_mutate_paid_state() {
    let store = Store::default();
    let before = request("a", true);
    store.put(key("a"), before.clone());
    assert!(run(store.authority().backfill_paid_usage(10, -1)).is_err());
    assert_eq!(store.value(&key("a")), before);
    let mut wrong = before;
    wrong["reservation"]["attempt_id"] = json!("foreign");
    store.put(key("a"), wrong.clone());
    assert!(run(store.authority().backfill_paid_usage(10, NOW)).is_err());
    assert_eq!(store.value(&key("a")), wrong);
}
#[test]
fn generated_backfill_rollup_roundtrips_the_existing_live_settlement_dto() -> TestResult {
    let store = Store::default();
    store.put(key("a"), request("a", true));
    assert_eq!(backfill(&store, 10).requests.rollups_written, 1);
    let raw = store.value(&rollup_key("a"));
    let typed: PaidRollup = decode_json(raw.clone())?;
    assert_eq!(typed.provider_quota, 1.5);
    assert_eq!(typed.spend_microcredits, 5.0);
    let restored = serde_json::to_value(typed)?;
    assert_eq!(restored["quota_sum"].as_f64(), raw["quota_sum"].as_f64());
    assert!(restored.get("provider_quota").is_none());
    for field in [
        "model",
        "provider",
        "request_count",
        "input_tokens",
        "cached_input_tokens",
        "output_tokens",
    ] {
        assert_eq!(restored[field], raw[field], "{field}");
    }
    Ok(())
}
