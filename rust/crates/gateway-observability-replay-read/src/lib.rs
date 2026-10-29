//! Complete fixed replay export operations; no raw store/transaction access.
pub mod replay_read;
mod store;
pub struct ReplayReader<'a, S, H> {
    store: &'a S,
    host: &'a H,
}
impl<'a, S, H> ReplayReader<'a, S, H> {
    pub fn new(store: &'a S, host: &'a H) -> Self {
        Self { store, host }
    }
}
