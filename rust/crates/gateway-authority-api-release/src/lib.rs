//! Complete checked normal/local release; private error origins never become caller-selected HTTP fields.
mod backend;
use gateway_authority_api_protocol::{
    ApiReservation, ApiUsageRequest, ApiUsageState, ApiUsageWindow, api_row_matches,
    validate_window,
};
use gateway_authority_contracts::{
    QUOTA_RETENTION_MS, TokenSource, invalid, retention, unavailable,
};
use gateway_authority_keys as keys;
use gateway_core::{error::GatewayError, state::AuthorityStore};

#[derive(Clone, Copy)]
pub(crate) enum ReleaseFailureOrigin {
    Malformed,
    ChangedConcurrently,
    Unavailable,
}
pub(crate) struct ReleaseFailure {
    origin: ReleaseFailureOrigin,
    diagnostic: GatewayError,
}
impl ReleaseFailure {
    pub(crate) fn malformed(diagnostic: GatewayError) -> Self {
        Self {
            origin: ReleaseFailureOrigin::Malformed,
            diagnostic,
        }
    }
    pub(crate) fn unavailable(diagnostic: GatewayError) -> Self {
        Self {
            origin: ReleaseFailureOrigin::Unavailable,
            diagnostic,
        }
    }
    pub(crate) fn changed(diagnostic: GatewayError) -> Self {
        Self {
            origin: ReleaseFailureOrigin::ChangedConcurrently,
            diagnostic,
        }
    }
    pub(crate) fn into_diagnostic(self) -> GatewayError {
        self.diagnostic
    }
    fn into_local(self) -> LocalApiReleaseQuotaError {
        LocalApiReleaseQuotaError {
            origin: self.origin,
        }
    }
}

/// Actual completed release/no-op data. Never admission or inference permission.
#[derive(Debug)]
pub struct LocalApiReleaseCompletion {
    released: bool,
}
impl LocalApiReleaseCompletion {
    pub fn was_released(&self) -> bool {
        self.released
    }
}
/// Closed source quota error with no caller-selected status, body or headers.
pub struct LocalApiReleaseQuotaError {
    origin: ReleaseFailureOrigin,
}
impl LocalApiReleaseQuotaError {
    pub fn as_gateway_error(&self) -> GatewayError {
        let message = match self.origin {
            ReleaseFailureOrigin::Malformed => "API key quota reservation is malformed",
            ReleaseFailureOrigin::ChangedConcurrently => {
                "API key quota reservation changed concurrently"
            }
            ReleaseFailureOrigin::Unavailable => "API key quota reservation is unavailable",
        };
        GatewayError::unavailable(message, "api_key_quota_reservation_unavailable")
    }
}
impl std::fmt::Debug for LocalApiReleaseQuotaError {
    fn fmt(&self, out: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        out.debug_tuple("LocalApiReleaseQuotaError")
            .field(&self.as_gateway_error())
            .finish()
    }
}

pub struct ApiReleaseService<'a, S, I> {
    store: &'a S,
    tokens: &'a I,
}
impl<S, I> Copy for ApiReleaseService<'_, S, I> {}
impl<S, I> Clone for ApiReleaseService<'_, S, I> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<'a, S: AuthorityStore, I: TokenSource> ApiReleaseService<'a, S, I> {
    pub fn new(store: &'a S, tokens: &'a I) -> Self {
        Self { store, tokens }
    }
    fn transaction(&self, now: i64) -> Result<backend::Transaction<'_, S, I>, GatewayError> {
        backend::Transaction::new(self.store, self.tokens, now)
    }
    pub(crate) async fn release_api_key_classified(
        self,
        reservation: &ApiReservation,
        known_before_transport: bool,
        reason: &str,
        now: i64,
    ) -> Result<bool, ReleaseFailure> {
        if reservation.deferred {
            return Ok(false);
        }
        for _ in 0..5 {
            let mut tx = self.transaction(now).map_err(ReleaseFailure::unavailable)?;
            let request_key = keys::api_request(
                &reservation.key_id.0,
                &reservation.policy_version,
                reservation.window_start_ms,
                &reservation.request_id.0,
            );
            let window_key = keys::api_window(
                &reservation.key_id.0,
                &reservation.policy_version,
                reservation.window_start_ms,
            );
            let Some(mut row) = tx
                .read_api_release::<ApiUsageRequest>(&request_key)
                .await
                .map_err(classify_release_read)?
            else {
                return Ok(false);
            };
            if !api_row_matches(&row, reservation) {
                return Err(ReleaseFailure::malformed(invalid(
                    "API release identity conflicts.",
                )));
            }
            if row.state == ApiUsageState::Released
                || (row.state == ApiUsageState::Dispatched
                    && (!known_before_transport || row.transport_started))
            {
                return Ok(false);
            }
            let mut window: ApiUsageWindow = tx
                .read_api_release(&window_key)
                .await
                .map_err(classify_release_read)?
                .ok_or_else(|| {
                    ReleaseFailure::malformed(invalid("API quota aggregate is missing."))
                })?;
            validate_window(&window, reservation).map_err(ReleaseFailure::malformed)?;
            if row.state == ApiUsageState::Reserved {
                if window.reserved_requests < 1
                    || window.reservations.remove(&row.request_id.0).is_none()
                {
                    return Err(ReleaseFailure::malformed(invalid(
                        "Missing API reservation capacity.",
                    )));
                }
                window.reserved_requests -= 1;
            } else {
                if window.committed_requests < 1 {
                    return Err(ReleaseFailure::malformed(invalid(
                        "Missing API committed capacity.",
                    )));
                }
                window.committed_requests -= 1;
            }
            row.state = ApiUsageState::Released;
            row.provider = None;
            row.dispatched_at_ms = None;
            row.released_at_ms = Some(now);
            row.release_reason = Some(reason.chars().take(120).collect());
            window.updated_at_ms = now;
            let expiry = Some(
                retention(reservation.window_reset_at_ms, QUOTA_RETENTION_MS)
                    .map_err(ReleaseFailure::unavailable)?,
            );
            tx.put(&request_key, &row, expiry)
                .map_err(ReleaseFailure::unavailable)?;
            tx.put(&window_key, &window, expiry)
                .map_err(ReleaseFailure::unavailable)?;
            if tx.commit().await.map_err(ReleaseFailure::unavailable)? {
                return Ok(true);
            }
        }
        Err(ReleaseFailure::changed(unavailable(
            "api_key_quota_reservation_unavailable",
            "API quota release changed concurrently.",
        )))
    }
}
fn classify_release_read(error: backend::ApiReleaseReadError) -> ReleaseFailure {
    match error {
        backend::ApiReleaseReadError::Backend(error) => ReleaseFailure::unavailable(error),
        backend::ApiReleaseReadError::Record(error) => ReleaseFailure::malformed(error),
    }
}

#[cfg(test)]
mod tests;

pub trait ApiReleaseFactory {
    type Store: AuthorityStore;
    type Tokens: TokenSource;
    fn api_release(&self) -> ApiReleaseService<'_, Self::Store, Self::Tokens>;
}

pub trait ApiReleaseOperations: ApiReleaseFactory {
    fn release_api_key(
        &self,
        reservation: &ApiReservation,
        known_before_transport: bool,
        reason: &str,
        now: i64,
    ) -> impl std::future::Future<Output = Result<bool, GatewayError>> {
        async move {
            let context = self.api_release();
            context
                .release_api_key_classified(reservation, known_before_transport, reason, now)
                .await
                .map_err(ReleaseFailure::into_diagnostic)
        }
    }
    fn release_local_response_api(
        &self,
        reservation: &ApiReservation,
        now: i64,
    ) -> impl std::future::Future<Output = Result<LocalApiReleaseCompletion, LocalApiReleaseQuotaError>>
    {
        async move {
            let context = self.api_release();
            context
                .release_api_key_classified(
                    reservation,
                    false,
                    "route_completed_without_provider_dispatch",
                    now,
                )
                .await
                .map(|released| LocalApiReleaseCompletion { released })
                .map_err(ReleaseFailure::into_local)
        }
    }
}
impl<T: ApiReleaseFactory + ?Sized> ApiReleaseOperations for T {}
