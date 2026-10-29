//! Complete finite borrowed observability operations; no store/proof escape.
pub mod ledger;
pub mod ledger_stats;
pub mod records;
mod store;
pub struct RequestLedger<'a, S, H> {
    store: &'a S,
    host: &'a H,
}
impl<'a, S, H> RequestLedger<'a, S, H> {
    pub fn new(store: &'a S, host: &'a H) -> Self {
        Self { store, host }
    }
}
