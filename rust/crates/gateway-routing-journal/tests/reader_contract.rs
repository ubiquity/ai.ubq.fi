use gateway_contracts::provider::UpstreamProvider;
use gateway_core::{
    source_control::encode_source_control_record,
    state::{AtomicTransition, AuthorityStore, CommitOutcome, StoredRecord},
};
use gateway_kv_codec::{key::RecordKey, tuple_key::TupleKey, value::ValueEncoding};
use gateway_routing_journal::reader::{load, session_key};
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use std::{
    cell::RefCell,
    future::Future,
    task::{Context, Poll, Waker},
};

type TestResult = Result<(), Box<dyn std::error::Error>>;
const ID: &str = "journal-reader";
const DOCUMENT: &str = r#"{
"v":3,
"context":{"request_id":"journal-reader","principal":"\ud800","key_id":null,"route":"/v1/responses","started_at_ms":10},
"input_digest":"0000000000000000000000000000000000000000000000000000000000000000",
"api":null,"kernel":null,"cursor":0,"attempts":[],"active":null,
"model_sibling_retries":0,"refreshed_versions":[],"recovery_attempted":false,
"claude_unavailable":false,"semantic_committed":false,"terminal_intent":null,
"finished":false,"recovering":false,"lease_expires_at_ms":200,"updated_at_ms":10,
"extras_family":null,"extras_child_count":null,"extras_dispatch":null,
"children":[],"kernel_override":null
}"#;

struct ReadStore {
    row: Option<StoredRecord>,
    reads: RefCell<Vec<RecordKey>>,
}
impl ReadStore {
    fn document(text: &str, expiry: Option<i64>) -> Result<Self, GatewayError> {
        let document = SourceJson::parse(text)?;
        Ok(Self {
            row: Some(encode_source_control_record(
                "original-revision",
                &document,
                expiry,
            )?),
            reads: RefCell::new(Vec::new()),
        })
    }
}
impl AuthorityStore for ReadStore {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        TupleKey::from_record_key(key)?.validate_stored_key()?;
        self.reads.borrow_mut().push(key.clone());
        Ok(self
            .row
            .as_ref()
            .filter(|row| row.expires_at_ms.is_none_or(|at| at > 100))
            .cloned())
    }
    async fn commit(&self, _: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        Err(GatewayError::unavailable(
            "Read-only fixture cannot commit.",
            "fixture_read_only",
        ))
    }
}

// This fixture has no asynchronous I/O; Pending is a failure, never a polling loop.
fn ready<F: Future>(future: F) -> F::Output {
    let mut context = Context::from_waker(Waker::noop());
    match std::pin::pin!(future).as_mut().poll(&mut context) {
        Poll::Ready(value) => value,
        Poll::Pending => panic!("read-only fixture unexpectedly pending"),
    }
}

#[test]
fn complete_profile_reader_retains_utf16_revision_expiry_and_single_actual_read() -> TestResult {
    let store = ReadStore::document(DOCUMENT, Some(150))?;
    let (record, journal) = ready(load(&store, ID))?;
    assert_eq!(record.revision, "original-revision");
    assert_eq!(record.expires_at_ms, Some(150));
    assert_eq!(record.value_encoding, ValueEncoding::ProfileV2);
    assert_eq!(
        journal.context.principal.as_source(),
        &JsString16::from_units(vec![0xd800])
    );
    assert!(journal.attempts.is_empty());
    assert_eq!(store.reads.borrow().as_slice(), &[session_key(ID)]);
    Ok(())
}

#[test]
fn missing_and_expired_rows_fail_before_any_journal_decode() -> TestResult {
    for expiry in [Some(99), Some(100)] {
        let store = ReadStore::document("{}", expiry)?;
        let error = ready(load(&store, ID))
            .err()
            .ok_or("expired row was decoded")?;
        assert_eq!(error.envelope.error.message, "Routing session is missing.");
        assert_eq!(store.reads.borrow().as_slice(), &[session_key(ID)]);
    }
    let store = ReadStore {
        row: None,
        reads: RefCell::new(Vec::new()),
    };
    assert_eq!(
        ready(load(&store, ID))
            .err()
            .ok_or("missing row accepted")?
            .envelope
            .error
            .message,
        "Routing session is missing."
    );
    Ok(())
}

#[test]
fn reader_keeps_source_encoding_version_and_context_failure_precedence() -> TestResult {
    for encoding in [ValueEncoding::ProfileV1, ValueEncoding::SourceGraphV1] {
        let mut store = ReadStore::document(DOCUMENT, None)?;
        store.row.as_mut().ok_or("fixture missing")?.value_encoding = encoding;
        let error = ready(load(&store, ID))
            .err()
            .ok_or("unsupported encoding accepted")?;
        assert_eq!(
            error.envelope.error.code.as_deref(),
            Some("source_control_record_invalid")
        );
    }
    let invalid = DOCUMENT
        .replace("\"v\":3", "\"v\":99")
        .replace("\"principal\":\"\\ud800\"", "\"principal\":false");
    let error = ready(load(&ReadStore::document(&invalid, None)?, ID))
        .err()
        .ok_or("unknown logical version accepted")?;
    assert_eq!(
        error.envelope.error.code.as_deref(),
        Some("local_work_custody_invalid")
    );
    let invalid = DOCUMENT.replace("\"principal\":\"\\ud800\"", "\"principal\":false");
    assert_eq!(
        ready(load(&ReadStore::document(&invalid, None)?, ID))
            .err()
            .ok_or("bad context accepted")?
            .envelope
            .error
            .message,
        "Routing session is corrupt."
    );
    let legacy = DOCUMENT.replace("\"v\":3", "\"v\":1");
    assert_eq!(
        ready(load(&ReadStore::document(&legacy, None)?, ID))
            .err()
            .ok_or("legacy scalar domain widened")?
            .envelope
            .error
            .message,
        "Routing session is corrupt."
    );
    Ok(())
}

#[test]
fn full_round_trip_rejects_unknown_missing_and_numeric_shape_before_identity() -> TestResult {
    for invalid in [
        DOCUMENT.replace(
            "\"kernel_override\":null",
            "\"kernel_override\":null,\"unknown\":true",
        ),
        DOCUMENT.replace("\"active\":null,", ""),
        DOCUMENT.replace("\"cursor\":0", "\"cursor\":0.0"),
    ] {
        assert!(ready(load(&ReadStore::document(&invalid, None)?, ID)).is_err());
    }
    let wrong = DOCUMENT.replace(
        "\"request_id\":\"journal-reader\"",
        "\"request_id\":\"another-request\"",
    );
    assert_eq!(
        ready(load(&ReadStore::document(&wrong, None)?, ID))
            .err()
            .ok_or("wrong identity accepted")?
            .envelope
            .error
            .message,
        "Routing session identity mismatch."
    );
    Ok(())
}

#[test]
fn nested_fence_key_is_validated_in_the_full_attempt_record() -> TestResult {
    let attempt = |key: String| {
        serde_json::json!({
            "id":"attempt", "provider":UpstreamProvider::ClaudeCode, "model":"model", "cursor":0,
            "fence":{"Claude":{"account_id":"account","cohort_id":"cohort","credential_fingerprint":"fingerprint","evidence_generation":"generation","workspace_id":null,"checks":[{"key":key,"expected_revision":null}]}},
            "paid":null,"phase":"Prepared","transport_started":false,
            "response_nonce":null,"response_count":0,"workspace_nonce":null,"workspace_continuations":0,
            "known_not_started":false,"observation":null,"provider_request_id":null,"billing":null
        })
    };
    for (key, accepted) in [
        (session_key("source").0, true),
        ("invalid-key".into(), false),
    ] {
        let document = DOCUMENT.replace(
            "\"attempts\":[]",
            &format!("\"attempts\":[{}]", attempt(key)),
        );
        assert_eq!(
            ready(load(&ReadStore::document(&document, None)?, ID)).is_ok(),
            accepted
        );
    }
    Ok(())
}
