pub mod endpoints;
#[cfg(feature = "native-import-fixture")]
pub mod fixed_fixture;
pub mod launch;
mod pin;
pub mod sandbox_identity;
fn fail() -> gateway_source::error::GatewayError {
    gateway_source::error::GatewayError::unavailable(
        "Exclusive native owner custody is unavailable or changed",
        "native_import_owner_exclusion_unavailable",
    )
}
pub mod service;
