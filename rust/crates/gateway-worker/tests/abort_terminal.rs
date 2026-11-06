//! A client abort mid-stream is persisted as the source cancelled terminal: it keeps its
//! interrupted delivery and writes no administrative error row, mirroring src/handler/terminal-log.ts
//! (`resolveSentinelClientFailureObservation` drops the failure kind of a `cancelled` terminal and
//! `recordAdminError` records only failures) and src/provider/stream-relay.ts `cancel`.
use gateway_core::lifecycle::{
    BodyOutcome, DeliveryOutcome, ReaderTermination, SemanticTerminal, TransportObservation,
};
use gateway_worker::inference_wire::admin_terminal;
use gateway_worker::transport::stream::{SourceEnd, end_facts, observed_delivery, piped};

fn ended(client: bool, deadline: bool) -> TransportObservation {
    let (reader, body) = end_facts(SourceEnd::Eof, client, deadline);
    TransportObservation {
        reader_termination: Some(reader),
        body: Some(body),
        delivery: observed_delivery(client),
        ..TransportObservation::default()
    }
}

#[test]
fn a_platform_client_abort_is_cancelled_interrupted_and_writes_no_error_row() {
    let observation = ended(true, false);
    assert_eq!(observation.delivery, DeliveryOutcome::Interrupted);
    assert_eq!(observation.body, Some(BodyOutcome::Interrupted));
    let row = admin_terminal(observation, true, Some("error".into()), Some("x".into()));
    assert_eq!(row, (Some("cancelled".into()), None));
}

#[test]
fn a_native_pipe_cancellation_without_the_request_signal_is_still_cancelled() {
    // Without a platform request-signal abort the delivery stays unobserved, never inferred.
    let observation = piped(TransportObservation::default(), true, false);
    assert_eq!(observation.delivery, DeliveryOutcome::Unobserved);
    assert_eq!(
        admin_terminal(observation, true, None, None),
        (Some("cancelled".into()), None)
    );
}

#[test]
fn failures_that_are_not_client_aborts_keep_their_error_rows() {
    let eof = TransportObservation {
        reader_termination: Some(ReaderTermination::Eof),
        ..TransportObservation::default()
    };
    assert_eq!(
        admin_terminal(eof, true, None, None),
        (Some("eof".into()), None)
    );
    assert_eq!(
        admin_terminal(ended(false, true), true, None, None),
        (Some("deadline".into()), Some("gateway_timeout".into()))
    );
    let completed = TransportObservation {
        semantic_terminal: Some(SemanticTerminal::Completed),
        reader_termination: Some(ReaderTermination::Eof),
        ..TransportObservation::default()
    };
    assert_eq!(
        admin_terminal(completed, true, Some("done".into()), None),
        (Some("response.completed".into()), None)
    );
}
