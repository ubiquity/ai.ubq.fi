//! Complete direct-usage source data, without credentials, storage or host authority.
pub mod readback;
pub mod snapshot;
pub mod state;
/// Original optional target parser: cohort validation remains after account lookup.
pub fn clear_target(
    body: &gateway_source::source_json::SourceJson,
) -> Result<
    (
        Option<gateway_source::js_string::JsString16>,
        Option<gateway_source::source_json::SourceJson>,
    ),
    gateway_source::error::GatewayError,
> {
    use gateway_source::source_serialization as s;
    let value = s::member(body, "account_id");
    if value.as_ref().is_some_and(|v| s::string(v).is_none()) {
        return Err(gateway_source::error::GatewayError::new(
            400,
            "account_id must be a string",
            Some("invalid_request_error"),
        ));
    }
    Ok((
        value.as_ref().and_then(s::string),
        s::member(body, "account_cohort_id"),
    ))
}
