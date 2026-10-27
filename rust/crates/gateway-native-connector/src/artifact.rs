use gateway_source::error::GatewayError;
/// A compile-time fixture artifact is never an ordinary native service, even
/// when dependency feature unification enabled it without the native feature.
pub fn require_production_artifact() -> Result<(), GatewayError> {
    if gateway_authority::NATIVE_IMPORT_FIXTURE_ENABLED {
        return Err(GatewayError::unavailable(
            "Fixture-enabled artifacts cannot run native services",
            "native_fixture_artifact_forbidden",
        ));
    }
    Ok(())
}
