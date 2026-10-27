//! Opaque pending-resource retention and local task completion; admission authority stays with its original owner.
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
};
pub type CleanupTask = std::pin::Pin<Box<dyn std::future::Future<Output = ()>>>;
pub struct Pending<T> {
    value: Option<T>,
    abandon: Option<Box<dyn FnOnce(T)>>,
}
impl<T> Pending<T> {
    pub fn get(&self) -> Option<&T> {
        self.value.as_ref()
    }
    pub fn get_mut(&mut self) -> Option<&mut T> {
        self.value.as_mut()
    }
    pub fn take(&mut self) -> Option<T> {
        self.value.take()
    }
    pub fn new(value: T) -> Self {
        Self {
            value: Some(value),
            abandon: None,
        }
    }
    pub fn arm(&mut self, abandon: impl FnOnce(T) + 'static) {
        if self.abandon.is_none() {
            self.abandon = Some(Box::new(abandon));
        }
    }
    pub fn armed(&self) -> bool {
        self.abandon.is_some()
    }
}
impl<T> Drop for Pending<T> {
    fn drop(&mut self) {
        if let Some(value) = self.value.take()
            && let Some(abandon) = self.abandon.take()
        {
            abandon(value);
        }
    }
}

struct CleanupSlot<R> {
    value: RefCell<Option<R>>,
    waiting: Cell<bool>,
    waker: RefCell<Option<std::task::Waker>>,
}
pub struct CleanupReceiver<R>(Rc<CleanupSlot<R>>);
impl<R> std::future::Future for CleanupReceiver<R> {
    type Output = R;
    fn poll(
        self: std::pin::Pin<&mut Self>,
        context: &mut std::task::Context<'_>,
    ) -> std::task::Poll<R> {
        *self.0.waker.borrow_mut() = Some(context.waker().clone());
        self.0
            .value
            .borrow_mut()
            .take()
            .map_or(std::task::Poll::Pending, std::task::Poll::Ready)
    }
}
impl<R> Drop for CleanupReceiver<R> {
    fn drop(&mut self) {
        self.0.waiting.set(false);
        drop(self.0.value.borrow_mut().take());
    }
}
pub fn enqueue<R: 'static>(
    future: impl std::future::Future<Output = R> + 'static,
    schedule: impl FnOnce(CleanupTask),
) -> CleanupReceiver<R> {
    let slot = Rc::new(CleanupSlot {
        value: RefCell::new(None),
        waiting: Cell::new(true),
        waker: RefCell::new(None),
    });
    let sender = slot.clone();
    schedule(Box::pin(async move {
        let result = future.await;
        if sender.waiting.get() {
            *sender.value.borrow_mut() = Some(result);
            let wake = sender.waker.borrow_mut().take();
            if let Some(wake) = wake {
                wake.wake();
            }
        }
    }));
    CleanupReceiver(slot)
}
