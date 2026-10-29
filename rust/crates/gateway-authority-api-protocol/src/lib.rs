//! Exact API policy/window/request data and pure identity guards; no reservation is minted here.
use gateway_authority_contracts::{add, invalid, nonnegative, positive, text};
use gateway_contracts::{
    identity::ApiKeyId, identity::RequestId, model::ModelId, provider::UpstreamProvider,
};
use gateway_source::error::GatewayError;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct ApiKeyPolicy {
    pub v: u32,
    pub key_id: ApiKeyId,
    pub expires_at_ms: i64,
    pub usage_limit_requests: i64,
    pub window_ms: i64,
    pub usage_reset_at_ms: i64,
    pub paid_fallback_enabled: bool,
    pub paid_fallback_limit_microcredits: i64,
    pub paid_fallback_quota_per_credit: i64,
    pub paid_fallback_pricing_checked_at_ms: i64,
    pub paid_fallback_models: Vec<ModelId>,
    #[serde(with = "gateway_authority_paid_control_records::model_map")]
    pub paid_fallback_max_exposure_microcredits: BTreeMap<ModelId, i64>,
}

impl ApiKeyPolicy {
    pub fn validate(&self, key: &ApiKeyId, now: i64) -> Result<(), GatewayError> {
        if self.v != 3 || &self.key_id != key {
            return Err(invalid("API key policy identity is malformed."));
        }
        text(&key.0)?;
        positive(self.window_ms)?;
        nonnegative(self.usage_reset_at_ms)?;
        if self.usage_limit_requests != -1 {
            nonnegative(self.usage_limit_requests)?;
        }
        if self.expires_at_ms != -1 && self.expires_at_ms <= now {
            return Err(GatewayError::new(
                401,
                "API key has expired.",
                Some("invalid_api_key"),
            ));
        }
        Ok(())
    }

    pub fn current_window(&self, now: i64) -> Result<(i64, i64), GatewayError> {
        positive(self.window_ms)?;
        nonnegative(now)?;
        let initial = self
            .usage_reset_at_ms
            .checked_sub(self.window_ms)
            .ok_or_else(|| invalid("Invalid usage window."))?;
        if initial < 0 {
            return Err(invalid(
                "API key usage window starts before the portable epoch.",
            ));
        }
        if now < self.usage_reset_at_ms {
            return Ok((initial, self.usage_reset_at_ms));
        }
        let elapsed = (now - initial) / self.window_ms;
        let duration = elapsed
            .checked_mul(self.window_ms)
            .ok_or_else(|| invalid("API usage window overflow."))?;
        let start = add(initial, duration)?;
        Ok((start, add(start, self.window_ms)?))
    }
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct ApiUsageWindow {
    pub v: u32,
    pub key_id: ApiKeyId,
    pub policy_version: String,
    pub window_start_ms: i64,
    pub window_reset_at_ms: i64,
    pub committed_requests: i64,
    pub reserved_requests: i64,
    pub updated_at_ms: i64,
    /// An index of active reservations permits event-owned reclaim without store scans.
    pub reservations: BTreeMap<String, i64>,
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ApiUsageState {
    Reserved,
    Dispatched,
    Released,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct ApiUsageRequest {
    pub v: u32,
    pub key_id: ApiKeyId,
    pub request_id: RequestId,
    pub route: String,
    pub state: ApiUsageState,
    pub reserved_at_ms: i64,
    pub lease_expires_at_ms: i64,
    pub provider: Option<UpstreamProvider>,
    pub dispatched_at_ms: Option<i64>,
    pub released_at_ms: Option<i64>,
    pub release_reason: Option<String>,
    /// Conservative durable transport intent; only a known pretransport cancellation compensates.
    pub transport_started: bool,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct ApiReservation {
    pub key_id: ApiKeyId,
    pub request_id: RequestId,
    pub route: String,
    pub policy_version: String,
    pub window_start_ms: i64,
    pub window_reset_at_ms: i64,
    #[serde(default)]
    pub deferred: bool,
}

pub fn api_row_matches(row: &ApiUsageRequest, reservation: &ApiReservation) -> bool {
    row.validate().is_ok()
        && row.v == 3
        && row.key_id == reservation.key_id
        && row.request_id == reservation.request_id
        && row.route == reservation.route
}

impl ApiUsageRequest {
    pub fn validate(&self) -> Result<(), GatewayError> {
        if self.v != 3 {
            return Err(invalid("API request version is invalid."));
        }
        text(&self.key_id.0)?;
        text(&self.request_id.0)?;
        text(&self.route)?;
        nonnegative(self.reserved_at_ms)?;
        nonnegative(self.lease_expires_at_ms)?;
        if let Some(at) = self.dispatched_at_ms {
            nonnegative(at)?;
        }
        if let Some(at) = self.released_at_ms {
            nonnegative(at)?;
        }
        match self.state {
            ApiUsageState::Reserved
                if self.provider.is_some()
                    || self.dispatched_at_ms.is_some()
                    || self.released_at_ms.is_some()
                    || self.release_reason.is_some() =>
            {
                Err(invalid("Reserved API request has terminal fields."))
            }
            ApiUsageState::Dispatched
                if self.provider.is_none()
                    || self.dispatched_at_ms.is_none()
                    || self.released_at_ms.is_some()
                    || self.release_reason.is_some() =>
            {
                Err(invalid("Dispatched API request is malformed."))
            }
            ApiUsageState::Released
                if self.provider.is_some()
                    || self.dispatched_at_ms.is_some()
                    || self.released_at_ms.is_none()
                    || self.release_reason.is_none() =>
            {
                Err(invalid("Released API request is malformed."))
            }
            _ => Ok(()),
        }
    }
}

pub fn validate_window(
    window: &ApiUsageWindow,
    reservation: &ApiReservation,
) -> Result<(), GatewayError> {
    if window.v != 3
        || window.key_id != reservation.key_id
        || window.policy_version != reservation.policy_version
        || window.window_start_ms != reservation.window_start_ms
        || window.window_reset_at_ms != reservation.window_reset_at_ms
    {
        return Err(invalid("API key quota aggregate identity is malformed."));
    }
    nonnegative(window.committed_requests)?;
    nonnegative(window.reserved_requests)?;
    nonnegative(window.window_start_ms)?;
    nonnegative(window.window_reset_at_ms)?;
    nonnegative(window.updated_at_ms)?;
    if window.window_reset_at_ms <= window.window_start_ms {
        return Err(invalid("API window reset must follow its start."));
    }
    if window.reserved_requests != window.reservations.len() as i64 {
        return Err(invalid(
            "API quota reservation index disagrees with the aggregate.",
        ));
    }
    Ok(())
}
