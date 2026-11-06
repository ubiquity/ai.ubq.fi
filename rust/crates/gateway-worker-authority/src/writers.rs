//! Actual completion events close writer groups; abandonment persists a blocker.
use crate::client::RemoteAuthorityStore;
use futures_util::task::AtomicWaker;
use gateway_authority_lifecycle::{
    model::{Blocker, Operation, Permit},
    policy,
};
use gateway_core::error::GatewayError;
use std::{
    cell::{Cell, RefCell},
    future::poll_fn,
    rc::Rc,
    task::Poll,
};

pub struct WriterGroup {
    active: Cell<usize>,
    closed: Cell<bool>,
    blocker: RefCell<Option<Blocker>>,
    waker: AtomicWaker,
}

pub struct Writer {
    group: Rc<WriterGroup>,
    completed: bool,
}

impl WriterGroup {
    pub fn new() -> Rc<Self> {
        Rc::new(Self {
            active: Cell::new(0),
            closed: Cell::new(false),
            blocker: RefCell::new(None),
            waker: AtomicWaker::new(),
        })
    }
    pub fn register(self: &Rc<Self>) -> Result<Writer, GatewayError> {
        if self.closed.get() {
            return Err(policy::refused("Writer group already completed"));
        }
        self.active.set(
            self.active
                .get()
                .checked_add(1)
                .ok_or_else(|| policy::refused("Writer group count overflow"))?,
        );
        Ok(Writer {
            group: self.clone(),
            completed: false,
        })
    }
    pub fn block(&self, blocker: Blocker) {
        self.blocker.borrow_mut().get_or_insert(blocker);
        self.waker.wake();
    }
    fn release(&self, blocker: Option<Blocker>) {
        if let Some(blocker) = blocker {
            self.block(blocker);
        }
        if let Some(next) = self.active.get().checked_sub(1) {
            self.active.set(next);
        } else {
            self.block(Blocker::TaskAbandoned);
        }
        self.waker.wake();
    }
    pub fn retain(
        self: &Rc<Self>,
        store: RemoteAuthorityStore,
        permit: Permit,
        context: &worker::Context,
    ) {
        let group = self.clone();
        context.wait_until(async move {
            let blocker = poll_fn(|cx| {
                group.waker.register(cx.waker());
                if group.active.get() != 0 {
                    return Poll::Pending;
                }
                group.closed.set(true);
                Poll::Ready(*group.blocker.borrow())
            })
            .await;
            if let Err(error) = store
                .coordination(Operation::Finish { permit, blocker })
                .await
            {
                worker::console_error!(
                    "Writer group durable completion remains unconfirmed: {}",
                    error.envelope.error.message
                );
            }
        });
    }
}

impl Writer {
    pub fn complete(mut self) {
        self.completed = true;
        self.group.release(None);
    }
    pub fn fail(mut self, blocker: Blocker) {
        self.completed = true;
        self.group.release(Some(blocker));
    }
}

impl Drop for Writer {
    fn drop(&mut self) {
        if !self.completed {
            self.group.release(Some(Blocker::TaskAbandoned));
        }
    }
}
