//! Deserialized access-only wire DATA, never an Authority-issued capability.
use serde::Deserialize;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct NativeLiveAccessWire {
    access_token: String,
}

impl NativeLiveAccessWire {
    pub fn expose(&self) -> &str {
        &self.access_token
    }
}
