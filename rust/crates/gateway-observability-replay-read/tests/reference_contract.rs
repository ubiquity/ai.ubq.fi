use gateway_core::{
    query::{QueryPage, QueryRequest, QueryStore},
    state::{AtomicTransition, AuthorityStore, CommitOutcome, StoredRecord},
};
use gateway_kv_codec::{
    key::RecordKey,
    tuple_key::{KeyPart, TupleKey},
};
use gateway_observability_contracts::source_object::portable_json;
use gateway_observability_replay_model::{replay_crypto::ReplayKey, replay_model as model};
use gateway_observability_replay_read::{
    ReplayReader,
    duplicate::{DuplicateCapture, ReplayDuplicates},
    reference::dedupe_record_matches_claimed_manifest,
};
use gateway_observability_replay_records::accounting::RetentionLimits;
use gateway_observability_replay_state::ReplayBootstrapState;
use gateway_observability_test_support::runtime::{Host, Memory, run};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use std::{cell::RefCell, rc::Rc};

type TestResult = Result<(), Box<dyn std::error::Error>>;
const NOW: i64 = 1_800_000_000_000;
const PARTS: &str = r#"["\ud800","nul\u0000","é😀",true,false,0.0,9007199254740993]"#;
const NEGATIVE_ZERO_PARTS: &str =
    r#"["\ud800","nul\u0000","é😀",true,false,-0.0,9007199254740993]"#;

fn record(source: &str) -> Result<StoredRecord, GatewayError> {
    StoredRecord::profile_v2(
        "reference-revision",
        portable_json(&SourceJson::parse(source)?)?,
        None,
    )
}
fn expected(negative_zero: bool) -> Result<RecordKey, GatewayError> {
    TupleKey::from_parts(&[
        KeyPart::source_string_utf16(&[0xd800]),
        KeyPart::String("nul\0".into()),
        KeyPart::String("é😀".into()),
        KeyPart::Boolean(true),
        KeyPart::Boolean(false),
        KeyPart::Number(if negative_zero { -0.0 } else { 0.0 }),
        KeyPart::Number(9_007_199_254_740_993_u64 as f64),
    ])
    .map(|key| key.to_record_key())
}
#[derive(Clone, Default)]
struct TraceStore {
    memory: Memory,
    reads: Rc<RefCell<Vec<RecordKey>>>,
}
impl AuthorityStore for TraceStore {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        self.reads.borrow_mut().push(key.clone());
        self.memory.read(key).await
    }
    async fn commit(&self, transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        self.memory.commit(transition).await
    }
}
impl QueryStore for TraceStore {
    async fn query(&self, request: QueryRequest) -> Result<QueryPage, GatewayError> {
        self.memory.query(request).await
    }
}

#[test]
fn complete_dedupe_predicate_preserves_seven_scalar_bytes_without_a_prefix_gate() -> TestResult {
    let row = record(&format!(r#"{{"manifest_key":{PARTS},"unknown":"\udfff"}}"#))?;
    let before = row.clone();
    assert!(dedupe_record_matches_claimed_manifest(
        &row,
        &expected(false)?
    )?);
    assert!(!dedupe_record_matches_claimed_manifest(
        &row,
        &expected(true)?
    )?);
    assert_eq!(row, before);
    Ok(())
}

#[test]
fn complete_dedupe_predicate_keeps_missing_length_and_decode_error_distinct() -> TestResult {
    let key = expected(true)?;
    assert!(!dedupe_record_matches_claimed_manifest(
        &record("{}")?,
        &key
    )?);
    assert!(!dedupe_record_matches_claimed_manifest(
        &record(r#"{"manifest_key":[null,null,null,null,null,null]}"#)?,
        &key
    )?);
    assert!(
        dedupe_record_matches_claimed_manifest(&record(r#"{"manifest_key":null}"#)?, &key).is_err()
    );
    assert!(
        dedupe_record_matches_claimed_manifest(
            &record(r#"{"manifest_key":[0,1,2,3,4,5,null]}"#)?,
            &key
        )
        .is_err()
    );
    Ok(())
}

#[test]
fn request_export_reads_the_original_scalar_key_before_missing_manifest_fallback() -> TestResult {
    let store = TraceStore::default();
    let host = Host::default();
    host.at.set(NOW);
    let fingerprint = "a".repeat(64);
    let request_key = model::request_key("reference-request");
    let status = record(&format!(
        r#"{{"version":1,"request_id":"reference-request","status":"ready","reason":null,"captured_at_ms":{NOW},"manifest_key":{PARTS},"fingerprint":"{fingerprint}","expires_at_ms":{},"unknown":"\udfff"}}"#,
        NOW + 1000
    ))?;
    store
        .memory
        .rows
        .borrow_mut()
        .insert(request_key.0.clone(), status);
    let response =
        run(ReplayReader::new(&store, &host).export_capture_request("reference-request"))?;
    assert_eq!(
        *store.reads.borrow(),
        vec![
            request_key,
            expected(false)?,
            model::tombstone_key(&fingerprint)
        ]
    );
    assert_eq!(store.memory.commits.get(), 0);
    let body = String::from_utf8(response.body.to_bytes()?)?;
    assert!(body.contains("\\udfff"));
    assert!(body.contains("manifest_unavailable"));
    Ok(())
}

#[test]
fn duplicate_operation_rejects_bad_last_scalar_before_any_winner_read_or_status_write() -> TestResult
{
    let store = TraceStore::default();
    let host = Host::default();
    host.at.set(NOW);
    let dedupe = record(r#"{"manifest_key":[0,1,2,3,4,5,null]}"#)?;
    let key = ReplayKey::decode("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA")?;
    let fingerprint = "a".repeat(64);
    let duplicates = ReplayDuplicates::new(&store, &host, ReplayBootstrapState::default());
    let result = run(duplicates.persist_duplicate(
        "reference-request",
        &key,
        DuplicateCapture {
            fingerprint: &fingerprint,
            dedupe: &dedupe,
            index: None,
            at: NOW,
            limits: RetentionLimits::default(),
            state: model::CaptureState::Ready,
        },
    ));
    assert!(result.is_err());
    assert!(store.reads.borrow().is_empty());
    assert_eq!(store.memory.commits.get(), 0);
    Ok(())
}

#[test]
fn dedupe_negative_zero_profile_v2_projection_refusal() -> TestResult {
    let row = record(&format!(
        r#"{{"manifest_key":{NEGATIVE_ZERO_PARTS},"unknown":"\udfff"}}"#
    ))?;
    let before = row.clone();
    assert!(row.profile_v2_value().is_ok());
    let projection = row.profile_v2_source_json().unwrap_err();
    assert_eq!(projection.status, 503);
    assert_eq!(
        projection.envelope.error.code.as_deref(),
        Some("source_json_invalid")
    );
    let error = dedupe_record_matches_claimed_manifest(&row, &expected(true)?).unwrap_err();
    assert_eq!(error.status, 503);
    assert_eq!(
        error.envelope.error.code.as_deref(),
        Some("source_json_invalid")
    );
    assert_eq!(row, before);
    Ok(())
}

#[test]
fn request_negative_zero_profile_v2_projection_refusal() -> TestResult {
    let store = TraceStore::default();
    let host = Host::default();
    host.at.set(NOW);
    let fingerprint = "a".repeat(64);
    let request_key = model::request_key("reference-request");
    let status = record(&format!(
        r#"{{"version":1,"request_id":"reference-request","status":"ready","reason":null,"captured_at_ms":{NOW},"manifest_key":{NEGATIVE_ZERO_PARTS},"fingerprint":"{fingerprint}","expires_at_ms":{},"unknown":"\udfff"}}"#,
        NOW + 1000
    ))?;
    store
        .memory
        .rows
        .borrow_mut()
        .insert(request_key.0.clone(), status.clone());
    let error = run(ReplayReader::new(&store, &host).export_capture_request("reference-request"))
        .err()
        .ok_or("Negative-zero request projection unexpectedly exported")?;
    assert_eq!(error.status, 503);
    assert_eq!(
        error.envelope.error.code.as_deref(),
        Some("sentinel_replay_export_failed")
    );
    assert_eq!(*store.reads.borrow(), vec![request_key.clone()]);
    assert_eq!(store.memory.commits.get(), 0);
    assert_eq!(
        store.memory.rows.borrow().get(&request_key.0),
        Some(&status)
    );
    Ok(())
}
