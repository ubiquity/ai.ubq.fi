//! Tests the production pending-owner/task protocol, not a fabricated Session.
use gateway_admission::handoff::{CleanupTask, Pending, enqueue};
use gateway_admission::{
    AdmissionCancellation, AdmissionClock, AdmissionController, AdmissionRequest, AdmissionResult,
    AdmissionWait, Permit, WaitFuture,
};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use gateway_contracts::lifecycle::TransportObservation;
use gateway_routing_runtime::types::{RoutingError, SourceRefusalBody};
use serde_json::json;
use std::{
    cell::{Cell, RefCell},
    collections::VecDeque,
    future::{Future, poll_fn},
    pin::Pin,
    rc::Rc,
    task::{Context, Poll, Waker},
    time::Duration,
};

struct Clock;
impl AdmissionClock for Clock {
    fn now(&self) -> Duration {
        Duration::ZERO
    }
}
struct Caller;
impl AdmissionCancellation for Caller {
    fn is_cancelled(&self) -> bool {
        false
    }
}
struct Wait;
impl AdmissionWait for Wait {
    fn wait_until<'a>(&'a self, _: Duration, _: &'a dyn AdmissionCancellation) -> WaitFuture<'a> {
        Box::pin(std::future::pending())
    }
}
#[derive(Default)]
struct Gate {
    ready: Cell<bool>,
    waker: RefCell<Option<Waker>>,
}
impl Gate {
    async fn wait(&self) {
        poll_fn(|context| {
            *self.waker.borrow_mut() = Some(context.waker().clone());
            if self.ready.get() {
                Poll::Ready(())
            } else {
                Poll::Pending
            }
        })
        .await;
    }
    fn open(&self) {
        self.ready.set(true);
        let wake = self.waker.borrow().clone();
        if let Some(wake) = wake {
            wake.wake();
        }
    }
}
#[derive(Default)]
struct Trace {
    cleanup: Cell<usize>,
    sinks: Cell<usize>,
    observed: RefCell<Option<TransportObservation>>,
    chosen: RefCell<Vec<RoutingError>>,
}
type Tasks = Rc<RefCell<VecDeque<CleanupTask>>>;
struct Fixture {
    admission: AdmissionController,
    tasks: Tasks,
    cleanup: Rc<Gate>,
    sink: Rc<Gate>,
    trace: Rc<Trace>,
}
fn poll<T, F: Future<Output = T> + ?Sized>(future: Pin<&mut F>) -> Poll<T> {
    future.poll(&mut Context::from_waker(Waker::noop()))
}
fn schedule(tasks: &Tasks) -> impl FnOnce(CleanupTask) + 'static + use<> {
    let tasks = tasks.clone();
    move |task| tasks.borrow_mut().push_back(task)
}
fn original(with_source: bool) -> Result<RoutingError, GatewayError> {
    let mut error: RoutingError =
        GatewayError::new(503, "original routing failure", Some("original_failure")).into();
    error
        .headers
        .insert("x-original-error".into(), "preserved".into());
    if with_source {
        error.source_body = Some(SourceRefusalBody::Json(SourceJson::from_serializable(
            &json!({"error":{"message":"source failure"}}),
        )?));
    }
    Ok(error)
}
impl Fixture {
    fn finish_task(&self, task: &mut CleanupTask) {
        assert!(poll(task.as_mut()).is_pending());
        assert_eq!(self.admission.snapshot().active, 1);
        self.cleanup.open();
        assert!(poll(task.as_mut()).is_pending());
        assert_eq!(self.admission.snapshot().active, 1);
        self.sink.open();
        assert!(poll(task.as_mut()).is_ready());
        assert_eq!(self.admission.snapshot().active, 0);
        assert_eq!(self.trace.cleanup.get(), 1);
        assert_eq!(self.trace.sinks.get(), 1);
    }
    fn new() -> Self {
        Self {
            admission: AdmissionController::new(Rc::new(Clock)),
            tasks: Rc::new(RefCell::new(VecDeque::new())),
            cleanup: Rc::new(Gate::default()),
            sink: Rc::new(Gate::default()),
            trace: Rc::new(Trace::default()),
        }
    }
    fn permit(&self) -> Result<Permit, &'static str> {
        let mut acquired = Box::pin(self.admission.acquire(
            AdmissionRequest {
                principal: Some("native-test".into()),
                request_id: "one-original".into(),
                route: "responses".into(),
                cancellation: Rc::new(Caller),
            },
            &Wait,
        ));
        match poll(acquired.as_mut()) {
            Poll::Ready(AdmissionResult::Admitted { permit, .. }) => Ok(permit),
            _ => Err("Expected a genuine immediate admission"),
        }
    }
    fn task(&self) -> Result<CleanupTask, &'static str> {
        self.tasks
            .borrow_mut()
            .pop_front()
            .ok_or("Expected one owned cleanup task")
    }
    fn completion(
        &self,
        permit: Permit,
        error: RoutingError,
        fails: bool,
    ) -> impl Future<Output = RoutingError> + 'static + use<> {
        let (cleanup, sink, trace) = (self.cleanup.clone(), self.sink.clone(), self.trace.clone());
        async move {
            trace.cleanup.set(trace.cleanup.get() + 1);
            *trace.observed.borrow_mut() = Some(TransportObservation::default());
            assert_eq!(permit.request_id(), "one-original");
            assert_eq!(permit.route(), "responses");
            cleanup.wait().await;
            let settled = if fails {
                Err(
                    GatewayError::new(502, "actual cleanup failure", Some("cleanup_failure"))
                        .into(),
                )
            } else {
                Ok(())
            };
            // The callback supplies the opaque result; Worker error-selection policy is tested by its defining owner.
            let selected = match settled {
                Ok(()) => error,
                Err(error) => error,
            };
            trace.sinks.set(trace.sinks.get() + 1);
            sink.wait().await;
            trace.chosen.borrow_mut().push(selected.clone());
            drop(permit);
            selected
        }
    }
}

#[test]
fn pending_prepare_drop_transfers_original_owners_to_one_cleanup_task()
-> Result<(), Box<dyn std::error::Error>> {
    let fixture = Rc::new(Fixture::new());
    let mut owner = Pending::new(fixture.permit()?);
    let cleanup_fixture = fixture.clone();
    let error = original(false)?;
    owner.arm(move |permit| {
        let receiver = enqueue(
            cleanup_fixture.completion(permit, error, false),
            schedule(&cleanup_fixture.tasks),
        );
        drop(receiver);
    });
    let mut shell = Box::pin(async move {
        let _owner = owner;
        std::future::pending::<()>().await;
    });
    assert!(poll(shell.as_mut()).is_pending());
    drop(shell);
    assert_eq!(fixture.tasks.borrow().len(), 1);
    let mut task = fixture.task()?;
    assert!(poll(task.as_mut()).is_pending());
    assert_eq!(fixture.trace.cleanup.get(), 1);
    assert_eq!(fixture.trace.sinks.get(), 0);
    assert_eq!(fixture.admission.snapshot().active, 1);
    assert_eq!(
        *fixture.trace.observed.borrow(),
        Some(TransportObservation::default())
    );
    fixture.cleanup.open();
    assert!(poll(task.as_mut()).is_pending());
    assert_eq!(fixture.admission.snapshot().active, 1);
    fixture.sink.open();
    assert!(poll(task.as_mut()).is_ready());
    drop(task);
    assert_eq!(fixture.trace.cleanup.get(), 1);
    assert_eq!(fixture.trace.sinks.get(), 1);
    assert_eq!(fixture.admission.snapshot().active, 0);
    assert!(fixture.tasks.borrow().is_empty());
    Ok(())
}

#[test]
fn cancelled_cleanup_receiver_cannot_cancel_owned_cleanup_or_lose_original_error()
-> Result<(), Box<dyn std::error::Error>> {
    for with_source in [true, false] {
        let fixture = Fixture::new();
        let error = original(with_source)?;
        let expected = error.clone();
        let mut owner = Pending::new(fixture.permit()?);
        let permit = owner.take().ok_or("Missing original permit")?;
        let mut receiver = Box::pin(enqueue(
            fixture.completion(permit, error, false),
            schedule(&fixture.tasks),
        ));
        assert!(poll(receiver.as_mut()).is_pending());
        drop(receiver);
        drop(owner);
        let mut task = fixture.task()?;
        assert!(poll(task.as_mut()).is_pending());
        assert_eq!(fixture.admission.snapshot().active, 1);
        fixture.cleanup.open();
        assert!(poll(task.as_mut()).is_pending());
        assert_eq!(fixture.admission.snapshot().active, 1);
        fixture.sink.open();
        assert!(poll(task.as_mut()).is_ready());
        let chosen = fixture.trace.chosen.borrow();
        assert_eq!(chosen.len(), 1);
        assert_eq!(chosen[0].error, expected.error);
        assert_eq!(chosen[0].headers, expected.headers);
        assert_eq!(
            chosen[0]
                .source_body
                .as_ref()
                .map(SourceRefusalBody::as_bytes),
            expected
                .source_body
                .as_ref()
                .map(SourceRefusalBody::as_bytes)
        );
        assert_eq!(fixture.trace.cleanup.get(), 1);
        assert_eq!(fixture.trace.sinks.get(), 1);
        assert_eq!(fixture.admission.snapshot().active, 0);
    }
    Ok(())
}

#[test]
fn cleanup_error_wins_and_published_result_receiver_drop_has_one_sink_owner()
-> Result<(), Box<dyn std::error::Error>> {
    for published_first in [false, true] {
        let fixture = Fixture::new();
        let mut receiver = Some(enqueue(
            fixture.completion(fixture.permit()?, original(true)?, true),
            schedule(&fixture.tasks),
        ));
        let mut task = fixture.task()?;
        assert!(poll(task.as_mut()).is_pending());
        if !published_first {
            drop(receiver.take());
        }
        fixture.cleanup.open();
        assert!(poll(task.as_mut()).is_pending());
        assert_eq!(fixture.admission.snapshot().active, 1);
        fixture.sink.open();
        assert!(poll(task.as_mut()).is_ready());
        drop(receiver.take());
        let chosen = fixture.trace.chosen.borrow();
        assert_eq!(chosen.len(), 1);
        assert_eq!(
            chosen[0].error.envelope.error.code.as_deref(),
            Some("cleanup_failure")
        );
        assert!(chosen[0].source_body.is_none());
        assert!(chosen[0].headers.is_empty());
        assert_eq!(fixture.trace.cleanup.get(), 1);
        assert_eq!(fixture.trace.sinks.get(), 1);
        assert_eq!(fixture.admission.snapshot().active, 0);
        assert!(fixture.tasks.borrow().is_empty());
    }
    Ok(())
}

#[test]
fn consuming_handoff_disarms_pending_drop_without_competing_body_finisher()
-> Result<(), Box<dyn std::error::Error>> {
    let fixture = Fixture::new();
    let mut owner = Pending::new(fixture.permit()?);
    let tasks = fixture.tasks.clone();
    owner.arm(move |permit| {
        tasks.borrow_mut().push_back(Box::pin(async move {
            drop(permit);
        }));
    });
    let destination = owner.take().ok_or("Missing original permit")?;
    drop(owner);
    assert!(fixture.tasks.borrow().is_empty());
    assert_eq!(fixture.admission.snapshot().active, 1);
    assert_eq!(destination.request_id(), "one-original");
    drop(destination);
    assert_eq!(fixture.admission.snapshot().active, 0);
    Ok(())
}

/// Native source-extraction control; actual paid transport/constructor proof stays workerd-owned.
#[test]
fn armed_paid_sse_extraction_failure_and_success_keep_one_recipient()
-> Result<(), Box<dyn std::error::Error>> {
    for fails in [true, false] {
        let fixture = Fixture::new();
        let mut owner = Pending::new(fixture.permit()?);
        let submitted = fixture.tasks.clone();
        owner.arm(move |permit| {
            submitted.borrow_mut().push_back(Box::pin(async move {
                drop(permit);
            }))
        });
        let extracted = if fails {
            Err(original(true)?)
        } else {
            Ok(b"actual source bytes".to_vec())
        };
        match extracted {
            Err(error) => {
                assert_eq!(fixture.admission.snapshot().active, 1);
                assert!(fixture.tasks.borrow().is_empty());
                let permit = owner.take().ok_or("Extraction error lost its owner")?;
                let receiver = enqueue(
                    fixture.completion(permit, error.clone(), false),
                    schedule(&fixture.tasks),
                );
                drop(owner);
                drop(receiver);
                fixture.finish_task(&mut fixture.task()?);
                let chosen = fixture.trace.chosen.borrow();
                assert_eq!(chosen[0].headers, error.headers);
                assert_eq!(
                    chosen[0]
                        .source_body
                        .as_ref()
                        .map(SourceRefusalBody::as_bytes),
                    error.source_body.as_ref().map(SourceRefusalBody::as_bytes)
                );
            }
            Ok(bytes) => {
                let recipient = owner.take().ok_or("Source recipient lost its owner")?;
                drop(owner);
                assert_eq!(bytes, b"actual source bytes");
                assert_eq!(recipient.request_id(), "one-original");
                assert_eq!(fixture.admission.snapshot().active, 1);
                assert!(fixture.tasks.borrow().is_empty());
                drop(recipient);
                assert_eq!(fixture.admission.snapshot().active, 0);
            }
        }
    }
    Ok(())
}

/// A genuine Rust future is dropped while the production Pending owner guards an awaited body.
#[test]
fn armed_buffered_reply_drop_and_lost_error_waiter_keep_cleanup_owner()
-> Result<(), Box<dyn std::error::Error>> {
    let fixture = Rc::new(Fixture::new());
    let mut first = Pending::new(fixture.permit()?);
    let permit = first.take().ok_or("Missing original owner")?;
    let mut selected = Pending::new(permit);
    let sink_fixture = fixture.clone();
    let error = original(true)?;
    selected.arm(move |permit| {
        let receiver = enqueue(
            sink_fixture.completion(permit, error, false),
            schedule(&sink_fixture.tasks),
        );
        drop(receiver);
    });
    drop(first);
    let body = Rc::new(Gate::default());
    let waiting = body.clone();
    let mut handler = Box::pin(async move {
        let _selected = selected;
        waiting.wait().await;
    });
    assert!(poll(handler.as_mut()).is_pending());
    assert_eq!(fixture.admission.snapshot().active, 1);
    assert!(fixture.tasks.borrow().is_empty());
    drop(handler);
    assert!(!body.ready.get());
    assert_eq!(fixture.tasks.borrow().len(), 1);
    fixture.finish_task(&mut fixture.task()?);
    assert!(fixture.tasks.borrow().is_empty());
    assert_eq!(fixture.trace.chosen.borrow().len(), 1);
    assert!(fixture.trace.chosen.borrow()[0].source_body.is_some());
    Ok(())
}
