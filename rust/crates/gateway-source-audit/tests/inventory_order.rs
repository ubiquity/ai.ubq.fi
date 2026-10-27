use gateway_admin_data::inventory::AuditInventory;
use gateway_core::{error::GatewayError, state::StoredRecord};
use gateway_migration_ledger::rows::ObservedRow;
use gateway_source_audit::driver::{inspect_inventory, inspect_source_rows};
use serde_json::{Value, json};
use std::cell::{Cell, RefCell};

type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;

struct Replay<'a> {
    rows: &'a [ObservedRow],
    visits: Cell<usize>,
    trace: RefCell<Vec<(usize, usize)>>,
    fail_at_eof: Option<usize>,
}
impl<'a> Replay<'a> {
    fn new(rows: &'a [ObservedRow], fail_at_eof: Option<usize>) -> Self {
        Self {
            rows,
            visits: Cell::new(0),
            trace: RefCell::new(Vec::new()),
            fail_at_eof,
        }
    }
}
impl AuditInventory for Replay<'_> {
    fn visit(
        &self,
        accept: &mut dyn FnMut(&ObservedRow) -> Result<(), GatewayError>,
    ) -> Result<(), GatewayError> {
        let pass = self.visits.get() + 1;
        self.visits.set(pass);
        for (index, row) in self.rows.iter().enumerate() {
            self.trace.borrow_mut().push((pass, index));
            accept(row)?;
        }
        if self.fail_at_eof == Some(pass) {
            return Err(GatewayError::unavailable(
                format!("fixture EOF {pass}"),
                "fixture_eof",
            ));
        }
        Ok(())
    }
}
fn row(parts: Value, value: Value) -> TestResult<ObservedRow> {
    Ok(ObservedRow::json(
        parts.as_array().ok_or("Expected fixture tuple")?.clone(),
        StoredRecord::json("fixture-revision", value, Some(50_000)),
    )?)
}
fn fixture() -> TestResult<Vec<ObservedRow>> {
    let hash = "a".repeat(43);
    let policy = json!({"id":"fixture-key","expires_at_ms":-1,"revoked_at_ms":null,"usage_limit_requests":-1,"usage_requests":0,"usage_reset_at_ms":10000,"window_ms":1000,"usage_quota_version":3,"paid_fallback_enabled":false,"paid_fallback_limit_microcredits":-1,"paid_fallback_spent_microcredits":0,"paid_fallback_reserved_microcredits":0,"paid_fallback_reservation_request_id":null});
    let mut id = policy.clone();
    let fields = id.as_object_mut().ok_or("Expected fixture policy")?;
    fields.insert("hash".into(), json!(hash));
    fields.insert("prefix".into(), json!("u_0123456789"));
    fields.insert("paid_fallback_model_ids".into(), json!(["fixture-model"]));
    fields.insert("paid_fallback_quota_per_credit".into(), json!(0));
    fields.insert("paid_fallback_pricing_checked_at_ms".into(), Value::Null);
    fields.insert("paid_fallback_max_exposure_microcredits".into(), json!({}));
    Ok(vec![
        row(json!(["ubq_ai", "api_keys", "id", "fixture-key"]), id)?,
        row(json!(["ubq_ai", "api_keys", "hash", hash]), policy)?,
        row(
            json!([
                "uos_ai",
                "api_key_usage",
                "v3",
                "window",
                "fixture-key",
                "v3:1000",
                0
            ]),
            json!({}),
        )?,
        row(
            json!([
                "uos_ai",
                "kernel_quota",
                "v2",
                "repo_policy",
                "owner",
                "repo"
            ]),
            json!({}),
        )?,
        row(
            json!([
                "uos_ai",
                "paid_fallback",
                "v3",
                "request",
                "fixture-key",
                "fixture-request"
            ]),
            json!({}),
        )?,
        row(
            json!([
                "uos_ai",
                "codex_reset_usage",
                "account",
                "v1",
                "fixture-key"
            ]),
            json!({}),
        )?,
        row(json!(["default", "model"]), json!("missing-model"))?,
        row(
            json!(["ubq_ai", "codex_models"]),
            json!({"models":[{"slug":"another-model"}]}),
        )?,
    ])
}
#[test]
fn complete_coupled_audit_preserves_original_passes_counts_and_report_error_order() -> TestResult {
    let rows = fixture()?;
    let before = rows
        .iter()
        .map(|row| row.record.clone())
        .collect::<Vec<_>>();
    let replay = Replay::new(&rows, None);
    let actual = inspect_inventory(&replay, 1000)?;
    let slice = inspect_source_rows(&rows, true, 1000)?;
    assert_eq!(actual.errors, slice.errors);
    let errors = actual
        .errors
        .iter()
        .map(|error| error.try_utf8())
        .collect::<Result<Vec<_>, _>>()?;
    let prefixes = [
        "API key usage V3 window is malformed:",
        "kernel quota V2 policy is malformed:",
        "paid fallback V3 request is malformed:",
        "codex reset usage record is malformed:",
        "default model is not present in codex model snapshot:",
    ];
    assert_eq!(errors.len(), prefixes.len());
    for (error, prefix) in errors.iter().zip(prefixes) {
        assert!(error.starts_with(prefix), "{error}");
    }
    for name in [
        "api_key_ids",
        "api_key_hashes",
        "api_key_usage_v3_windows",
        "kernel_v2_repo_policys",
        "paid_fallback_v3_requests",
        "codex_reset_usage",
    ] {
        assert_eq!(actual.counts.get(name), Some(&1), "{name}");
        assert_eq!(actual.counts.get(name), slice.counts.get(name), "{name}");
    }
    assert_eq!(replay.visits.get(), 3);
    let mut trace = (0..rows.len()).map(|i| (1, i)).collect::<Vec<_>>();
    trace.push((2, 0));
    trace.extend((0..rows.len()).map(|i| (3, i)));
    trace.extend((1..rows.len()).map(|i| (2, i)));
    assert_eq!(*replay.trace.borrow(), trace);
    assert_eq!(
        rows.iter()
            .map(|row| row.record.clone())
            .collect::<Vec<_>>(),
        before
    );
    Ok(())
}
#[test]
fn first_second_and_nested_eof_failures_never_publish_a_partial_audit() -> TestResult {
    let rows = fixture()?;
    for pass in 1..=3 {
        let replay = Replay::new(&rows, Some(pass));
        let error = inspect_inventory(&replay, 1000)
            .err()
            .ok_or("Expected complete EOF refusal")?;
        assert_eq!(error.envelope.error.code.as_deref(), Some("fixture_eof"));
        assert_eq!(error.envelope.error.message, format!("fixture EOF {pass}"));
    }
    Ok(())
}
#[test]
fn every_deferred_error_waits_for_complete_key_visits_and_yields_to_eof_failure() -> TestResult {
    for index in 2..=6 {
        let mut rows = fixture()?;
        rows[index].record.schema_version = 0;
        let replay = Replay::new(&rows, None);
        let error = inspect_inventory(&replay, 1000)
            .err()
            .ok_or("Expected deferred source refusal")?;
        assert_eq!(
            error.envelope.error.code.as_deref(),
            Some("authority_state_version_unsupported")
        );
        assert_eq!(replay.visits.get(), 3);
        for pass in [1, 2, 3] {
            let replay = Replay::new(&rows, Some(pass));
            let error = inspect_inventory(&replay, 1000)
                .err()
                .ok_or("Expected EOF before deferred finish")?;
            assert_eq!(error.envelope.error.code.as_deref(), Some("fixture_eof"));
            assert_eq!(error.envelope.error.message, format!("fixture EOF {pass}"));
        }
    }
    Ok(())
}
