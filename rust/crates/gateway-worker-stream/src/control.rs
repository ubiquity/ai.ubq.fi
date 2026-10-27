use gateway_core::error::GatewayError;
use std::task::Waker;

pub const ATTEMPT_DEADLINE_MS: i32 = 1_800_000;

/// Implemented by the existing Worker-owned cancellation object, never request data.
pub trait StreamControl: 'static {
    type Deadline: Unpin + 'static;
    fn signal(&self) -> worker::AbortSignal;
    fn register_waker(&self, waker: &Waker);
    fn client_aborted(&self) -> bool;
    fn set_timeout_ms(&self, milliseconds: i32) -> Result<Self::Deadline, GatewayError>;
    fn abort(&self);
}
