//! Private wire/lifetime facts from the real request task, never admission proof.
use std::{cell::Cell, rc::Rc};

pub struct HeadState {
    accepted: Cell<bool>,
    published: Cell<bool>,
    publication_failed: Cell<bool>,
    delivery_lost: Cell<bool>,
}

impl HeadState {
    pub fn new() -> Rc<Self> {
        Rc::new(Self {
            accepted: Cell::new(false),
            published: Cell::new(false),
            publication_failed: Cell::new(false),
            delivery_lost: Cell::new(false),
        })
    }
    /// Sole producer: checked free-hop selection after its actual physical SSE head.
    pub fn accepted_head(&self) {
        self.accepted.set(true);
    }
    pub fn seen(&self) -> bool {
        self.accepted.get()
    }
    pub fn published(&self) {
        self.published.set(true);
    }
    pub fn is_published(&self) -> bool {
        self.published.get()
    }
    pub fn publication_failed(&self) {
        self.publication_failed.set(true);
    }
    pub fn expects_stream(&self) -> bool {
        self.accepted.get() && !self.publication_failed.get()
    }
    /// Only a genuine unfinished task/body drop supplies this observation.
    pub fn delivery_lost(&self) {
        self.delivery_lost.set(true);
    }
    pub fn is_delivery_lost(&self) -> bool {
        self.delivery_lost.get()
    }
}
