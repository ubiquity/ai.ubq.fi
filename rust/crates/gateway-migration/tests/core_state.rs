#![cfg(not(target_arch = "wasm32"))]
use gateway_auth::store::key;
use gateway_core::{state::AuthorityStore, tuple_key::TupleKey};
use gateway_migration_core_state::CORE_STATE_DO_ROW_WRITE_LIMIT;
use gateway_migration_data::native_capture::NativeCapture;
use gateway_migration_data::native_capture::NativeEntry;
use gateway_migration_data::native_capture::NativeRevision;
use gateway_migration_data::native_capture::NativeValue;
use gateway_migration_inverse::native_inverse::export_native_latest;
use gateway_migration_native_custody::native_custody::NativeCustody;
use gateway_migration_native_custody::native_custody::OwnedNativeSnapshot;
use gateway_migration_prepare::engine::adopt_empty;
use gateway_migration_prepare::engine::prepare_native;
use gateway_migration_stage::native_stage::Captured;
use gateway_migration_test_support::fixture::{Directory, run};
use gateway_migration_test_support::sqlite_store::SqliteStore;
use serde_json::{Value, json};

const SOURCE: &str = "6d4d4790a7c06514b92938ff677dbfd52764461d";

fn atom(value: Value, nodes: &mut Vec<Value>) -> Value {
    match value {
        Value::Null => json!({"type":"null","value":null}),
        Value::Bool(value) => json!({"type":"boolean","value":value}),
        Value::String(value) => json!({"type":"string","value":value}),
        Value::Number(value) => json!({"type":"number","value":value}),
        Value::Object(fields) => {
            let id = nodes.len();
            nodes.push(json!({"type":"object","entries":[]}));
            let entries = fields
                .into_iter()
                .map(|(name, value)| json!([name, atom(value, nodes)]))
                .collect::<Vec<_>>();
            nodes[id] = json!({"type":"object","entries":entries});
            json!({"type":"ref","ref":id})
        }
        Value::Array(items) => {
            let id = nodes.len();
            let length = items.len();
            nodes.push(json!({"type":"Array","length":length,"entries":[]}));
            let entries = items
                .into_iter()
                .enumerate()
                .map(|(index, value)| json!([index.to_string(), atom(value, nodes)]))
                .collect::<Vec<_>>();
            nodes[id] = json!({"type":"Array","length":length,"entries":entries});
            json!({"type":"ref","ref":id})
        }
    }
}

fn graph(value: Value) -> NativeValue {
    let mut nodes = Vec::new();
    let root = atom(value, &mut nodes);
    NativeValue::SourceGraphV1(
        gateway_core::source_json::SourceJson::from_serializable(&json!({
            "root": root,
            "nodes": nodes
        }))
        .unwrap(),
    )
}

fn entry(key_parts: Value, value: Value, revision: &str, expiration_ms: i64) -> NativeEntry {
    let raw_key = TupleKey::from_json_parts(key_parts.as_array().unwrap())
        .unwrap()
        .sort_bytes()
        .to_vec();
    NativeEntry {
        raw_key,
        value: graph(value),
        revision: NativeRevision {
            version: revision.into(),
            sequence: "0".into(),
        },
        expiration_ms,
    }
}

fn capture(entries: Vec<NativeEntry>) -> NativeCapture {
    NativeCapture {
        v: 2,
        source_git_sha: SOURCE.into(),
        captured_at_ms: 2200,
        source_codec: "bun-jsc-v1".into(),
        native_schema_version: 3,
        data_revision: NativeRevision {
            version: "50".into(),
            sequence: "0".into(),
        },
        queue_entries: 0,
        running_queue_entries: 0,
        entries,
    }
}

fn key_records(token: &str) -> (Value, Value) {
    let hash = gateway_auth::crypto::sha256_base64url(token.as_bytes());
    let hash_record = json!({
        "id":"core-key","expires_at_ms":-1,"revoked_at_ms":null,
        "usage_limit_requests":-1,"usage_requests":0,"usage_reset_at_ms":3000,
        "window_ms":1000,"usage_quota_version":3,
        "paid_fallback_enabled":false,"paid_fallback_limit_microcredits":-1,
        "paid_fallback_spent_microcredits":0,"paid_fallback_reserved_microcredits":0,
        "paid_fallback_reservation_request_id":null
    });
    let mut id = hash_record.clone();
    for (name, value) in json!({
        "name":"Core key","prefix":&token[..12],"hash":hash,"created_at_ms":1000,
        "paid_fallback_model_ids":[],"paid_fallback_quota_per_credit":0,
        "paid_fallback_max_exposure_microcredits":{},"paid_fallback_pricing_checked_at_ms":null
    })
    .as_object()
    .unwrap()
    {
        id[name] = value.clone();
    }
    (id, hash_record)
}

fn current_api_window() -> Value {
    json!({"v":3,"key_id":"core-key","policy_version":"v3:1000","window_start_ms":2000,"window_reset_at_ms":3000,"committed_requests":42,"reserved_requests":0,"updated_at_ms":2100})
}

fn paid_request(request: &str, reset: i64, state: &str) -> Value {
    json!({
        "v":3,"key_id":"core-key","request_id":request,"policy_version":"v3:1000",
        "route":"responses","path":"/v1/responses","stream":false,
        "model":"gpt-6.1-sol","reasoning":null,
        "window_reset_at_ms":reset,"reserved_microcredits":if state=="unresolved"{5}else{0},
        "quota_per_credit":1,"provider":"surplus","provider_request_id":null,"provider_quota":null,
        "input_tokens":null,"output_tokens":null,
        "spend_microcredits":if state=="settled"{Value::from(5)}else{Value::Null},
        "dispatch_state":if state=="settled"{"dispatched"}else{"reserved"},
        "terminal_state":if state=="settled"{"completed"}else{"pending"},
        "billing_state":state,"reconciliation_attempts":0,"last_reconciliation_at_ms":null,
        "dispatched_at_ms":if state=="settled"{Value::from(2050)}else{Value::Null},
        "terminal_at_ms":if state=="settled"{Value::from(2100)}else{Value::Null},
        "settled_at_ms":if state=="settled"{Value::from(2100)}else{Value::Null},
        "created_at_ms":2000,"updated_at_ms":if state=="settled"{2100}else{2000}
    })
}

fn valid_core_entries() -> Vec<NativeEntry> {
    let token = format!("u_{}", "2".repeat(64));
    let (id, hash) = key_records(&token);
    let hash_key = gateway_auth::crypto::sha256_base64url(token.as_bytes());
    vec![
        entry(
            json!(["ubq_ai", "api_keys", "id", "core-key"]),
            id,
            "49",
            -1,
        ),
        entry(
            json!(["ubq_ai", "api_keys", "hash", hash_key]),
            hash,
            "49",
            -1,
        ),
        entry(
            json!([
                "uos_ai",
                "api_key_usage",
                "v3",
                "window",
                "core-key",
                "v3:1000",
                2000
            ]),
            current_api_window(),
            "49",
            10_000,
        ),
        entry(
            json!([
                "uos_ai",
                "paid_fallback",
                "v3",
                "request",
                "core-key",
                "current"
            ]),
            paid_request("current", 3000, "unresolved"),
            "49",
            -1,
        ),
        entry(
            json!([
                "uos_ai",
                "paid_fallback",
                "v3",
                "pending",
                "core-key",
                "current"
            ]),
            json!({"created_at_ms":2000,"next_reconciliation_at_ms":2300}),
            "49",
            -1,
        ),
        entry(
            json!(["default", "kernel_policy_limit_requests"]),
            json!(-1),
            "49",
            -1,
        ),
        entry(
            json!(["default", "kernel_policy_window_ms"]),
            json!(604800000),
            "49",
            -1,
        ),
    ]
}

#[test]
fn current_core_excludes_histories_and_proves_the_free_write_budget() {
    let token = format!("u_{}", "2".repeat(64));
    let (id, hash) = key_records(&token);
    let hash_key = gateway_auth::crypto::sha256_base64url(token.as_bytes());
    let mut entries = valid_core_entries();
    entries[0] = entry(
        json!(["ubq_ai", "api_keys", "id", "core-key"]),
        id,
        "49",
        -1,
    );
    entries[1] = entry(
        json!(["ubq_ai", "api_keys", "hash", hash_key]),
        hash,
        "49",
        -1,
    );
    entries.extend([
        entry(
            json!(["uos_ai", "api_key_usage", "v3", "window", "core-key", "v3:1000", 1000]),
            json!({"v":3,"key_id":"core-key","policy_version":"v3:1000","window_start_ms":1000,"window_reset_at_ms":2000,"committed_requests":41,"reserved_requests":0,"updated_at_ms":1900}),
            "48",
            9000,
        ),
        entry(
            json!(["uos_ai", "api_key_usage", "v3", "request", "core-key", "v3:1000", 2000, "request-history"]),
            json!({"v":3,"key_id":"core-key","request_id":"request-history","route":"responses","state":"released","reserved_at_ms":2000,"lease_expires_at_ms":2100,"provider":null,"dispatched_at_ms":null,"released_at_ms":2100,"release_reason":"fixture"}),
            "48",
            9000,
        ),
        entry(
            json!(["uos_ai", "paid_fallback", "v3", "request", "core-key", "old"]),
            paid_request("old", 2000, "settled"),
            "48",
            -1,
        ),
        entry(
            json!(["uos_ai", "paid_fallback", "v3", "pending", "core-key", "old"]),
            json!({"created_at_ms":1000,"next_reconciliation_at_ms":1200}),
            "48",
            -1,
        ),
        entry(
            json!(["uos_ai", "paid_fallback", "v3", "reconciliation_gate"]),
            json!({"created_at_ms":1000,"next_reconciliation_at_ms":2300}),
            "48",
            -1,
        ),
        entry(json!(["uos_ai", "admin_error_log", "v1", "x"]), json!({}), "48", -1),
        entry(json!(["uos_ai", "sentinel_replay", "v1", "x"]), json!({}), "48", -1),
        entry(json!(["uos_ai", "sentinel_incident", "v1", "x"]), json!({}), "48", -1),
        entry(json!(["uos_ai", "prompt_cache_analytics", "v1", "x"]), json!({}), "48", -1),
        entry(json!(["uos_ai", "prompt_cache_telemetry_gate", "v1", "x"]), json!({}), "48", -1),
        entry(json!(["uos_ai", "provider_capacity", "v1", "history", 1000]), json!({}), "48", -1),
        entry(json!(["uos_ai", "metered_quota", "v1", "balance_history", 1000]), json!({}), "48", -1),
        entry(json!(["uos_ai", "api_keys", "change", 1]), json!({}), "48", -1),
    ]);
    let directory = Directory::new();
    let spool = directory.file("spool");
    std::fs::create_dir(&spool).unwrap();
    let core = Captured::owned(capture(entries), &spool)
        .unwrap()
        .core_capture()
        .unwrap();
    assert_eq!(core.manifest.source_rows, 20);
    assert_eq!(core.manifest.selected_rows, 7);
    assert_eq!(core.manifest.excluded_rows, 13);
    assert_eq!(core.manifest.current_api_windows, 1);
    assert_eq!(core.manifest.current_paid_requests, 1);
    assert_eq!(core.manifest.current_paid_unresolved, 1);
    assert_eq!(core.manifest.current_paid_pending, 1);
    assert!(
        core.manifest
            .excluded
            .iter()
            .any(|rule| rule.rule == "historical_observability" && rule.rows == 4)
    );
    assert!(
        core.manifest
            .excluded
            .iter()
            .any(|rule| rule.rule == "historical_error_log" && rule.rows == 1)
    );
    assert!(
        core.manifest
            .excluded
            .iter()
            .any(|rule| rule.rule == "historical_paid_fallback" && rule.rows == 2)
    );
    assert!(
        core.manifest
            .excluded
            .iter()
            .any(|rule| rule.rule == "historical_provider_capacity" && rule.rows == 1)
    );
    assert!(
        core.manifest
            .excluded
            .iter()
            .any(|rule| rule.rule == "historical_metered_balance" && rule.rows == 1)
    );
    assert!(
        core.manifest
            .excluded
            .iter()
            .any(|rule| rule.rule == "recomputed_paid_fallback_gate" && rule.rows == 1)
    );
    assert!(core.manifest.do_row_writes < CORE_STATE_DO_ROW_WRITE_LIMIT);
    assert_eq!(core.manifest.derived_rows, 4);
    assert_eq!(core.manifest.do_row_writes, 19);
    assert!(
        !serde_json::to_string(&core.manifest)
            .unwrap()
            .contains("request-history")
    );
}

#[test]
fn unknown_source_family_is_refused_before_any_adoption_plan() {
    let mut entries = valid_core_entries();
    entries.push(entry(
        json!(["unknown_namespace", "record"]),
        json!({}),
        "49",
        -1,
    ));
    let directory = Directory::new();
    let spool = directory.file("spool");
    std::fs::create_dir(&spool).unwrap();
    assert!(
        Captured::owned(capture(entries), &spool)
            .unwrap()
            .core_capture()
            .is_err()
    );
}

#[test]
fn selected_core_adopts_restarts_reads_quota_and_round_trips_current_accounting()
-> Result<(), Box<dyn std::error::Error>> {
    let token = format!("u_{}", "2".repeat(64));
    let directory = Directory::new();
    let path = directory.file("core.sqlite3");
    let spool = directory.file("spool");
    std::fs::create_dir(&spool)?;
    let core = Captured::owned(capture(valid_core_entries()), &spool)?.core_capture()?;
    let prepared = match prepare_native(&core.capture) {
        Ok(prepared) => prepared,
        Err(gateway_migration_data::error::MigrationError::SourceInventory(audit)) => panic!(
            "prepare_native failed: {:?}",
            audit
                .errors
                .iter()
                .map(|error| error.try_utf8().unwrap_or_else(|_| "<non-utf8>".into()))
                .collect::<Vec<_>>()
        ),
        Err(error) => panic!("prepare_native failed: {error:?}"),
    };
    assert_eq!(prepared.source_rows(), core.manifest.selected_rows);
    assert_eq!(prepared.derived_rows(), core.manifest.derived_rows);
    assert_eq!(
        prepared.expected_do_row_writes(),
        Some(core.manifest.do_row_writes)
    );
    assert_eq!(core.manifest.do_row_writes, 19);
    let store = SqliteStore::open(&path);
    let receipt = run(adopt_empty(&store, &prepared))?;
    assert_eq!(receipt.source_rows, core.manifest.selected_rows);
    assert_eq!(receipt.derived_rows, core.manifest.derived_rows);
    drop(store);

    let store = SqliteStore::open(&path);
    assert_eq!(
        run(gateway_auth::api_keys::authenticate(&store, &token, 2300))?.key_id,
        "core-key"
    );
    let listed = run(gateway_admin::reads::api_keys(&store, 2300))?;
    assert_eq!(listed.status, 200);
    let body = String::from_utf8(listed.body.to_bytes()?)?;
    assert!(body.contains("core-key"));
    assert!(body.contains("42"));
    let listed: Value = serde_json::from_str(&body)?;
    assert_eq!(
        listed["data"][0]["paid_fallback_reserved_credits"].as_f64(),
        Some(0.000005)
    );
    let authority = gateway_authority::store::Authority::with_clock(
        store.clone(),
        support_clock::Tokens(std::sync::atomic::AtomicU64::new(1)),
        support_clock::Clock,
    );
    let reservation = run(authority.reserve_api_key(
        &gateway_core::identity::ApiKeyId("core-key".into()),
        &gateway_core::identity::RequestId("post-restart".into()),
        "responses",
        2300,
    ))?;
    assert_eq!(reservation.request_id.0, "post-restart");
    drop(authority);
    let paid_key = gateway_authority::keys::paid_attempt("core-key", "current");
    let paid = run(store.read(&paid_key))?.ok_or("Expected retained fixture row")?;
    let paid =
        gateway_authority::paid::source_records::decode_paid_attempt_record(&paid_key, &paid)?;
    assert_eq!(
        paid.billing_state,
        gateway_core::lifecycle::BillingState::Unresolved
    );
    assert_eq!(paid.reservation.reserved_microcredits, 5);
    let gate = run(store.read(&gateway_authority::keys::paid_gate()))?
        .ok_or("Expected retained fixture row")?;
    assert_eq!(
        gate.json_value()?["pending"]
            .as_object()
            .ok_or("Expected pending accounting object")?
            .len(),
        1
    );
    let latest = run(export_native_latest(&store, 2300))?;
    let paid_sort = TupleKey::from_record_key(&paid_key)?.sort_bytes().to_vec();
    assert!(
        latest
            .entries
            .iter()
            .any(|entry| entry.raw_key == paid_sort)
    );
    let paid_window = gateway_authority::keys::paid_window("core-key", 3000);
    let paid_window_sort = TupleKey::from_record_key(&paid_window)?
        .sort_bytes()
        .to_vec();
    assert!(
        !latest
            .entries
            .iter()
            .any(|entry| entry.raw_key == paid_window_sort),
        "derived paid window must not become source state"
    );
    let inverse = SqliteStore::open(&directory.file("inverse.sqlite3"));
    let inverse_prepared = prepare_native(&latest)?;
    run(adopt_empty(&inverse, &inverse_prepared))?;
    assert_eq!(
        run(gateway_auth::api_keys::authenticate(&inverse, &token, 2300))?.key_id,
        "core-key"
    );
    assert!(run(inverse.read(&key(json!(["uos_ai", "admin_error_log"]))))?.is_none());
    Ok(())
}

#[test]
fn bounded_core_staging_uses_the_same_spool_and_write_budget() -> FixtureResult<()> {
    let directory = Directory::new();
    let spool = directory.file("spool");
    std::fs::create_dir(&spool)?;
    let (manifest, mut plan) =
        run(Captured::owned(capture(valid_core_entries()), &spool)?.prepare_core(&NoCustody))?;
    let summary = plan.summary();
    assert_eq!(summary.source_rows, manifest.selected_rows);
    assert_eq!(summary.derived_rows, manifest.derived_rows);
    assert_eq!(summary.records as u64 + 1, manifest.do_row_writes);
    let store = SqliteStore::open(&directory.file("staged.sqlite3"));
    run(gateway_migration_protocol::staged::execute_staged(
        &store,
        plan.begin(),
    ))?;
    while let Some(batch) = plan.next_batch()? {
        run(gateway_migration_protocol::staged::execute_staged(
            &store, batch,
        ))?;
    }
    let receipt = run(gateway_migration_protocol::staged::execute_staged(
        &store,
        plan.seal(),
    ))?;
    assert!(receipt.sealed);
    assert!(!receipt.activation_authorized);
    assert_eq!(receipt.accepted_records as u64 + 1, manifest.do_row_writes);
    Ok(())
}
struct NoCustody;
impl NativeCustody for NoCustody {
    async fn observe(
        &self,
        _: &str,
    ) -> Result<OwnedNativeSnapshot, gateway_core::error::GatewayError> {
        Err(gateway_core::error::GatewayError::unavailable(
            "no native custody in synthetic core-state fixture",
            "migration_native_custody_required",
        ))
    }
}

type FixtureResult<T> = Result<T, Box<dyn std::error::Error>>;

#[derive(serde::Deserialize)]
struct ClaudeSourceFixture {
    source_sha: String,
    synthetic: bool,
    fixture_clock_ms: i64,
    rows: Vec<ClaudeSourceRow>,
}

#[derive(serde::Deserialize)]
struct ClaudeSourceRow {
    key: Vec<Value>,
    value: gateway_core::source_json::SourceJson,
    revision: String,
    expires_at_ms: Option<i64>,
}

fn claude_source_entries() -> FixtureResult<Vec<NativeEntry>> {
    let fixture: ClaudeSourceFixture =
        serde_json::from_str(include_str!("fixtures/claude-core-retention-source.json"))?;
    assert_eq!(
        fixture.source_sha,
        "b59243d674567d418d1410f8390fc7fe47a08c92"
    );
    assert!(fixture.synthetic);
    assert_eq!(fixture.fixture_clock_ms, 2200);
    fixture
        .rows
        .into_iter()
        .map(|row| {
            Ok(NativeEntry {
                raw_key: TupleKey::from_json_parts(&row.key)?.sort_bytes().to_vec(),
                value: NativeValue::SourceGraphV1(
                    gateway_migration_graph::transport::tree_to_source_graph(
                        &gateway_core::source_control::source_json_tree(&row.value)?,
                    )?,
                ),
                revision: NativeRevision {
                    version: row.revision,
                    sequence: "0".into(),
                },
                expiration_ms: row.expires_at_ms.unwrap_or(-1),
            })
        })
        .collect()
}

fn entry_key(entry: &NativeEntry) -> FixtureResult<gateway_core::state::RecordKey> {
    Ok(TupleKey::from_sort_bytes(entry.raw_key.clone())?.to_record_key())
}

fn native_tree(entry: &NativeEntry) -> FixtureResult<gateway_core::value::PortableValue> {
    let NativeValue::SourceGraphV1(graph) = &entry.value else {
        return Err("Expected source-produced JSON graph".into());
    };
    Ok(
        gateway_migration_graph::transport::from_source_graph(graph)?
            .try_identity_free_tree()?
            .ok_or("Expected identity-free source JSON")?,
    )
}

fn assert_original_entry(actual: &NativeCapture, expected: &NativeEntry) -> FixtureResult<()> {
    let row = actual
        .entries
        .iter()
        .find(|row| row.raw_key == expected.raw_key)
        .ok_or("Retained source row is missing")?;
    assert_eq!(row.revision, expected.revision);
    assert_eq!(row.expiration_ms, expected.expiration_ms);
    assert_eq!(native_tree(row)?, native_tree(expected)?);
    Ok(())
}

fn claude_parts(entry: &NativeEntry) -> FixtureResult<Vec<Value>> {
    Ok(gateway_migration_data::parts(&entry_key(entry)?)?)
}

fn write_current_quota_and_delete_lease(
    store: &SqliteStore,
    quota: &NativeEntry,
    lease: &NativeEntry,
) -> FixtureResult<gateway_core::value::PortableValue> {
    use gateway_core::state::{
        AtomicTransition, CommitOutcome, RecordMutation, RevisionCheck, StoredRecord,
    };
    use gateway_core::value::PortableValue;
    let quota_key = entry_key(quota)?;
    let lease_key = entry_key(lease)?;
    let current_quota = run(store.read(&quota_key))?.ok_or("Current quota row missing")?;
    let current_lease = run(store.read(&lease_key))?.ok_or("Current collector lease missing")?;
    let mut next = native_tree(quota)?;
    let PortableValue::ObjectUtf16(fields) = &mut next else {
        return Err("Expected a lossless source object".into());
    };
    fields
        .iter_mut()
        .find(|(name, _)| *name == gateway_core::js_string::JsString16::from_utf8("updated_at_ms"))
        .ok_or("Source quota update clock missing")?
        .1 = PortableValue::number(2300.0);
    let record = StoredRecord::profile_v2(
        "post-adoption-current-quota",
        next.clone(),
        current_quota.expires_at_ms,
    )?;
    assert_eq!(
        run(store.commit(AtomicTransition {
            checks: vec![
                RevisionCheck {
                    key: quota_key.clone(),
                    expected_revision: Some(current_quota.revision),
                },
                RevisionCheck {
                    key: lease_key.clone(),
                    expected_revision: Some(current_lease.revision),
                },
            ],
            mutations: vec![
                RecordMutation::Put {
                    key: quota_key,
                    record
                },
                RecordMutation::Delete { key: lease_key },
            ],
        }))?,
        CommitOutcome::Committed
    );
    Ok(next)
}

#[test]
fn source_claude_core_keeps_current_rows_and_latest_writes_without_renewing_lease_expiry()
-> Result<(), Box<dyn std::error::Error>> {
    let rows = claude_source_entries()?;
    assert_eq!(rows.len(), 5);
    let mut lease = None;
    let mut quota = None;
    for row in &rows {
        let parts = claude_parts(row)?;
        if gateway_migration_data::claude::usage_lease_key(&parts) {
            lease = Some(row);
        }
        if parts[1] == "claude_code_quota" {
            quota = Some(row);
        }
    }
    let lease = lease.ok_or("Source collector lease missing")?;
    let quota = quota.ok_or("Source quota row missing")?;
    assert_eq!(lease.expiration_ms, 22_200);
    assert_eq!(quota.expiration_ms, -1);
    let mut entries = valid_core_entries();
    entries.extend(rows.clone());
    let directory = Directory::new();
    let spool = directory.file("claude-spool");
    std::fs::create_dir(&spool)?;
    let core = Captured::owned(capture(entries), &spool)?.core_capture()?;
    assert_eq!(core.manifest.source_rows, 12);
    assert_eq!(core.manifest.selected_rows, 9);
    assert_eq!(core.manifest.excluded_rows, 3);
    assert_eq!(core.manifest.do_row_writes, 23);
    assert_original_entry(&core.capture, lease)?;
    assert_original_entry(&core.capture, quota)?;
    for excluded in &rows {
        if gateway_migration_data::claude::historical_capacity_key(&claude_parts(excluded)?) {
            assert!(
                core.capture
                    .entries
                    .iter()
                    .all(|row| row.raw_key != excluded.raw_key)
            );
        }
    }
    let path = directory.file("claude-core.sqlite3");
    let store = SqliteStore::open(&path);
    run(adopt_empty(&store, &prepare_native(&core.capture)?))?;
    drop(store);
    let store = SqliteStore::open(&path);
    let unchanged = run(export_native_latest(&store, 2300))?;
    assert_original_entry(&unchanged, lease)?;
    assert_original_entry(&unchanged, quota)?;
    let next = write_current_quota_and_delete_lease(&store, quota, lease)?;
    drop(store);
    let store = SqliteStore::open(&path);
    let latest = run(export_native_latest(&store, 2300))?;
    assert!(
        latest
            .entries
            .iter()
            .all(|row| row.raw_key != lease.raw_key)
    );
    let updated = latest
        .entries
        .iter()
        .find(|row| row.raw_key == quota.raw_key)
        .ok_or("Updated current quota row missing")?;
    assert_ne!(updated.revision, quota.revision);
    assert_eq!(updated.expiration_ms, -1);
    assert_eq!(native_tree(updated)?, next);
    for row in &latest.entries {
        assert!(!gateway_migration_data::claude::historical_capacity_key(
            &claude_parts(row)?
        ));
    }
    let inverse_path = directory.file("claude-core-inverse.sqlite3");
    let inverse = SqliteStore::open(&inverse_path);
    run(adopt_empty(&inverse, &prepare_native(&latest)?))?;
    drop(inverse);
    let inverse = SqliteStore::open(&inverse_path);
    assert!(run(inverse.read(&entry_key(lease)?))?.is_none());
    let inverse_latest = run(export_native_latest(&inverse, 2300))?;
    assert_original_entry(&inverse_latest, updated)?;
    let api = gateway_authority::keys::api_window("core-key", "v3:1000", 2000);
    assert_eq!(
        run(inverse.read(&api))?
            .ok_or("Current API window missing")?
            .json_value()?["committed_requests"],
        42
    );
    Ok(())
}

#[test]
fn full_history_export_still_retains_every_source_claude_history_kind()
-> Result<(), Box<dyn std::error::Error>> {
    let rows = claude_source_entries()?;
    let mut entries = valid_core_entries();
    entries.extend(rows.clone());
    let input = capture(entries);
    let prepared = prepare_native(&input)?;
    assert_eq!(prepared.source_rows(), 12);
    let directory = Directory::new();
    let path = directory.file("claude-full-history.sqlite3");
    let store = SqliteStore::open(&path);
    run(adopt_empty(&store, &prepared))?;
    drop(store);
    let store = SqliteStore::open(&path);
    let latest = run(export_native_latest(&store, 2300))?;
    for row in &rows {
        assert_original_entry(&latest, row)?;
    }
    Ok(())
}

#[test]
fn unknown_claude_versions_kinds_and_key_shapes_refuse_core_selection() -> FixtureResult<()> {
    for key in [
        json!(["uos_ai", "claude_code_usage", "v2", "lease", "a", "c"]),
        json!(["uos_ai", "claude_code_usage", "v1", "future", "a", "c"]),
        json!([
            "uos_ai",
            "claude_code_usage",
            "v1",
            "lease",
            "a",
            "c",
            "extra"
        ]),
        json!([
            "uos_ai",
            "provider_capacity",
            "claude",
            "v2",
            "history",
            0,
            "a",
            "c"
        ]),
        json!([
            "uos_ai",
            "provider_capacity",
            "claude",
            "v1",
            "future",
            0,
            "a",
            "c"
        ]),
        json!([
            "uos_ai",
            "provider_capacity",
            "claude",
            "v1",
            "history",
            0,
            "a",
            "c",
            "extra"
        ]),
        json!([
            "uos_ai",
            "provider_capacity",
            "claude",
            "v1",
            "rollup",
            0,
            "a",
            "c",
            "future",
            "unknown"
        ]),
        json!([
            "uos_ai",
            "provider_capacity",
            "claude",
            "v1",
            "usage_rollup",
            0,
            "a",
            "c",
            "not-a-digest"
        ]),
    ] {
        let mut entries = valid_core_entries();
        entries.push(entry(key.clone(), json!({}), "49", -1));
        let input = capture(entries);
        if key[1] == "claude_code_usage" {
            assert!(prepare_native(&input).is_err());
        }
        let directory = Directory::new();
        let spool = directory.file("unknown-claude-spool");
        std::fs::create_dir(&spool)?;
        assert!(Captured::owned(input, &spool)?.core_capture().is_err());
    }
    Ok(())
}
mod support_clock {
    use gateway_core::error::GatewayError;
    pub struct Tokens(pub std::sync::atomic::AtomicU64);
    impl gateway_authority::store::TokenSource for Tokens {
        fn next_token(&self) -> Result<String, GatewayError> {
            Ok(format!(
                "core-fixture-{}",
                self.0.fetch_add(1, std::sync::atomic::Ordering::SeqCst)
            ))
        }
    }
    pub struct Clock;
    impl gateway_authority::store::AuthorityClock for Clock {
        fn now_ms(&self) -> Result<i64, GatewayError> {
            Ok(2300)
        }
    }
}
