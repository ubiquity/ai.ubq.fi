use gateway_core::{
    error::GatewayError,
    query::QueryStore,
    tuple_key::{ScalarKeyPart, TupleKey},
};
use gateway_extras_contract::types::ExtrasHost;
use gateway_schema::selection::ProviderSelection;
fn invalid() -> GatewayError {
    GatewayError::unavailable(
        "Embeddings durable state is invalid.",
        "embeddings_state_invalid",
    )
}
pub fn now<H: ExtrasHost>(host: &H) -> Result<i64, GatewayError> {
    let now = host.now_ms();
    if (0..=gateway_core::identity::MAX_SAFE_INTEGER).contains(&now) {
        Ok(now)
    } else {
        Err(invalid())
    }
}
pub async fn selection<S: QueryStore>(
    store: &S,
    at: i64,
) -> Result<Option<ProviderSelection>, GatewayError> {
    let key = TupleKey::from_scalar_parts(&[
        ScalarKeyPart::String("uos_ai"),
        ScalarKeyPart::String("provider_selection"),
        ScalarKeyPart::String("v1"),
    ])
    .to_record_key();
    let Some(row) = store.read(&key).await? else {
        return Ok(None);
    };
    row.validate_version()?;
    if row.expires_at_ms.is_some_and(|expiry| expiry <= at) {
        return Ok(None);
    }
    Ok(ProviderSelection::normalize(row.json_value()?))
}
pub fn cancelled() -> GatewayError {
    GatewayError::new(499, "Request was cancelled.", Some("request_cancelled"))
        .with_type("server_error")
        .with_param(None)
}
