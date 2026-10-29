pub mod inspector;
mod pin;
use gateway_source::error::GatewayError;
use pin::PinnedExecutable;
use std::path::Path;
fn verified_tool(path: &Path, digests: &[&str]) -> Result<PinnedExecutable, GatewayError> {
    digests
        .iter()
        .find_map(|digest| PinnedExecutable::verify(path, digest).ok())
        .ok_or_else(fail)
}
fn fail() -> GatewayError {
    GatewayError::unavailable(
        "Exclusive native owner custody is unavailable or changed",
        "native_import_owner_exclusion_unavailable",
    )
}
