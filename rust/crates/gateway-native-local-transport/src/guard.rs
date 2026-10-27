use gateway_source::error::GatewayError;
use std::future::Future;

/// Constructor-owned bridge to the original durable action. The start hook
/// must atomically commit possible-effect intent and refuse replay/unknown.
/// None of these methods can be supplied by deserializing request fields.
pub trait NativeRenewalGuard {
    fn now_ms(&self) -> Result<i64, GatewayError>;
    fn ensure_current(&self) -> impl Future<Output = Result<(), GatewayError>>;
    fn start_refresh(&self) -> impl Future<Output = Result<(), GatewayError>>;
}
