//! Private wire/lifetime facts from the real request task, never admission proof.
use std::{cell::Cell, rc::Rc};

pub(crate) struct HeadState {
    accepted: Cell<bool>,
    published: Cell<bool>,
    publication_failed: Cell<bool>,
    delivery_lost: Cell<bool>,
}

impl HeadState {
    pub(crate) fn new() -> Rc<Self> {
        Rc::new(Self {
            accepted: Cell::new(false),
            published: Cell::new(false),
            publication_failed: Cell::new(false),
            delivery_lost: Cell::new(false),
        })
    }
    /// Sole producer: checked free-hop selection after its actual physical SSE head.
    pub(crate) fn accepted_head(&self) {
        self.accepted.set(true);
    }
    pub(crate) fn seen(&self) -> bool {
        self.accepted.get()
    }
    pub(crate) fn published(&self) {
        self.published.set(true);
    }
    pub(crate) fn is_published(&self) -> bool {
        self.published.get()
    }
    pub(crate) fn publication_failed(&self) {
        self.publication_failed.set(true);
    }
    pub(crate) fn expects_stream(&self) -> bool {
        self.accepted.get() && !self.publication_failed.get()
    }
    /// Only a genuine unfinished task/body drop supplies this observation.
    pub(crate) fn delivery_lost(&self) {
        self.delivery_lost.set(true);
    }
    pub(crate) fn is_delivery_lost(&self) -> bool {
        self.delivery_lost.get()
    }
}
