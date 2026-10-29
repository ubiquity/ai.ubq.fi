use gateway_source::error::GatewayError;

pub fn require_managed_generation_data(
    bytes: &[u8],
    expected: (&str, &str),
) -> Result<(), GatewayError> {
    let value = serde_json::from_slice(bytes).map_err(|_| unavailable())?;
    let credentials = gateway_auth_credentials::native_credentials::parse_native_document(&value)?;
    if credentials.routing_account_hash() != expected.0
        || credentials.generation_hash() != expected.1
        || credentials.external_user_id.is_some()
    {
        return Err(unavailable());
    }
    Ok(())
}

fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Exclusive native owner custody is unavailable or changed",
        "native_import_owner_exclusion_unavailable",
    )
}
