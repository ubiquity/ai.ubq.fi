use gateway_authority::store::AuthorityClock;
use gateway_core::{
    error::GatewayError,
    lifecycle::TransportObservation,
    source_control::decode_source_control_record,
    state::{AuthorityStore, RecordKey},
    tuple_key::{ScalarKeyPart, TupleKey},
};
use gateway_providers::policy::HopFailure;
use gateway_routing_runtime::{
    session::RoutingSession,
    types::{AttemptFailure, FailureOrigin, RoutingHost, TransportState},
};
use gateway_routing_runtime_test_support::{
    exec::run,
    host::Host,
    input::{NOW, input},
    seed::{seed_key, window},
    store::Store,
};
use gateway_wait_observation::outcome::WaitOutcome;
use serde_json::Value;
use std::sync::atomic::Ordering;

type TestResult = Result<(), Box<dyn std::error::Error>>;
struct FaultCase {
    id: &'static str,
    start: bool,
    cancel_after: Option<i64>,
    expected_code: &'static str,
}
fn clock_error() -> GatewayError {
    GatewayError::unavailable(
        "The admission clock is unavailable.",
        "admission_clock_unavailable",
    )
}
fn session_key(id: &str) -> RecordKey {
    TupleKey::from_scalar_parts(&[
        ScalarKeyPart::String("uos_ai"),
        ScalarKeyPart::String("routing_runtime"),
        ScalarKeyPart::String("v1"),
        ScalarKeyPart::String("session"),
        ScalarKeyPart::String(id),
    ])
    .to_record_key()
}
async fn journal(store: &Store, id: &str) -> Result<Value, Box<dyn std::error::Error>> {
    let record = store
        .read(&session_key(id))
        .await?
        .ok_or("missing original routing journal")?;
    Ok(decode_source_control_record(&record)?.deserialize()?)
}
async fn route_fault(case: FaultCase) -> TestResult {
    let store = Store::new();
    seed_key(&store);
    let mut host = Host::new(&store);
    if case.start {
        host.wait_start_clock_error = Some(clock_error());
    } else {
        host.wait_end_clock_error = Some(clock_error());
    }
    host.cancel_wait_after_ms = case.cancel_after;
    let session = RoutingSession::begin(
        input(case.id, "deepseek-ai/DeepSeek-V4.1-Flash", &["lithos"]),
        store.clone(),
        host,
        store.clone(),
        store.clone(),
    )
    .await?;
    let attempt = session
        .next_attempt()
        .await?
        .ok_or("missing actual admitted attempt")?;
    session.before_dispatch(&attempt).await?;
    session.transport_started(&attempt).await?;
    let failure = AttemptFailure {
        origin: FailureOrigin::Provider,
        status: Some(429),
        headers: [("retry-after".into(), "60".into())].into(),
        body: Vec::new(),
        body_complete: false,
        failure: HopFailure::Status(429),
        transport: TransportState::Started,
        semantic_committed: false,
        cancelled: false,
    };
    let error = match session.refusal(&attempt, failure).await {
        Err(error) => error,
        Ok(decision) => {
            return Err(
                format!("clock fault must stop before retry/fallback: {decision:?}").into(),
            );
        }
    };
    assert_eq!(
        error.error.envelope.error.code.as_deref(),
        Some(case.expected_code)
    );
    // This counter sums observations; an unknown duration contributes no fabricated sample.
    assert_eq!(session.elapsed_wait_ms(), 0);
    let actual_fixture_time = store.now_ms()?;
    if case.start {
        assert_eq!(
            actual_fixture_time, NOW,
            "a start fault must not execute the finite wait"
        );
    } else if let Some(elapsed) = case.cancel_after {
        assert_eq!(
            actual_fixture_time,
            NOW + elapsed,
            "the actual finite cancellation result remains"
        );
    } else {
        assert!(
            actual_fixture_time > NOW,
            "the wait occurred even though its completion clock failed"
        );
    }
    let before = journal(&store, case.id).await?;
    let attempts = before["attempts"]
        .as_array()
        .ok_or("missing real attempt inventory")?;
    assert_eq!(attempts.len(), 1);
    assert_eq!(before["cursor"], 0);
    assert!(attempts.iter().all(|attempt| attempt["paid"].is_null()));
    session.finish(TransportObservation::default()).await?;
    let finished = journal(&store, case.id).await?;
    assert_eq!(finished["finished"], true);
    let terminal = finished["terminal_intent"].clone();
    assert_ne!(terminal["delivery"], "delivered");
    assert_ne!(terminal["semantic_terminal"], "response.completed");
    let first_window = window(&store);
    assert_eq!(first_window["committed_requests"], 1);
    assert_eq!(first_window["reserved_requests"], 0);
    session.finish(TransportObservation::default()).await?;
    assert_eq!(
        window(&store),
        first_window,
        "repeated cleanup cannot settle the original admission twice"
    );
    assert_eq!(journal(&store, case.id).await?["terminal_intent"], terminal);
    Ok(())
}
#[test]
fn start_clock_failure_performs_no_wait_or_retry_and_finishes_original_once() -> TestResult {
    run(route_fault(FaultCase {
        id: "clock-start-fault",
        start: true,
        cancel_after: None,
        expected_code: "admission_clock_unavailable",
    }))
}
#[test]
fn completion_clock_failure_retains_real_wait_without_inventing_elapsed_or_retry() -> TestResult {
    run(route_fault(FaultCase {
        id: "clock-end-fault",
        start: false,
        cancel_after: None,
        expected_code: "admission_clock_unavailable",
    }))
}
#[test]
fn completion_clock_failure_preserves_original_cancellation_and_single_cleanup() -> TestResult {
    run(route_fault(FaultCase {
        id: "clock-cancel-fault",
        start: false,
        cancel_after: Some(4),
        expected_code: "fixture_cancelled",
    }))
}
#[test]
fn start_clock_failure_keeps_actual_prior_cancellation_without_attempting_wait() -> TestResult {
    run(async {
        let store = Store::new();
        let mut host = Host::new(&store);
        host.wait_start_clock_error = Some(clock_error());
        host.cancelled.store(true, Ordering::SeqCst);
        match host.wait(60_000).await {
            WaitOutcome::NotStarted { error } => {
                assert_eq!(
                    error.envelope.error.code.as_deref(),
                    Some("fixture_cancelled")
                );
            }
            WaitOutcome::Attempted { .. } => {
                return Err("a start clock failure attempted a wait".into());
            }
        }
        assert_eq!(store.now_ms()?, NOW);
        Ok(())
    })
}
