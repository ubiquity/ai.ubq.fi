//! Trusted configured native mapping DATA; no filesystem, credential or native ownership grant.
use gateway_authority_contracts::text;
use gateway_source::error::GatewayError;
fn fail() -> GatewayError {
    crate::rows::fail()
}
/// Registry construction is a host operation; no request may select a native home.
pub struct RegisteredRenewalOwner {
    id: String,
    home: String,
}
impl RegisteredRenewalOwner {
    pub fn configured(id: String, home: String) -> Result<Self, GatewayError> {
        text(&id)?;
        // Worker validates a configured native POSIX resource name, not its own
        // filesystem. wasm32-unknown-unknown Path::is_absolute always refuses it.
        // Actual native filesystem/custody enforcement remains separately owned.
        #[cfg(target_arch = "wasm32")]
        let absolute = home.starts_with('/');
        #[cfg(not(target_arch = "wasm32"))]
        let absolute = std::path::Path::new(&home).is_absolute();
        if !absolute {
            return Err(fail());
        }
        Ok(Self { id, home })
    }
    pub fn native_owner_id(&self) -> &str {
        &self.id
    }
    /// Read-only configured native mapping, never a caller-selected path or permission.
    pub fn registered_home(&self) -> &str {
        &self.home
    }
}
