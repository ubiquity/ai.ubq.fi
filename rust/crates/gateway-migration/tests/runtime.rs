#![cfg(not(target_arch = "wasm32"))]
use gateway_auth::store::key;
use gateway_core::error::GatewayError;
use gateway_core::identity::ApiKeyId;
use gateway_core::identity::RequestId;
use gateway_core::state::AtomicTransition;
use gateway_core::state::RecordMutation;
use gateway_core::state::RevisionCheck;
use gateway_core::state::StoredRecord;
use gateway_core::value::PortableValue;
use gateway_migration_data::capture::PrivateCapture;
use gateway_migration_data::capture::SourceEntry;
use gateway_migration_inverse::inverse::export_latest;
use gateway_migration_prepare::engine::adopt_empty;
use gateway_migration_prepare::engine::prepare;
use gateway_migration_projection::projection::known_source_family;
use gateway_migration_test_support::fixture::Directory;
use gateway_migration_test_support::fixture::run;
use gateway_migration_test_support::runtime::Clock;
use gateway_migration_test_support::runtime::SOURCE;
use gateway_migration_test_support::runtime::Tokens;
use gateway_migration_test_support::runtime::capture;
use gateway_migration_test_support::runtime::fixture;
use gateway_migration_test_support::runtime::raw;
use gateway_migration_test_support::runtime::source;
use gateway_migration_test_support::sqlite_store::SqliteStore;
use gateway_profile_source::codec::from_source_profile as from_profile;
use gateway_profile_source::codec::to_source_profile as to_profile;
use gateway_profile_values::date::CanonicalDates;
use gateway_profile_values::date::SourceDateHost;
use serde_json::Value;
use serde_json::json;
use std::sync::atomic::AtomicU64;

#[test]
fn actual_sqlite_adoption_restart_auth_admission_mutation_and_latest_inverse()
-> Result<(), Box<dyn std::error::Error>> {
    let directory = Directory::new();
    let path = directory.file("one-authority.sqlite3");
    let (original, token) = fixture();
    let prepared = prepare(&original, &CanonicalDates)?;
    let store = SqliteStore::open(&path);
    let receipt = run(adopt_empty(&store, &prepared))?;
    assert_eq!(receipt.source_rows, 6);
    drop(store);
    let store = SqliteStore::open(&path);
    let policy = run(gateway_auth::api_keys::authenticate(&store, &token, 2300))?;
    assert_eq!(policy.key_id, "fixture-key");
    let authority = gateway_authority::store::Authority::with_clock(
        store.clone(),
        Tokens(AtomicU64::new(1)),
        Clock,
    );
    assert!(
        run(authority.reserve_api_key(
            &ApiKeyId("fixture-key".into()),
            &RequestId("old-dispatched".into()),
            "responses",
            2300
        ))
        .is_err()
    );
    let consumed = gateway_authority::quota::ApiReservation {
        key_id: ApiKeyId("fixture-key".into()),
        request_id: RequestId("old-dispatched".into()),
        route: "responses".into(),
        policy_version: "v3:1000".into(),
        window_start_ms: 2000,
        window_reset_at_ms: 3000,
        deferred: false,
    };
    assert!(!run(authority.release_api_key(
        &consumed,
        false,
        "unknown source transport",
        2300
    ))?);
    assert_eq!(
        run(store.read(&gateway_authority::keys::api_request(
            "fixture-key",
            "v3:1000",
            2000,
            "old-dispatched"
        )))?
        .ok_or("Expected retained fixture row")?
        .value["state"],
        "dispatched"
    );
    let reservation = run(authority.reserve_api_key(
        &ApiKeyId("fixture-key".into()),
        &RequestId("new-request".into()),
        "responses",
        2300,
    ))?;
    let old = run(store.read(&gateway_authority::keys::api_request(
        "fixture-key",
        "v3:1000",
        2000,
        "old-reserved",
    )))?
    .ok_or("Expected retained fixture row")?;
    assert_eq!(old.value["state"], "released");
    let current = run(store.read(&gateway_authority::keys::api_request(
        "fixture-key",
        "v3:1000",
        2000,
        "new-request",
    )))?
    .ok_or("Expected retained fixture row")?;
    assert_eq!(current.value["state"], "reserved");
    assert_eq!(reservation.request_id.0, "new-request");
    drop(authority);
    // Real same-authority write after restart; deleting an adopted row must not resurrect it.
    let debug = key(json!(["uos_ai", "debug_routing", "v1"]));
    let old = run(store.read(&debug))?.ok_or("Expected retained fixture row")?;
    run(store.commit(AtomicTransition {
        checks: vec![RevisionCheck {
            key: debug.clone(),
            expected_revision: Some(old.revision),
        }],
        mutations: vec![RecordMutation::Delete { key: debug }],
    }))?;
    let exported = run(export_latest(&store, 2300))?;
    assert!(
        !exported
            .entries
            .iter()
            .any(|e| e.versionstamp == "debug-original")
    );
    let request = exported
        .entries
        .iter()
        .find(|e| e.key.last() == Some(&raw(json!({"type":"string","value":"new-request"}))))
        .ok_or("Expected latest request source entry")?;
    let value = from_profile(&request.value)?
        .to_json_lossless()
        .ok_or("Expected lossless fixture JSON")?;
    assert_eq!(value["request_id"], "new-request");
    assert!(value.get("transport_started").is_none());
    assert_eq!(request.expires_at_ms, current.expires_at_ms);
    assert_ne!(request.versionstamp, "reserved-original");
    let rollback = SqliteStore::open(&directory.file("inverse-new-destination.sqlite3"));
    let replay = prepare(&exported, &CanonicalDates)?;
    run(adopt_empty(&rollback, &replay))?;
    assert!(
        run(gateway_auth::api_keys::authenticate(
            &rollback, &token, 2300
        ))
        .is_ok()
    );
    let again = run(export_latest(&rollback, 2300))?;
    assert_eq!(
        serde_json::to_value(&exported)?,
        serde_json::to_value(&again)?
    );
    Ok(())
}

#[test]
fn adoption_conflict_never_partially_writes_and_keeps_foreign_state() {
    let directory = Directory::new();
    let store = SqliteStore::open(&directory.file("conflict.sqlite3"));
    let (capture, _) = fixture();
    let prepared = prepare(&capture, &CanonicalDates).unwrap();
    let foreign_key = key(json!(["ubq_ai", "api_keys", "id", "fixture-key"]));
    *store.before_commit.borrow_mut() = Some((
        foreign_key.clone(),
        StoredRecord::json("foreign-owner", json!({"preserved":true}), None),
    ));
    assert!(run(adopt_empty(&store, &prepared)).is_err());
    assert_eq!(
        run(store.read(&foreign_key)).unwrap().unwrap().revision,
        "foreign-owner"
    );
    assert!(
        run(store.read(&key(json!(["uos_ai", "migration", "v3", "manifest"]))))
            .unwrap()
            .is_none()
    );
    assert!(
        run(store.read(&gateway_authority::keys::api_policy("fixture-key")))
            .unwrap()
            .is_none()
    );
}

#[test]
fn typed_values_revision_expiry_and_deleted_rows_survive_inverse() {
    let directory = Directory::new();
    let store = SqliteStore::open(&directory.file("typed.sqlite3"));
    let value = PortableValue::Map(vec![(
        PortableValue::BigInt("123456789012345678901234567890".into()),
        PortableValue::Set(vec![
            PortableValue::number(-0.0),
            PortableValue::Date {
                epoch_ms: Some(12345),
            },
        ]),
    )]);
    let mut row = source(
        json!(["default", "typed-fixture"]),
        Value::Null,
        "typed-source-revision",
        Some(9900),
    );
    row.value = to_profile(&value).unwrap();
    let input = capture(vec![row]);
    run(adopt_empty(
        &store,
        &prepare(&input, &CanonicalDates).unwrap(),
    ))
    .unwrap();
    drop(store);
    let store = SqliteStore::open(&directory.file("typed.sqlite3"));
    let result = run(export_latest(&store, 2300)).unwrap();
    assert_eq!(result.entries[0].versionstamp, "typed-source-revision");
    assert_eq!(result.entries[0].expires_at_ms, Some(9900));
    assert_eq!(
        from_profile(&result.entries[0].value).unwrap(),
        from_profile(&input.entries[0].value).unwrap()
    );
}

#[test]
fn wrong_indexes_unknown_state_and_duplicates_refuse_before_writes() {
    let (mut input, _) = fixture();
    let mut bad: Value = input.entries[2].value.deserialize().unwrap();
    bad["value"]["reserved_requests"] = json!({"type":"number","value":2});
    input.entries[2].value = raw(bad);
    assert!(prepare(&input, &CanonicalDates).is_err());
    let (mut input, _) = fixture();
    input.entries.push(input.entries[0].clone());
    assert!(prepare(&input, &CanonicalDates).is_err());
    let input = capture(vec![source(
        json!(["unknown", "arbitrary"]),
        json!({}),
        "x",
        None,
    )]);
    assert!(prepare(&input, &CanonicalDates).is_err());
}

#[test]
fn captured_uint8_catalog_is_usable_after_sqlite_restart_and_inverts_exactly()
-> Result<(), Box<dyn std::error::Error>> {
    use gateway_schema::{
        catalog_cache::prepare_catalog,
        catalog_store::{chunk_key, load_catalog, metadata_key},
    };
    let directory = Directory::new();
    let path = directory.file("catalog.sqlite3");
    let catalog = prepare_catalog(
        gateway_schema::catalog_cache::CatalogIdentity {
            client_version: "0.163.0",
            auth_generation: "auth-fixture",
            body_generation: "body-fixture",
        },
        r#"{"models":[{"slug":"gpt-fixture","display_name":"Fixture"}]}"#,
        Some("etag-fixture"),
        Some("application/json"),
        2100,
        &Default::default(),
    )?;
    let mut rows = vec![source(
        json!(["ubq_ai", "codex_catalog", "0.163.0"]),
        serde_json::to_value(&catalog.metadata)?,
        "catalog-meta-source",
        Some(9999),
    )];
    for (is_source, chunks) in [(false, &catalog.chunks), (true, &catalog.source_chunks)] {
        for (index, bytes) in chunks.iter().enumerate() {
            let key = chunk_key("0.163.0", "body-fixture", index, is_source);
            let decoded =
                gateway_core::tuple_key::TupleKey::from_record_key(&key)?.decoded_parts()?;
            let parts = Value::Array(
                decoded
                    .into_iter()
                    .map(|p| match p {
                        gateway_core::tuple_key::DecodedKeyPart::String(v) => {
                            String::from_utf8(v).map(|v| json!(v))
                        }
                        gateway_core::tuple_key::DecodedKeyPart::Number(v) => Ok(json!(v)),
                        _ => panic!("fixture key"),
                    })
                    .collect::<Result<Vec<_>, _>>()?,
            );
            let mut row = source(
                parts,
                Value::Null,
                &format!("catalog-chunk-{is_source}-{index}"),
                Some(9999),
            );
            row.value = to_profile(&PortableValue::Binary {
                kind: "Uint8Array".into(),
                bytes: bytes.clone(),
                byte_offset: 0,
                byte_length: bytes.len() as u64,
            })?;
            rows.push(row);
        }
    }
    let input = capture(rows);
    let store = SqliteStore::open(&path);
    run(adopt_empty(&store, &prepare(&input, &CanonicalDates)?))?;
    drop(store);
    let store = SqliteStore::open(&path);
    let loaded = run(load_catalog(&store, "0.163.0", "auth-fixture", 2300))?
        .ok_or("Expected retained fixture row")?;
    let parsed: Value = loaded.parsed.deserialize()?;
    assert_eq!(parsed["models"][0]["slug"], "gpt-fixture");
    assert!(loaded.source_body.is_some());
    assert!(
        run(load_catalog(
            &store,
            "0.163.0",
            "wrong-current-auth-generation",
            2300
        ))?
        .is_none()
    );
    let metadata =
        run(store.read(&metadata_key("0.163.0")))?.ok_or("Expected retained fixture row")?;
    assert_eq!(metadata.revision, "catalog-meta-source");
    let result = run(export_latest(&store, 2300))?;
    for original in &input.entries {
        let restored = result
            .entries
            .iter()
            .find(|e| e.key == original.key)
            .ok_or("Expected original catalog entry")?;
        assert_eq!(restored.value, original.value);
        assert_eq!(restored.expires_at_ms, original.expires_at_ms);
        assert_eq!(restored.versionstamp, original.versionstamp);
    }
    Ok(())
}

#[test]
fn source_date_host_is_explicit_and_nested_values_keep_the_selected_realm()
-> Result<(), Box<dyn std::error::Error>> {
    struct RecordedRealm(Value);
    impl SourceDateHost for RecordedRealm {
        fn date_value(&self, input: Option<&Value>) -> Result<Option<i64>, GatewayError> {
            let text = input.and_then(Value::as_str).ok_or_else(|| {
                GatewayError::unavailable(
                    "Date input not in source oracle",
                    "source_date_host_missing",
                )
            })?;
            let row = self.0["dates"]
                .as_array()
                .and_then(|dates| dates.iter().find(|r| r["input"] == text))
                .ok_or_else(|| {
                    GatewayError::unavailable(
                        "Date source realm unavailable",
                        "source_date_host_missing",
                    )
                })?;
            Ok(row["epochMs"].as_i64())
        }
    }
    let utc: Value = serde_json::from_str(include_str!("fixtures/normalization-utc.json"))?;
    let ny: Value = serde_json::from_str(include_str!("fixtures/normalization-new-york.json"))?;
    let input = json!({"type":"object","value":{"when":{"type":"Array","value":[{"type":"Date","value":"2024-01-02T03:04:05"}]}}});
    assert!(gateway_profile_values::codec::from_profile(&input).is_err());
    let first = gateway_profile_values::codec::from_profile_with_host(&input, &RecordedRealm(utc))?;
    let second = gateway_profile_values::codec::from_profile_with_host(&input, &RecordedRealm(ny))?;
    assert_ne!(first, second);
    let original = from_profile(&to_profile(&first)?)?;
    assert_eq!(first, original);
    Ok(())
}

#[test]
fn source_shaped_routing_capacity_keep_timeout_legacy_probe_and_named_limits_as_data() {
    let directory = Directory::new();
    let path = directory.file("source-routing.sqlite3");
    let routing = json!({"v":2,"updated_at_ms":2100,"banked_reset_legacy_identity_unresolved":true,"slots":[{"account_id_hash":null,"credential_version":"fixture-generation","quota_blocked_until_ms":9000,"quota_block_source":"header_retry_after","quota_blocked_classes":["spark"],"quota_blocks_by_class":{"spark":{"blocked_until_ms":9000,"source":"header_retry_after","legacy_fallback":true,"quota_signal_observed_at_ms":2000,"observed_reset_at_ms":9000,"observed_reset_at_is_stable":false,"banked_reset_generation_ambiguous":true,"banked_reset_recovery_probe_pending":true}},"invalid_credential_version":null,"primary_used_percent":100,"secondary_used_percent":20,"quota_signal_observed_at_ms":2000,"capacity_observed_at_ms":2050,"upstream_timeout_blocked_until_ms":8000,"observed_reset_at_ms":9000,"observed_reset_at_is_stable":false,"banked_reset_generation_ambiguous":true,"banked_reset_recovery_probe_pending":true,"generation":3,"probe_lease":{"token":"fixture-probe","expires_at_ms":7000,"generation":3,"circuit":"upstream_timeout","quota_class":null}}]});
    let capacity = json!({"v":1,"updated_at_ms":2100,"observations":[{"slot":0,"account_id_hash":"fixture-hash","state":"stale","source_observed_at_ms":1990,"snapshot_at_ms":2000,"windows":{"primary":{"limit_window_seconds":18000,"used_percent":100,"reset_at_ms":9000},"secondary":null},"additional_rate_limits":[{"limit_name":"GPT-5.3-Codex-Spark","metered_feature":"spark","windows":{"primary":{"limit_window_seconds":300,"used_percent":23,"reset_at_ms":8000},"secondary":null}}]}]});
    let input = capture(vec![
        source(
            json!(["uos_ai", "codex_account_routing", "v2"]),
            routing.clone(),
            "routing-source",
            None,
        ),
        source(
            json!(["uos_ai", "codex_capacity_routing", "v1", "observations"]),
            capacity.clone(),
            "capacity-source",
            None,
        ),
    ]);
    let store = SqliteStore::open(&path);
    run(adopt_empty(
        &store,
        &prepare(&input, &CanonicalDates).unwrap(),
    ))
    .unwrap();
    drop(store);
    let store = SqliteStore::open(&path);
    assert_eq!(
        run(store.read(&gateway_authority::keys::routing()))
            .unwrap()
            .unwrap()
            .value,
        routing
    );
    assert_eq!(
        run(store.read(&gateway_authority::keys::capacity()))
            .unwrap()
            .unwrap()
            .value,
        capacity
    );
    // Data adoption never creates an enabled normalized pool or current publication proof.
    assert!(
        run(store.read(&gateway_authority::keys::pool()))
            .unwrap()
            .is_none()
    );
    let result = run(export_latest(&store, 2300)).unwrap();
    for original in &input.entries {
        let actual = result
            .entries
            .iter()
            .find(|e| e.key == original.key)
            .unwrap();
        assert_eq!(actual.value, original.value);
        assert_eq!(actual.versionstamp, original.versionstamp);
    }
}

#[test]
fn reviewed_complete_source_key_inventory_has_no_silent_unknown_omissions() {
    let inventory: Value =
        serde_json::from_str(include_str!("../source-key-inventory.json")).unwrap();
    assert_eq!(inventory["source_git_sha"], SOURCE);
    for family in inventory["families"].as_array().unwrap() {
        let mut parts = vec![family["namespace"].clone()];
        if family["family"] != "*" {
            parts.push(family["family"].clone());
        }
        assert!(
            known_source_family(&parts),
            "unmapped reviewed source family {parts:?}"
        );
    }
    assert!(!known_source_family(&[
        json!("uos_ai"),
        json!("invented_new_authority")
    ]));
}

#[test]
fn identity_mapping_reconstructs_all_source_supported_typed_tuple_parts()
-> Result<(), Box<dyn std::error::Error>> {
    let directory = Directory::new();
    let store = SqliteStore::open(&directory.file("typed-key.sqlite3"));
    let entry = SourceEntry {
        key: vec![
            json!({"type":"string","value":"default"}),
            json!({"type":"string","value":"typed-key"}),
            json!({"type":"boolean","value":true}),
            json!({"type":"bigint","value":"-123456789012345678901234567890"}),
            json!({"type":"Uint8Array","value":"AAH_AA","byteLength":4}),
            json!({"type":"number","value":"-0"}),
        ]
        .into_iter()
        .map(raw)
        .collect(),
        value: raw(json!({"type":"string","value":"typed tuple payload"})),
        versionstamp: "typed-key-source".into(),
        expires_at_ms: Some(9000),
    };
    let input = capture(vec![entry]);
    run(adopt_empty(&store, &prepare(&input, &CanonicalDates)?))?;
    let output = run(export_latest(&store, 2300))?;
    let before = gateway_profile_source::line::normalize_import_line(&serde_json::to_string(
        &input.entries[0],
    )?)?
    .ok_or("Expected normalized typed tuple fixture")?;
    let after = gateway_profile_source::line::normalize_import_line(&serde_json::to_string(
        &output.entries[0],
    )?)?
    .ok_or("Expected normalized typed tuple fixture")?;
    assert_eq!(before.key, after.key);
    assert_eq!(before.value, after.value);
    assert_eq!(output.entries[0].versionstamp, "typed-key-source");
    Ok(())
}

#[test]
fn a_profile_row_without_explicit_expiry_is_not_a_full_capture() {
    let (capture, _) = fixture();
    let mut json = serde_json::to_value(&capture).unwrap();
    json["entries"][0]
        .as_object_mut()
        .unwrap()
        .remove("expires_at_ms");
    assert!(
        serde_json::from_str::<PrivateCapture>(&serde_json::to_string(&json).unwrap()).is_err()
    );
}
