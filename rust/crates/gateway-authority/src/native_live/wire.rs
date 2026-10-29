//! Private legacy access-only projection; issuer stays with its original effect.
use serde::{Deserialize, Serialize};

/// Ephemeral access-only projection. No Debug/Clone and no durable-row serialization.
/// Only the fixed authenticated TLS response may serialize it. Never include in evidence.
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct NativeLiveAccess {
    access_token: String,
}
impl NativeLiveAccess {
    pub(super) fn current(access_token: String) -> Self {
        Self { access_token }
    }
    pub fn expose(&self) -> &str {
        &self.access_token
    }
}
impl std::fmt::Debug for NativeLiveAccess {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("NativeLiveAccess([redacted])")
    }
}
