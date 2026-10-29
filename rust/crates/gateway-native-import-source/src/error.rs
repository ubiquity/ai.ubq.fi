use gateway_source::error::GatewayError;
pub(crate) fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Native import custody is unavailable; reconcile the original action",
        "native_import_custody_unknown",
    )
}
