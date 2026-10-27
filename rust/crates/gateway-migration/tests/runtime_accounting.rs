#![cfg(not(target_arch = "wasm32"))]
use gateway_admin_contracts::operations::AdminHost as _;
use gateway_core::identity::RequestId;
use gateway_core::state::AuthorityStore;
use gateway_migration_data::error::MigrationError;
use gateway_migration_inverse::inverse::export_latest;
use gateway_migration_prepare::engine::adopt_empty;
use gateway_migration_prepare::engine::prepare;
use gateway_migration_test_support::fixture::Directory;
use gateway_migration_test_support::fixture::run;
use gateway_migration_test_support::runtime::AdminHost;
use gateway_migration_test_support::runtime::Capabilities;
use gateway_migration_test_support::runtime::Clock;
use gateway_migration_test_support::runtime::Tokens;
use gateway_migration_test_support::runtime::admin_input;
use gateway_migration_test_support::runtime::capture;
use gateway_migration_test_support::runtime::fixture;
use gateway_migration_test_support::runtime::paid_exposure_capture;
use gateway_migration_test_support::runtime::raw;
use gateway_migration_test_support::runtime::source;
use gateway_migration_test_support::sqlite_store::SqliteStore;
use gateway_profile_source::codec::from_source_profile as from_profile;
use gateway_profile_values::date::CanonicalDates;
use serde_json::Value;
use serde_json::json;
use std::sync::atomic::AtomicU64;

#[test]
fn pending_paid_exposure_and_rollup_marker_survive_restart_and_deferral_inverse()
-> Result<(), Box<dyn std::error::Error>> {
    let directory = Directory::new();
    let path = directory.file("paid.sqlite3");
    let input = paid_exposure_capture();
    let mut input = input;
    let (keys, _) = fixture();
    input.entries.extend(keys.entries.into_iter().take(2));
    let store = SqliteStore::open(&path);
    run(adopt_empty(&store, &prepare(&input, &CanonicalDates)?))?;
    drop(store);
    let store = SqliteStore::open(&path);
    let authority = gateway_authority::store::Authority::with_clock(
        store.clone(),
        Tokens(AtomicU64::new(1)),
        Clock,
    );
    let due = run(authority.due_paid(2300, 10))?;
    assert_eq!(due.len(), 1);
    assert_eq!(due[0].attempt_id.0, "pending-attempt");
    let row = run(store.read(&gateway_authority::keys::paid_attempt(
        "fixture-key",
        "pending-attempt",
    )))?
    .ok_or("Expected retained fixture row")?;
    let row: gateway_authority::paid_types::PaidAttempt = serde_json::from_value(row.value)?;
    assert_eq!(row.reservation.request_id.0, "pending-attempt");
    assert_eq!(row.reservation.attempt_id.0, "pending-attempt");
    assert!(row.spend_microcredits.is_none());
    assert!(!row.transport_started);
    assert_eq!(
        run(authority.defer_paid(&row.reservation, 2300))?,
        Some(5000)
    );
    let result = run(export_latest(&store, 2300))?;
    let mut next = None;
    for e in &result.entries {
        if e.key.last() == Some(&raw(json!({"type":"string","value":"pending-attempt"})))
            && from_profile(&e.value)?
                .to_json_lossless()
                .ok_or("Expected lossless fixture JSON")?
                .get("billing_state")
                .is_some()
        {
            next = Some(e);
            break;
        }
    }
    let next = next.ok_or("Expected pending paid source row")?;
    let next_value = from_profile(&next.value)?
        .to_json_lossless()
        .ok_or("Expected lossless fixture JSON")?;
    assert_eq!(next_value["billing_state"], "unresolved");
    assert_eq!(next_value["spend_microcredits"], Value::Null);
    assert_eq!(next_value["reconciliation_attempts"], 1);
    assert!(next_value.get("reservation").is_none());
    let settled = result
        .entries
        .iter()
        .find(|e| e.versionstamp == "settled-attempt-source")
        .ok_or("Expected settled source entry")?;
    assert_eq!(settled.value, input.entries[2].value);
    let rollback = SqliteStore::open(&directory.file("paid-inverse.sqlite3"));
    run(adopt_empty(&rollback, &prepare(&result, &CanonicalDates)?))?;
    let next = run(rollback.read(&gateway_authority::keys::paid_pending(
        "fixture-key",
        "pending-attempt",
    )))?
    .ok_or("Expected retained fixture row")?;
    assert_eq!(next.value["next_reconciliation_at_ms"], 7300);
    Ok(())
}

#[test]
fn actual_admin_reads_and_writes_adopted_state_then_inverse_uses_new_revision()
-> Result<(), Box<dyn std::error::Error>> {
    let directory = Directory::new();
    let path = directory.file("admin.sqlite3");
    let (input, _) = fixture();
    let store = SqliteStore::open(&path);
    run(adopt_empty(&store, &prepare(&input, &CanonicalDates)?))?;
    drop(store);
    let store = SqliteStore::open(&path);
    let host = crate::AdminHost;
    let capabilities = Capabilities;
    let config = gateway_admin_runtime::RuntimeConfig::default();
    let runtime = gateway_admin_runtime::AdminRuntime {
        store: &store,
        host: &host,
        capabilities: &capabilities,
        config: &config,
    };
    let response = run(runtime.execute(
        gateway_admin_contracts::routes::AdminOperation::DebugGet,
        admin_input(Value::Null)?,
    ))?;
    assert_eq!(
        response
            .body
            .as_json()
            .ok_or("Expected fixture JSON body")?["routing"]["scenario"],
        "codex_429"
    );
    run(runtime.execute(
        gateway_admin_contracts::routes::AdminOperation::DebugSet,
        admin_input(json!({"scenario":"normal","duration_ms":0}))?,
    ))?;
    let inverse = run(export_latest(&store, 2300))?;
    let row = inverse
        .entries
        .iter()
        .find(|e| e.key == input.entries[5].key)
        .ok_or("Expected admin source entry")?;
    assert_ne!(row.versionstamp, "debug-original");
    assert_eq!(
        from_profile(&row.value)?
            .to_json_lossless()
            .ok_or("Expected lossless fixture JSON")?["scenario"],
        "normal"
    );
    Ok(())
}

#[test]
fn source_kernel_defaults_feed_actual_admission_and_new_org_rows_invert()
-> Result<(), Box<dyn std::error::Error>> {
    let directory = Directory::new();
    let store = SqliteStore::open(&directory.file("kernel-defaults.sqlite3"));
    let input = capture(vec![
        source(
            json!(["default", "kernel_policy_limit_requests"]),
            json!(1),
            "default-limit",
            None,
        ),
        source(
            json!(["default", "kernel_policy_window_ms"]),
            json!(1000),
            "default-window",
            None,
        ),
    ]);
    run(adopt_empty(&store, &prepare(&input, &CanonicalDates)?))?;
    let authority = gateway_authority::store::Authority::with_clock(
        store.clone(),
        Tokens(AtomicU64::new(1)),
        Clock,
    );
    let reservation = run(authority.reserve_kernel(
        "fixture-owner",
        "fixture-repo",
        &RequestId("one-kernel-request".into()),
        "responses",
        2300,
    ))?;
    assert!(reservation.repo.is_none());
    assert_eq!(
        run(authority.reserve_kernel(
            "fixture-owner",
            "fixture-repo",
            &RequestId("two-kernel-request".into()),
            "responses",
            2300
        ))
        .err()
        .ok_or("Expected kernel quota refusal")?
        .status,
        429
    );
    let generated_window = run(store.read(&gateway_authority::keys::kernel_window(
        "fixture-owner",
        None,
    )))?
    .ok_or("Expected retained fixture row")?;
    let generated = generated_window.json_value()?;
    assert_eq!(generated["reserved_requests"], 1);
    assert_eq!(
        generated["reservations"]
            .as_object()
            .ok_or("Expected fixture object")?
            .len(),
        1
    );
    let inverse = run(export_latest(&store, 2300))?;
    let window = inverse
        .entries
        .iter()
        .find(|e| e.key.get(3) == Some(&raw(json!({"type":"string","value":"org_window"}))))
        .ok_or("Expected org window source entry")?;
    let value = from_profile(&window.value)?
        .to_json_lossless()
        .ok_or("Expected lossless fixture JSON")?;
    assert_eq!(value["scope"], "org");
    assert!(value.get("repo").is_none());
    assert_eq!(value["reserved_requests"], 1);
    assert!(value.get("reservations").is_none());
    assert_eq!(window.versionstamp, generated_window.revision);
    assert_eq!(window.expires_at_ms, generated_window.expires_at_ms);
    let mut missing_reservation = inverse.clone();
    missing_reservation.entries.retain(|entry| {
        entry.key.get(3) != Some(&raw(json!({"type":"string","value":"org_reservation"})))
    });
    assert!(matches!(
        prepare(&missing_reservation, &CanonicalDates),
        Err(MigrationError::SourceInventory(_))
    ));
    let rollback = SqliteStore::open(&directory.file("kernel-inverse-new.sqlite3"));
    run(adopt_empty(&rollback, &prepare(&inverse, &CanonicalDates)?))?;
    Ok(())
}
