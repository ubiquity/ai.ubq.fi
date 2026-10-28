//! Administrative defaults, selection, debug and principal settings.
pub mod defaults;
pub mod state;

pub struct SettingsAdmin<'a, S, H, C> {
    pub store: &'a S,
    pub host: &'a H,
    pub capabilities: &'a C,
    pub config: &'a gateway_admin_runtime_contracts::RuntimeConfig,
}
