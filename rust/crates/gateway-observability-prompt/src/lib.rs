//! Complete finite borrowed observability operations; no store/proof escape.
mod counters;
pub mod prompt;
pub mod prompt_gate;
mod store;
pub struct PromptAnalytics<'a, S, H> {
    store: &'a S,
    host: &'a H,
}
impl<'a, S, H> PromptAnalytics<'a, S, H> {
    pub fn new(store: &'a S, host: &'a H) -> Self {
        Self { store, host }
    }
}
