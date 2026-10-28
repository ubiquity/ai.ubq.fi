#![cfg(not(target_arch = "wasm32"))]
type FixtureResult<T> = Result<T, Box<dyn std::error::Error>>;
use gateway_kv_codec::tuple_key::{ScalarKeyPart, TupleKey};

use gateway_core::state::AtomicTransition;
use gateway_core::state::AuthorityStore;
use gateway_core::state::RecordMutation;
use gateway_core::state::RevisionCheck;
use gateway_core::state::StoredRecord;
use gateway_migration_data::custody::ArtifactContext;
use gateway_migration_inverse::inverse::export_latest;
use gateway_migration_inverse::inverse::export_rollback_bundle;
use gateway_migration_prepare::engine::adopt_empty;
use gateway_migration_prepare::engine::prepare;
use gateway_migration_test_support::fixture::Directory;
use gateway_migration_test_support::fixture::run;
use gateway_migration_test_support::runtime::capture;
use gateway_migration_test_support::runtime::fixture;
use gateway_migration_test_support::runtime::raw;
use gateway_migration_test_support::runtime::source;
use gateway_migration_test_support::sqlite_store::SqliteStore;
use gateway_profile_source::codec::from_source_profile as from_profile;
use gateway_profile_values::date::CanonicalDates;
use serde_json::json;

#[test]
fn identity_sidecars_retain_neither_deleted_session_token_nor_provider_secret() -> FixtureResult<()>
{
    use gateway_core::query::QueryStore;
    let directory = Directory::new();
    let store = SqliteStore::open(&directory.file("private-sidecars.sqlite3"));
    let token = "synthetic-session-secret-do-not-retain";
    let input = capture(vec![
        source(
            json!(["uos_ai", "auth", "users", "user-fixture"]),
            json!({"id":"user-fixture","handle":"fixture","is_admin":true,"credential_ids":[],"created_at_ms":1000,"updated_at_ms":2000}),
            "user-source",
            None,
        )?,
        source(
            json!(["uos_ai", "auth", "handles", "fixture"]),
            json!("user-fixture"),
            "handle-source",
            None,
        )?,
        source(
            json!(["uos_ai", "auth", "sessions", token]),
            json!({"token":token,"user_id":"user-fixture","created_at_ms":2000,"expires_at_ms":9000}),
            "session-source",
            Some(9000),
        )?,
    ]);
    run(adopt_empty(&store, &prepare(&input, &CanonicalDates)?))?;
    let session = TupleKey::from_scalar_parts(&[
        ScalarKeyPart::String("uos_ai"),
        ScalarKeyPart::String("auth"),
        ScalarKeyPart::String("sessions"),
        ScalarKeyPart::String(token),
    ])
    .to_record_key();
    let fence = run(gateway_auth_passkeys::session_index::read_current(
        &store,
        &gateway_auth_passkeys::session_index::digest(token),
        2300,
    ))?
    .ok_or("Required fixture row missing")?;
    assert_eq!(fence.fence.session_revision, "session-source");
    assert_eq!(fence.fence.user_id, "user-fixture");
    run(store.commit(AtomicTransition {
        checks: vec![RevisionCheck {
            key: session.clone(),
            expected_revision: Some("session-source".into()),
        }],
        mutations: vec![
            RecordMutation::Delete { key: session },
            RecordMutation::Delete {
                key: gateway_auth_passkeys::session_index::index_key(
                    &gateway_auth_passkeys::session_index::digest(token),
                )?,
            },
        ],
    }))?;
    let metadata = run(store.query(gateway_core::query::QueryRequest {
        selector: gateway_core::query::QuerySelector::Prefix {
            prefix: TupleKey::from_scalar_parts(&[
                ScalarKeyPart::String("uos_ai"),
                ScalarKeyPart::String("migration"),
            ])
            .to_record_key(),
            start: None,
            end: None,
        },
        reverse: false,
        limit: 100,
        cursor: None,
        now_ms: 2300,
    }))?;
    for row in metadata.entries {
        assert!(!serde_json::to_string(&row.record)?.contains(token));
        assert!(
            !row.key.0.contains(
                &token
                    .bytes()
                    .map(|b| format!("{b:02x}"))
                    .collect::<String>()
            )
        );
    }
    let inverse = run(export_latest(&store, 2300))?;
    assert_eq!(inverse.entries.len(), 2);
    assert!(!serde_json::to_string(&inverse)?.contains(token));
    Ok(())
}

#[test]
fn rust_control_nonce_custody_is_separate_and_source_only_export_cannot_drop_it()
-> FixtureResult<()> {
    let directory = Directory::new();
    let store = SqliteStore::open(&directory.file("control-custody.sqlite3"));
    let (input, _) = fixture()?;
    run(adopt_empty(&store, &prepare(&input, &CanonicalDates)?))?;
    let nonce = gateway_auth_native_protocol::native_replay::native_nonce_key();
    let nonce_value =
        json!({"v":1,"nonces":{"fixture-key:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA":9000}});
    store.put(
        nonce.clone(),
        StoredRecord::json("current-replay-fence", nonce_value.clone(), None),
    );
    let owner = gateway_authority_keys::paid_control_owner();
    store.put(owner.clone(),StoredRecord::json("current-owner-revision",json!({"v":1,"owner_id":"fixture-owner","owner_epoch":"fixture-epoch","key_fingerprint":"f".repeat(64)}),None));
    assert_eq!(
        run(export_latest(&store, 2300))
            .err()
            .ok_or("Expected fixture operation refusal")?
            .code(),
        Some("migration_control_custody_required")
    );
    let context = ArtifactContext {
        rust_git_sha: "a".repeat(40),
        rust_git_tree: "b".repeat(40),
        wasm_sha256: "c".repeat(64),
        native_sha256: "d".repeat(64),
        native_nonce_capture_sha256: Some("e".repeat(64)),
    };
    let bundle = run(export_rollback_bundle(&store, 2300, context))?;
    assert_eq!(bundle.rust_controls.len(), 2);
    let replay = bundle
        .rust_controls
        .iter()
        .find(|r| r.key == nonce.0)
        .ok_or("Required fixture value missing")?;
    assert_eq!(replay.record.revision, "current-replay-fence");
    assert_eq!(replay.record.value, nonce_value);
    assert!(replay.record.expires_at_ms.is_none());
    let expected_nonce_label = raw(json!({"type":"string","value":"native_nonce_registry"}))?;
    assert!(
        !bundle
            .source
            .entries
            .iter()
            .any(|e| e.key.get(1) == Some(&expected_nonce_label))
    );
    // No restore API consumes these rows. Current transport ownership/replay must be reconciled externally.
    let rollback = SqliteStore::open(&directory.file("source-only-new-destination.sqlite3"));
    run(adopt_empty(
        &rollback,
        &prepare(&bundle.source, &CanonicalDates)?,
    ))?;
    assert!(run(rollback.read(&nonce))?.is_none());
    assert!(run(rollback.read(&owner))?.is_none());
    Ok(())
}

#[test]
fn inverse_refuses_to_drop_derived_kernel_policy_without_matching_source_defaults()
-> FixtureResult<()> {
    let directory = Directory::new();
    let store = SqliteStore::open(&directory.file("kernel-defaults-drift.sqlite3"));
    let input = capture(vec![source(
        json!(["default", "kernel_policy_limit_requests"]),
        json!(1),
        "source-limit",
        None,
    )?]);
    run(adopt_empty(&store, &prepare(&input, &CanonicalDates)?))?;
    let key = gateway_authority_keys::kernel_defaults();
    let mut changed = run(store.read(&key))?.ok_or("Required fixture row missing")?;
    changed.revision = "independent-runtime-write".into();
    changed.value["usage_limit_requests"] = json!(17);
    store.put(key, changed);
    assert_eq!(
        run(export_latest(&store, 2300))
            .err()
            .ok_or("Expected fixture operation refusal")?
            .code(),
        Some("migration_state_invalid")
    );
    store.put(
        crate::key(json!(["default", "kernel_policy_limit_requests"])),
        StoredRecord::json("matching-current-source", json!(17), None),
    );
    let result = run(export_latest(&store, 2300))?;
    assert_eq!(
        from_profile(&result.entries[0].value)?.to_json_lossless(),
        Some(json!(17))
    );
    assert_eq!(result.entries[0].versionstamp, "matching-current-source");
    Ok(())
}

#[test]
fn inverse_refuses_changed_or_missing_private_mapping_metadata() -> FixtureResult<()> {
    use gateway_core::query::{QueryRequest, QuerySelector, QueryStore};
    let directory = Directory::new();
    let store = SqliteStore::open(&directory.file("mapping-integrity.sqlite3"));
    let input = capture(vec![source(
        json!(["default", "fixture"]),
        json!("retained current data"),
        "original",
        None,
    )?]);
    run(adopt_empty(&store, &prepare(&input, &CanonicalDates)?))?;
    let page = run(store.query(QueryRequest {
        selector: QuerySelector::Prefix {
            prefix: TupleKey::from_scalar_parts(&[
                ScalarKeyPart::String("uos_ai"),
                ScalarKeyPart::String("migration"),
                ScalarKeyPart::String("v3"),
                ScalarKeyPart::String("row"),
            ])
            .to_record_key(),
            start: None,
            end: None,
        },
        reverse: false,
        limit: 10,
        cursor: None,
        now_ms: 2300,
    }))?;
    let mapping = page
        .entries
        .first()
        .ok_or("Required fixture value missing")?;
    let mut corrupt = mapping.record.clone();
    corrupt.value["baseline_digest"] = json!("tampered baseline");
    store.put(mapping.key.clone(), corrupt);
    assert!(run(export_latest(&store, 2300)).is_err());
    store.put(mapping.key.clone(), mapping.record.clone());
    assert!(run(export_latest(&store, 2300)).is_ok());
    run(store.commit(AtomicTransition {
        checks: vec![RevisionCheck {
            key: mapping.key.clone(),
            expected_revision: Some(mapping.record.revision.clone()),
        }],
        mutations: vec![RecordMutation::Delete {
            key: mapping.key.clone(),
        }],
    }))?;
    assert!(run(export_latest(&store, 2300)).is_err());
    assert_eq!(
        run(store.read(
            &TupleKey::from_scalar_parts(&[
                ScalarKeyPart::String("default"),
                ScalarKeyPart::String("fixture")
            ])
            .to_record_key()
        ))?
        .ok_or("Required fixture row missing")?
        .value,
        "retained current data"
    );
    Ok(())
}
