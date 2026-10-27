#![cfg(not(target_arch = "wasm32"))]
//! Source-shaped stopped input exercises M20's real projection/reader/inverse.
//! This does not replace the separately required actual M19 writer fixture.
use gateway_core::{
    js_string::JsString16,
    model::ModelId,
    source_control::{encode_source_control_record, source_json_tree},
    source_json::SourceJson,
    source_serialization as source,
    value::PortableValue,
};
use gateway_migration_data::capture::PrivateCapture;
use gateway_migration_data::capture::SourceEntry;
use gateway_migration_inverse::inverse::export_latest;
use gateway_migration_prepare::engine::adopt_empty;
use gateway_migration_prepare::engine::prepare;
use gateway_migration_test_support::fixture::{Directory, run};
use gateway_migration_test_support::sqlite_store::SqliteStore;
use gateway_profile_source::codec::from_source_profile;
use gateway_profile_source::codec::to_source_profile;
use gateway_profile_values::date::CanonicalDates;
use serde_json::{Value, json};

fn entry(parts: Value, document: SourceJson) -> SourceEntry {
    SourceEntry {
        key: parts
            .as_array()
            .unwrap()
            .iter()
            .map(|part| to_source_profile(&PortableValue::from_json(part)).unwrap())
            .collect(),
        value: to_source_profile(&source_json_tree(&document).unwrap()).unwrap(),
        versionstamp: "source-paid-control".into(),
        expires_at_ms: None,
    }
}

#[test]
fn latest_paid_projection_preserves_current_model_effort_and_source_only_fields()
-> Result<(), Box<dyn std::error::Error>> {
    let mut capture: PrivateCapture =
        serde_json::from_str(include_str!("fixtures/offline-source.json"))?;
    let key_id = "offline-key";
    let attempt_id = "source-model-attempt";
    let finite = json!({"v":3,"key_id":key_id,"request_id":attempt_id,"policy_version":"v3:1000","route":"responses","path":"/v1/responses","stream":false,"window_reset_at_ms":10000,"reserved_microcredits":5,"quota_per_credit":1,"provider":"surplus","provider_request_id":null,"provider_quota":null,"input_tokens":null,"output_tokens":null,"spend_microcredits":null,"dispatch_state":"reserved","terminal_state":"pending","billing_state":"pending","reconciliation_attempts":0,"last_reconciliation_at_ms":null,"dispatched_at_ms":null,"terminal_at_ms":null,"settled_at_ms":null,"created_at_ms":2000,"updated_at_ms":2000,"source_only_fixture_field":"retained"});
    let mut fields = SourceJson::from_serializable(&finite)?.object_entries()?;
    fields.push((
        JsString16::from_utf8("model"),
        SourceJson::parse(r#""\ud800""#)?,
    ));
    fields.push((
        JsString16::from_utf8("reasoning"),
        SourceJson::parse(r#""\udc00""#)?,
    ));
    capture.entries.push(entry(
        json!([
            "uos_ai",
            "paid_fallback",
            "v3",
            "request",
            key_id,
            attempt_id
        ]),
        source::object_keys(fields)?,
    ));
    capture.entries.push(entry(json!(["uos_ai","paid_fallback","v3","window",key_id,10000]), SourceJson::from_serializable(&json!({"v":3,"key_id":key_id,"policy_version":"v3:1000","window_reset_at_ms":10000,"limit_microcredits":100,"settled_microcredits":0,"reserved_microcredits":5,"pending_count":1,"updated_at_ms":2000}))?));
    capture.entries.push(entry(
        json!([
            "uos_ai",
            "paid_fallback",
            "v3",
            "pending",
            key_id,
            attempt_id
        ]),
        SourceJson::parse(r#"{"created_at_ms":2000,"next_reconciliation_at_ms":2200}"#)?,
    ));
    let directory = Directory::new();
    let path = directory.file("paid-source.sqlite3");
    let store = SqliteStore::open(&path);
    run(adopt_empty(&store, &prepare(&capture, &CanonicalDates)?))?;
    drop(store);
    let store = SqliteStore::open(&path);
    let key = gateway_authority::keys::paid_attempt(key_id, attempt_id);
    let original = run(gateway_core::state::AuthorityStore::read(&store, &key))?
        .ok_or("Expected retained fixture row")?;
    let mut current =
        gateway_authority::paid::source_records::decode_paid_attempt_record(&key, &original)?;
    assert_eq!(current.model.as_js_string().as_units(), &[0xd800]);
    assert_eq!(
        current
            .reasoning
            .as_ref()
            .ok_or("Expected source reasoning")?
            .as_units(),
        &[0xdc00]
    );
    // Controlled post-capture state mutation, not a claim of an M19 writer run.
    current.model = ModelId::from_source(JsString16::from_units(vec![0xdc00]));
    current.reasoning = Some(JsString16::from_units(vec![0xd800]));
    let changed = encode_source_control_record(
        "post-capture-current",
        &SourceJson::from_serializable(&current)?,
        original.expires_at_ms,
    )?;
    store.put(key.clone(), changed.clone());
    let latest = run(export_latest(&store, 2400))?;
    let mut request_key = None;
    for row in &capture.entries {
        if row.versionstamp == "source-paid-control"
            && from_source_profile(&row.value)?
                .to_source_json()?
                .as_str()
                .contains("source_only_fixture_field")
        {
            request_key = Some(row.key.clone());
            break;
        }
    }
    let request_key = request_key.ok_or("Expected original paid source key")?;
    let exported = latest
        .entries
        .iter()
        .find(|row| row.key == request_key)
        .ok_or("Expected current paid source entry")?;
    let document = from_source_profile(&exported.value)?.to_source_json()?;
    assert_eq!(
        source::member(&document, "model")
            .ok_or("Expected source fixture member")?
            .deserialize::<JsString16>()?
            .as_units(),
        &[0xdc00]
    );
    assert_eq!(
        source::member(&document, "reasoning")
            .ok_or("Expected source fixture member")?
            .deserialize::<JsString16>()?
            .as_units(),
        &[0xd800]
    );
    assert_eq!(
        source::member(&document, "source_only_fixture_field")
            .ok_or("Expected source fixture member")?
            .deserialize::<String>()?,
        "retained"
    );
    assert_eq!(exported.versionstamp, "post-capture-current");
    assert_eq!(exported.expires_at_ms, original.expires_at_ms);
    assert_eq!(
        run(gateway_core::state::AuthorityStore::read(&store, &key))?,
        Some(changed)
    );
    Ok(())
}
