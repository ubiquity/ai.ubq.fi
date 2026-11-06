//! Discovery cache storage and request-owned singleflight release; no provider policy.
use gateway_schema::catalog::ProviderCatalog;
use std::{cell::RefCell, collections::BTreeMap, rc::Rc};

#[derive(Clone)]
pub struct MemoEntry<I> {
    pub identity: I,
    pub snapshot: Option<ProviderCatalog>,
    pub expires_ms: i64,
    pub retry_ms: i64,
    pub published_generation: u64,
    pub shared_request: Option<u64>,
}
impl<I> MemoEntry<I> {
    pub fn empty(identity: I) -> Self {
        Self {
            identity,
            snapshot: None,
            expires_ms: 0,
            retry_ms: 0,
            published_generation: 0,
            shared_request: None,
        }
    }
}
pub struct DiscoveryMemo<P, I> {
    pub entries: BTreeMap<P, MemoEntry<I>>,
    pub next_request: u64,
}
impl<P, I> Default for DiscoveryMemo<P, I> {
    fn default() -> Self {
        Self {
            entries: BTreeMap::new(),
            next_request: 0,
        }
    }
}
/// Dropping a cancelled request releases only that request's memo ownership.
pub struct PendingGuard<P: Ord, I> {
    pub memo: Rc<RefCell<DiscoveryMemo<P, I>>>,
    pub provider: P,
    pub request: u64,
}
impl<P: Ord, I> Drop for PendingGuard<P, I> {
    fn drop(&mut self) {
        if let Some(entry) = self.memo.borrow_mut().entries.get_mut(&self.provider)
            && entry.shared_request == Some(self.request)
        {
            entry.shared_request = None;
        }
    }
}
