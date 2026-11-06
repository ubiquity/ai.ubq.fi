//! A relayed delay keeps its original timer and deadline while waking the latest pull.
use gateway_worker::transport::relay_delay::RelayDelay;
use std::cell::RefCell;
use std::future::Future;
use std::pin::Pin;
use std::rc::Rc;
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::task::{Context, Poll, Wake, Waker};

type TestResult = Result<(), Box<dyn std::error::Error>>;

#[derive(Default)]
struct Counter(AtomicUsize);

impl Wake for Counter {
    fn wake(self: Arc<Self>) {
        self.0.fetch_add(1, Ordering::SeqCst);
    }

    fn wake_by_ref(self: &Arc<Self>) {
        self.0.fetch_add(1, Ordering::SeqCst);
    }
}

#[derive(Default)]
struct Probe {
    created: usize,
    polls: usize,
    drops: usize,
    ready: bool,
    retained: Option<Waker>,
    same_retained_waker: Vec<bool>,
    deadlines: Vec<u64>,
}

/// Models the pinned delay: register once, preserve its deadline, clear on drop.
struct FirstWakerDelay {
    probe: Rc<RefCell<Probe>>,
    deadline: u64,
}

impl Future for FirstWakerDelay {
    type Output = ();

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let mut probe = self.probe.borrow_mut();
        probe.polls += 1;
        probe.deadlines.push(self.deadline);
        let retained = probe
            .retained
            .get_or_insert_with(|| context.waker().clone());
        let equivalent = retained.will_wake(context.waker());
        probe.same_retained_waker.push(equivalent);
        if probe.ready {
            Poll::Ready(())
        } else {
            Poll::Pending
        }
    }
}

impl Drop for FirstWakerDelay {
    fn drop(&mut self) {
        let mut probe = self.probe.borrow_mut();
        probe.drops += 1;
        probe.retained = None;
    }
}

fn original_delay(deadline: u64) -> (FirstWakerDelay, Rc<RefCell<Probe>>) {
    let probe = Rc::new(RefCell::new(Probe {
        created: 1,
        ..Probe::default()
    }));
    (
        FirstWakerDelay {
            probe: Rc::clone(&probe),
            deadline,
        },
        probe,
    )
}

fn fire(probe: &Rc<RefCell<Probe>>) -> TestResult {
    let waker = {
        let mut probe = probe.borrow_mut();
        probe.ready = true;
        probe
            .retained
            .clone()
            .ok_or("the delay retained no waker")?
    };
    waker.wake();
    Ok(())
}

fn poll<F: Future<Output = ()> + Unpin>(delay: &mut F, waker: &Waker) -> Poll<()> {
    Pin::new(delay).poll(&mut Context::from_waker(waker))
}

#[test]
fn unwrapped_control_wakes_only_the_original_task() -> TestResult {
    let (mut delay, probe) = original_delay(17);
    let old = Arc::new(Counter::default());
    let current = Arc::new(Counter::default());
    let old_waker = Waker::from(Arc::clone(&old));
    let current_waker = Waker::from(Arc::clone(&current));
    assert!(poll(&mut delay, &old_waker).is_pending());
    assert!(poll(&mut delay, &current_waker).is_pending());
    fire(&probe)?;
    assert_eq!(old.0.load(Ordering::SeqCst), 1);
    assert_eq!(current.0.load(Ordering::SeqCst), 0);
    assert_eq!(probe.borrow().same_retained_waker, [true, false]);
    Ok(())
}

#[test]
fn existing_timer_wakes_latest_task_without_restarting_deadline() -> TestResult {
    let (original, probe) = original_delay(17);
    let mut delay = RelayDelay::new(original);
    let old = Arc::new(Counter::default());
    let current = Arc::new(Counter::default());
    let old_waker = Waker::from(Arc::clone(&old));
    let current_waker = Waker::from(Arc::clone(&current));
    assert!(poll(&mut delay, &old_waker).is_pending());
    assert!(poll(&mut delay, &current_waker).is_pending());
    fire(&probe)?;
    assert_eq!(old.0.load(Ordering::SeqCst), 0);
    assert_eq!(current.0.load(Ordering::SeqCst), 1);
    assert!(poll(&mut delay, &current_waker).is_ready());
    let probe = probe.borrow();
    assert_eq!(probe.created, 1);
    assert_eq!(probe.polls, 3);
    assert_eq!(probe.deadlines, [17, 17, 17]);
    assert_eq!(probe.same_retained_waker, [true, true, true]);
    Ok(())
}

#[test]
fn drop_preserves_timer_cancellation_without_waking_a_task() {
    let (original, probe) = original_delay(23);
    let mut delay = RelayDelay::new(original);
    let old = Arc::new(Counter::default());
    let current = Arc::new(Counter::default());
    assert!(poll(&mut delay, &Waker::from(Arc::clone(&old))).is_pending());
    assert!(poll(&mut delay, &Waker::from(Arc::clone(&current))).is_pending());
    drop(delay);
    let probe = probe.borrow();
    assert_eq!(probe.created, 1);
    assert_eq!(probe.drops, 1);
    assert_eq!(probe.deadlines, [23, 23]);
    assert!(probe.retained.is_none());
    assert_eq!(old.0.load(Ordering::SeqCst), 0);
    assert_eq!(current.0.load(Ordering::SeqCst), 0);
}

#[test]
fn ready_result_is_forwarded_with_one_original_poll() {
    let (original, probe) = original_delay(29);
    probe.borrow_mut().ready = true;
    let mut delay = RelayDelay::new(original);
    let task = Arc::new(Counter::default());
    assert!(poll(&mut delay, &Waker::from(Arc::clone(&task))).is_ready());
    let probe = probe.borrow();
    assert_eq!(probe.created, 1);
    assert_eq!(probe.polls, 1);
    assert_eq!(probe.deadlines, [29]);
    assert_eq!(task.0.load(Ordering::SeqCst), 0);
}
