//! Complete API reserve/defer/dispatch/transport operations over the original borrowed authority.
mod backend;
use gateway_authority_api_protocol::{
    ApiKeyPolicy, ApiReservation, ApiUsageRequest, ApiUsageState, ApiUsageWindow, api_row_matches,
    validate_window,
};
use gateway_authority_contracts::{
    QUOTA_RETENTION_MS, REQUEST_LEASE_MS, TokenSource, add, invalid, quota_error, retention, text,
    unavailable,
};
use gateway_authority_keys as keys;
use gateway_core::{
    error::GatewayError,
    identity::{ApiKeyId, RequestId},
    provider::UpstreamProvider,
    state::AuthorityStore,
};
use std::collections::BTreeMap;

pub struct ApiQuotaService<'a, S, I> {
    store: &'a S,
    tokens: &'a I,
}
impl<S, I> Copy for ApiQuotaService<'_, S, I> {}
impl<S, I> Clone for ApiQuotaService<'_, S, I> {
    fn clone(&self) -> Self {
        *self
    }
}
impl<'a, S: AuthorityStore, I: TokenSource> ApiQuotaService<'a, S, I> {
    pub fn new(store: &'a S, tokens: &'a I) -> Self {
        Self { store, tokens }
    }
    fn transaction(&self, now: i64) -> Result<backend::Transaction<'_, S, I>, GatewayError> {
        backend::Transaction::new(self.store, self.tokens, now)
    }
    pub async fn reserve_api_key(
        self,
        key_id: &ApiKeyId,
        request_id: &RequestId,
        route: &str,
        now: i64,
    ) -> Result<ApiReservation, GatewayError> {
        text(&key_id.0)?;
        text(&request_id.0)?;
        text(route)?;
        for _ in 0..5 {
            let mut tx = self.transaction(now)?;
            let policy: ApiKeyPolicy = tx
                .read_source_control(&keys::api_policy(&key_id.0))
                .await?
                .ok_or_else(|| {
                    GatewayError::new(401, "API key is unavailable.", Some("invalid_api_key"))
                })?;
            policy.validate(key_id, now)?;
            let (start, reset) = policy.current_window(now)?;
            let reservation = ApiReservation {
                key_id: key_id.clone(),
                request_id: request_id.clone(),
                route: route.to_owned(),
                policy_version: format!("v3:{}", policy.window_ms),
                window_start_ms: start,
                window_reset_at_ms: reset,
                deferred: false,
            };
            let window_key = keys::api_window(&key_id.0, &reservation.policy_version, start);
            let request_key =
                keys::api_request(&key_id.0, &reservation.policy_version, start, &request_id.0);
            let mut window: ApiUsageWindow =
                tx.read(&window_key).await?.unwrap_or(ApiUsageWindow {
                    v: 3,
                    key_id: key_id.clone(),
                    policy_version: reservation.policy_version.clone(),
                    window_start_ms: start,
                    window_reset_at_ms: reset,
                    committed_requests: 0,
                    reserved_requests: 0,
                    updated_at_ms: now,
                    reservations: BTreeMap::new(),
                });
            validate_window(&window, &reservation)?;
            if let Some(existing) = tx.read::<ApiUsageRequest>(&request_key).await? {
                if !api_row_matches(&existing, &reservation)
                    || existing.state != ApiUsageState::Reserved
                {
                    return Err(unavailable(
                        "api_key_quota_reservation_unavailable",
                        "API key quota request identity is already consumed or conflicts.",
                    ));
                }
                if tx.commit().await? {
                    return Ok(reservation);
                } else {
                    continue;
                }
            }
            let mut reclaimed = false;
            if policy.usage_limit_requests != -1
                && add(window.committed_requests, window.reserved_requests)?
                    >= policy.usage_limit_requests
            {
                let expired: Vec<_> = window
                    .reservations
                    .iter()
                    .filter(|(_, expiry)| **expiry <= now)
                    .map(|(id, _)| id.clone())
                    .collect();
                for id in expired {
                    let key = keys::api_request(&key_id.0, &reservation.policy_version, start, &id);
                    let mut row: ApiUsageRequest = tx
                        .read(&key)
                        .await?
                        .ok_or_else(|| invalid("API quota reservation is missing."))?;
                    if row.state != ApiUsageState::Reserved || row.lease_expires_at_ms > now {
                        return Err(invalid("API quota reservation index is malformed."));
                    }
                    row.state = ApiUsageState::Released;
                    row.released_at_ms = Some(now);
                    row.release_reason = Some("lease_expired".to_owned());
                    tx.put(&key, &row, Some(retention(reset, QUOTA_RETENTION_MS)?))?;
                    window.reservations.remove(&id);
                    window.reserved_requests -= 1;
                    reclaimed = true;
                }
                if reclaimed {
                    window.updated_at_ms = now;
                    tx.put(
                        &window_key,
                        &window,
                        Some(retention(reset, QUOTA_RETENTION_MS)?),
                    )?;
                    tx.commit().await?;
                    continue;
                }
                if tx.commit().await? {
                    return Err(quota_error(
                        "API key usage limit exceeded.",
                        "rate_limit_exceeded",
                    ));
                } else {
                    continue;
                }
            }
            let lease = add(now, REQUEST_LEASE_MS)?;
            let row = ApiUsageRequest {
                v: 3,
                key_id: key_id.clone(),
                request_id: request_id.clone(),
                route: route.to_owned(),
                state: ApiUsageState::Reserved,
                reserved_at_ms: now,
                lease_expires_at_ms: lease,
                provider: None,
                dispatched_at_ms: None,
                released_at_ms: None,
                release_reason: None,
                transport_started: false,
            };
            window.reserved_requests = add(window.reserved_requests, 1)?;
            window.reservations.insert(request_id.0.clone(), lease);
            window.updated_at_ms = now;
            let expiry = Some(retention(reset, QUOTA_RETENTION_MS)?);
            tx.put(&request_key, &row, expiry)?;
            tx.put(&window_key, &window, expiry)?;
            if tx.commit().await? {
                return Ok(reservation);
            }
        }
        Err(unavailable(
            "api_key_quota_reservation_unavailable",
            "API key quota ledger changed concurrently.",
        ))
    }

    /// Matches handler deferWhenFull: local compaction/cache paths may finish without quota.
    /// The runtime must resolve this handle immediately before its first provider dispatch.
    pub async fn reserve_api_key_deferred(
        self,
        key_id: &ApiKeyId,
        request_id: &RequestId,
        route: &str,
        now: i64,
    ) -> Result<ApiReservation, GatewayError> {
        match self.reserve_api_key(key_id, request_id, route, now).await {
            Ok(reservation) => Ok(reservation),
            Err(error) if error.status == 429 => {
                for _ in 0..5 {
                    let mut tx = self.transaction(now)?;
                    let policy: ApiKeyPolicy = tx
                        .read_source_control(&keys::api_policy(&key_id.0))
                        .await?
                        .ok_or_else(|| {
                            unavailable(
                                "api_key_quota_reservation_unavailable",
                                "API policy is missing.",
                            )
                        })?;
                    policy.validate(key_id, now)?;
                    let (start, reset) = policy.current_window(now)?;
                    let reservation = ApiReservation {
                        key_id: key_id.clone(),
                        request_id: request_id.clone(),
                        route: route.to_owned(),
                        policy_version: format!("v3:{}", policy.window_ms),
                        window_start_ms: start,
                        window_reset_at_ms: reset,
                        deferred: true,
                    };
                    if tx.commit().await? {
                        return Ok(reservation);
                    }
                }
                Err(unavailable(
                    "api_key_quota_reservation_unavailable",
                    "Deferred API policy changed concurrently.",
                ))
            }
            Err(error) => Err(error),
        }
    }

    pub async fn resolve_deferred_api(
        self,
        reservation: &ApiReservation,
        now: i64,
    ) -> Result<ApiReservation, GatewayError> {
        if reservation.deferred {
            self.reserve_api_key(
                &reservation.key_id,
                &reservation.request_id,
                &reservation.route,
                now,
            )
            .await
        } else {
            Ok(reservation.clone())
        }
    }

    /// Returns true only for this reservation's first charge; all retries share that charge.
    pub async fn dispatch_api_key(
        self,
        reservation: &ApiReservation,
        provider: UpstreamProvider,
        now: i64,
    ) -> Result<bool, GatewayError> {
        if reservation.deferred {
            return Err(unavailable(
                "api_key_quota_reservation_unavailable",
                "Resolve deferred API admission before provider dispatch.",
            ));
        }
        for _ in 0..5 {
            let mut tx = self.transaction(now)?;
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
            let mut row: ApiUsageRequest = tx.read(&request_key).await?.ok_or_else(|| {
                unavailable(
                    "api_key_quota_reservation_unavailable",
                    "API key reservation is missing.",
                )
            })?;
            if !api_row_matches(&row, reservation) || row.state == ApiUsageState::Released {
                return Err(unavailable(
                    "api_key_quota_reservation_unavailable",
                    "API key reservation is consumed.",
                ));
            }
            if row.state == ApiUsageState::Dispatched {
                if tx.commit().await? {
                    return Ok(false);
                } else {
                    continue;
                }
            }
            let mut window: ApiUsageWindow = tx
                .read(&window_key)
                .await?
                .ok_or_else(|| invalid("API quota aggregate is missing."))?;
            validate_window(&window, reservation)?;
            if window.reserved_requests < 1 || !window.reservations.contains_key(&row.request_id.0)
            {
                return Err(invalid("API quota has no reserved capacity."));
            }
            row.state = ApiUsageState::Dispatched;
            row.provider = Some(provider);
            row.dispatched_at_ms = Some(now);
            window.reserved_requests -= 1;
            window.reservations.remove(&row.request_id.0);
            window.committed_requests = add(window.committed_requests, 1)?;
            window.updated_at_ms = now;
            let expiry = Some(retention(
                reservation.window_reset_at_ms,
                QUOTA_RETENTION_MS,
            )?);
            tx.put(&request_key, &row, expiry)?;
            tx.put(&window_key, &window, expiry)?;
            if tx.commit().await? {
                return Ok(true);
            }
        }
        Err(unavailable(
            "api_key_quota_reservation_unavailable",
            "API quota dispatch changed concurrently.",
        ))
    }

    pub async fn mark_api_transport_started(
        self,
        reservation: &ApiReservation,
        now: i64,
    ) -> Result<(), GatewayError> {
        for _ in 0..5 {
            let mut tx = self.transaction(now)?;
            let key = keys::api_request(
                &reservation.key_id.0,
                &reservation.policy_version,
                reservation.window_start_ms,
                &reservation.request_id.0,
            );
            let mut row: ApiUsageRequest = tx
                .read(&key)
                .await?
                .ok_or_else(|| invalid("API dispatch is missing."))?;
            if !api_row_matches(&row, reservation) || row.state != ApiUsageState::Dispatched {
                return Err(invalid("API dispatch is not current."));
            }
            row.transport_started = true;
            tx.put(
                &key,
                &row,
                Some(retention(
                    reservation.window_reset_at_ms,
                    QUOTA_RETENTION_MS,
                )?),
            )?;
            if tx.commit().await? {
                return Ok(());
            }
        }
        Err(unavailable(
            "api_key_quota_reservation_unavailable",
            "API dispatch changed concurrently.",
        ))
    }
}
