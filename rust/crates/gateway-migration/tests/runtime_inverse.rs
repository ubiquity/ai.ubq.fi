#![cfg(not(target_arch = "wasm32"))]
use gateway_auth::store::key;
use gateway_core::state::AtomicTransition;
use gateway_core::state::RecordMutation;
use gateway_core::state::RevisionCheck;
use gateway_core::state::StoredRecord;
use gateway_migration::engine::adopt_empty;
use gateway_migration::engine::prepare;
use gateway_migration::inverse::export_latest;
use gateway_migration::inverse::export_rollback_bundle;
use gateway_migration_data::custody::ArtifactContext;
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
fn identity_sidecars_retain_neither_deleted_session_token_nor_provider_secret() {
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
        ),
        source(
            json!(["uos_ai", "auth", "handles", "fixture"]),
            json!("user-fixture"),
            "handle-source",
            None,
        ),
        source(
            json!(["uos_ai", "auth", "sessions", token]),
            json!({"token":token,"user_id":"user-fixture","created_at_ms":2000,"expires_at_ms":9000}),
            "session-source",
            Some(9000),
        ),
    ]);
    run(adopt_empty(
        &store,
        &prepare(&input, &CanonicalDates).unwrap(),
    ))
    .unwrap();
    let session = key(json!(["uos_ai", "auth", "sessions", token]));
    let fence = run(gateway_auth::session_index::read_current(
        &store,
        &gateway_auth::session_index::digest(token),
        2300,
    ))
    .unwrap()
    .unwrap();
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
                key: gateway_auth::session_index::index_key(&gateway_auth::session_index::digest(
                    token,
                ))
                .unwrap(),
            },
        ],
    }))
    .unwrap();
    let metadata = run(store.query(gateway_core::query::QueryRequest {
        selector: gateway_core::query::QuerySelector::Prefix {
            prefix: key(json!(["uos_ai", "migration"])),
            start: None,
            end: None,
        },
        reverse: false,
        limit: 100,
        cursor: None,
        now_ms: 2300,
    }))
    .unwrap();
    for row in metadata.entries {
        assert!(!serde_json::to_string(&row.record).unwrap().contains(token));
        assert!(
            !row.key.0.contains(
                &token
                    .bytes()
                    .map(|b| format!("{b:02x}"))
                    .collect::<String>()
            )
        );
    }
    let inverse = run(export_latest(&store, 2300)).unwrap();
    assert_eq!(inverse.entries.len(), 2);
    assert!(!serde_json::to_string(&inverse).unwrap().contains(token));
}

#[test]
fn rust_control_nonce_custody_is_separate_and_source_only_export_cannot_drop_it() {
    let directory = Directory::new();
    let store = SqliteStore::open(&directory.file("control-custody.sqlite3"));
    let (input, _) = fixture();
    run(adopt_empty(
        &store,
        &prepare(&input, &CanonicalDates).unwrap(),
    ))
    .unwrap();
    let nonce = gateway_auth::native_replay::native_nonce_key();
    let nonce_value =
        json!({"v":1,"nonces":{"fixture-key:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA":9000}});
    store.put(
        nonce.clone(),
        StoredRecord::json("current-replay-fence", nonce_value.clone(), None),
    );
    let owner = gateway_authority::keys::paid_control_owner();
    store.put(owner.clone(),StoredRecord::json("current-owner-revision",json!({"v":1,"owner_id":"fixture-owner","owner_epoch":"fixture-epoch","key_fingerprint":"f".repeat(64)}),None));
    assert_eq!(
        run(export_latest(&store, 2300)).unwrap_err().code(),
        Some("migration_control_custody_required")
    );
    let context = ArtifactContext {
        rust_git_sha: "a".repeat(40),
        rust_git_tree: "b".repeat(40),
        wasm_sha256: "c".repeat(64),
        native_sha256: "d".repeat(64),
        native_nonce_capture_sha256: Some("e".repeat(64)),
    };
    let bundle = run(export_rollback_bundle(&store, 2300, context)).unwrap();
    assert_eq!(bundle.rust_controls.len(), 2);
    let replay = bundle
        .rust_controls
        .iter()
        .find(|r| r.key == nonce.0)
        .unwrap();
    assert_eq!(replay.record.revision, "current-replay-fence");
    assert_eq!(replay.record.value, nonce_value);
    assert!(replay.record.expires_at_ms.is_none());
    assert!(!bundle.source.entries.iter().any(|e| e.key.get(1)
        == Some(&raw(
            json!({"type":"string","value":"native_nonce_registry"})
        ))));
    // No restore API consumes these rows. Current transport ownership/replay must be reconciled externally.
    let rollback = SqliteStore::open(&directory.file("source-only-new-destination.sqlite3"));
    run(adopt_empty(
        &rollback,
        &prepare(&bundle.source, &CanonicalDates).unwrap(),
    ))
    .unwrap();
    assert!(run(rollback.read(&nonce)).unwrap().is_none());
    assert!(run(rollback.read(&owner)).unwrap().is_none());
}

#[test]
fn inverse_refuses_to_drop_derived_kernel_policy_without_matching_source_defaults() {
    let directory = Directory::new();
    let store = SqliteStore::open(&directory.file("kernel-defaults-drift.sqlite3"));
    let input = capture(vec![source(
        json!(["default", "kernel_policy_limit_requests"]),
        json!(1),
        "source-limit",
        None,
    )]);
    run(adopt_empty(
        &store,
        &prepare(&input, &CanonicalDates).unwrap(),
    ))
    .unwrap();
    let key = gateway_authority::keys::kernel_defaults();
    let mut changed = run(store.read(&key)).unwrap().unwrap();
    changed.revision = "independent-runtime-write".into();
    changed.value["usage_limit_requests"] = json!(17);
    store.put(key, changed);
    assert_eq!(
        run(export_latest(&store, 2300)).unwrap_err().code(),
        Some("migration_state_invalid")
    );
    store.put(
        crate::key(json!(["default", "kernel_policy_limit_requests"])),
        StoredRecord::json("matching-current-source", json!(17), None),
    );
    let result = run(export_latest(&store, 2300)).unwrap();
    assert_eq!(
        from_profile(&result.entries[0].value)
            .unwrap()
            .to_json_lossless(),
        Some(json!(17))
    );
    assert_eq!(result.entries[0].versionstamp, "matching-current-source");
}

#[test]
fn inverse_refuses_changed_or_missing_private_mapping_metadata() {
    use gateway_core::query::{QueryRequest, QuerySelector, QueryStore};
    let directory = Directory::new();
    let store = SqliteStore::open(&directory.file("mapping-integrity.sqlite3"));
    let input = capture(vec![source(
        json!(["default", "fixture"]),
        json!("retained current data"),
        "original",
        None,
    )]);
    run(adopt_empty(
        &store,
        &prepare(&input, &CanonicalDates).unwrap(),
    ))
    .unwrap();
    let page = run(store.query(QueryRequest {
        selector: QuerySelector::Prefix {
            prefix: key(json!(["uos_ai", "migration", "v3", "row"])),
            start: None,
            end: None,
        },
        reverse: false,
        limit: 10,
        cursor: None,
        now_ms: 2300,
    }))
    .unwrap();
    let mapping = page.entries.first().unwrap();
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
    }))
    .unwrap();
    assert!(run(export_latest(&store, 2300)).is_err());
    assert_eq!(
        run(store.read(&key(json!(["default", "fixture"]))))
            .unwrap()
            .unwrap()
            .value,
        "retained current data"
    );
}
