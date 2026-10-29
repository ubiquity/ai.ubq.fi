//! Complete fixed reset/overage/cache diagnostic settings; no provider credentials or redemption.
pub mod settings;
mod store;
pub struct ControlDiagnostics<'a, S, H> {
    store: &'a S,
    host: &'a H,
}
impl<'a, S, H> ControlDiagnostics<'a, S, H> {
    pub fn new(store: &'a S, host: &'a H) -> Self {
        Self { store, host }
    }
    fn prompt(&self) -> gateway_observability_prompt::PromptAnalytics<'_, S, H> {
        gateway_observability_prompt::PromptAnalytics::new(self.store, self.host)
    }
}
