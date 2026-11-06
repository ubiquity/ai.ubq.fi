//! Keep an existing delay's wake target current across successive stream pulls.
use std::future::Future;
use std::pin::Pin;
use std::sync::Arc;
use std::task::{Context, Poll, Wake, Waker};

use futures_util::task::AtomicWaker;

/// The inner delay keeps its original timer and deadline. Its retained waker
/// targets this stable relay rather than a possibly completed earlier pull.
pub struct RelayDelay<F> {
    delay: Pin<Box<F>>,
    wake: Arc<WakeLatest>,
}

struct WakeLatest {
    current: AtomicWaker,
}

impl Wake for WakeLatest {
    fn wake(self: Arc<Self>) {
        self.current.wake();
    }

    fn wake_by_ref(self: &Arc<Self>) {
        self.current.wake();
    }
}

impl<F> RelayDelay<F> {
    pub fn new(delay: F) -> Self {
        Self {
            delay: Box::pin(delay),
            wake: Arc::new(WakeLatest {
                current: AtomicWaker::new(),
            }),
        }
    }
}

impl<F: Future<Output = ()>> Future for RelayDelay<F> {
    type Output = ();

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.get_mut();
        this.wake.current.register(context.waker());
        let stable = Waker::from(Arc::clone(&this.wake));
        let mut stable_context = Context::from_waker(&stable);
        this.delay.as_mut().poll(&mut stable_context)
    }
}
