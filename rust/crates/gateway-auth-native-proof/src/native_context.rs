//! Actual native request lifetime; only checked authenticators mint a bound context.
use crate::native_auth::AuthenticatedNativeOwner;
use gateway_auth_native_contracts::transport::NativeAuthHeaders;
use gateway_auth_native_wire::control::NATIVE_RENEWAL_MAX_MS;
use gateway_source::error::GatewayError;
use std::{
    sync::Arc,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

/// A cancellation signal only. It carries no authentication or effect authority.
pub struct NativeRequestLifetime<'a> {
    deadline: Instant,
    disconnected: &'a dyn Fn() -> bool,
}
impl<'a> NativeRequestLifetime<'a> {
    pub fn until(deadline: Instant, disconnected: &'a dyn Fn() -> bool) -> Self {
        Self {
            deadline: deadline
                .min(Instant::now() + Duration::from_millis(NATIVE_RENEWAL_MAX_MS as u64)),
            disconnected,
        }
    }
    pub fn cancelled(&self) -> bool {
        Instant::now() >= self.deadline || (self.disconnected)()
    }
    pub fn check(&self) -> Result<(), GatewayError> {
        if self.cancelled() {
            return Err(control_interrupted());
        }
        Ok(())
    }
}

/// Only a real MAC/nonce admission can create this native context.
/// ```compile_fail,E0624
/// use gateway_auth_native_proof::native_context::NativeControlContext;
/// let _ = NativeControlContext::admitted();
/// ```
pub struct NativeControlContext<'a> {
    lifetime: NativeRequestLifetime<'a>,
    original_auth: [String; 5],
    admission_identity: Arc<()>,
}
impl<'a> NativeControlContext<'a> {
    pub(crate) fn admitted(
        lifetime: NativeRequestLifetime<'a>,
        auth: NativeAuthHeaders<'_>,
        admission_identity: Arc<()>,
    ) -> Self {
        Self {
            lifetime,
            original_auth: [
                auth.version.into(),
                auth.key_id.into(),
                auth.time_ms.into(),
                auth.nonce.into(),
                auth.mac.into(),
            ],
            admission_identity,
        }
    }
    pub fn deadline(&self) -> Instant {
        self.lifetime.deadline
    }
    pub fn original_auth_headers(&self) -> NativeAuthHeaders<'_> {
        NativeAuthHeaders {
            version: &self.original_auth[0],
            key_id: &self.original_auth[1],
            time_ms: &self.original_auth[2],
            nonce: &self.original_auth[3],
            mac: &self.original_auth[4],
        }
    }
    pub fn now_ms(&self) -> Result<i64, GatewayError> {
        let value = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .ok()
            .and_then(|duration| i64::try_from(duration.as_millis()).ok())
            .filter(|value| *value <= gateway_contracts::identity::MAX_SAFE_INTEGER)
            .ok_or_else(|| {
                GatewayError::unavailable(
                    "Native control clock is unavailable",
                    "native_clock_unavailable",
                )
            })?;
        Ok(value)
    }
    pub fn cancelled(&self) -> bool {
        self.lifetime.cancelled()
    }
    pub fn check(&self) -> Result<(), GatewayError> {
        self.lifetime.check()
    }
    pub fn require_owner_context(
        &self,
        proof: &AuthenticatedNativeOwner,
    ) -> Result<(), GatewayError> {
        self.require_identity(&proof.admission_identity)
    }
    pub(crate) fn require_identity(&self, identity: &Arc<()>) -> Result<(), GatewayError> {
        if !Arc::ptr_eq(&self.admission_identity, identity) {
            return Err(GatewayError::new(
                403,
                "Native import context does not match the original proof",
                Some("native_capability_forbidden"),
            ));
        }
        Ok(())
    }
}
pub fn control_interrupted() -> GatewayError {
    GatewayError::unavailable(
        "Native control was interrupted; reconcile the original action",
        "native_control_outcome_unknown",
    )
}
pub fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Native operation is unavailable",
        "native_operation_unavailable",
    )
}
/// The original HTTP clock mapping remains distinct from the control context clock.
pub fn now_ms() -> Result<i64, GatewayError> {
    let value = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| unavailable())?
        .as_millis();
    i64::try_from(value).map_err(|_| unavailable())
}
