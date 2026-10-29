//! Source lease-guarded event/admin sampler. No timer, alarm, or inference operation.
use crate::ObservabilityRuntime;
use gateway_admin_runtime_contracts::capabilities::Context;
use gateway_auth_contracts::types::AuthResponse;
use gateway_capacity_projection::{scalars::DAY, scalars::safe_i64, shapes as shape};
use gateway_core::query::QueryStore;
use gateway_observability_capacity::{capacity::claude_source, capacity::unavailable_claude};
use gateway_observability_contracts::{
    data, source_object::SourceObject, types::ObservabilityHost,
};
use gateway_observability_health::health;
use gateway_source::{
    error::GatewayError, source_json::SourceJson, source_serialization as source,
};
use serde_json::json;
impl<S: QueryStore, H: ObservabilityHost> ObservabilityRuntime<S, H> {
    pub async fn capacity_response(
        &self,
        live: bool,
        context: Context,
    ) -> Result<AuthResponse, GatewayError> {
        let at = data::now(self.host.as_ref())?;
        let inventory = self.host.provider_inventory(&context).await?;
        let cached = self.capacity().persisted_capacity_view().await?;
        let usage_fresh = inventory.claude.accounts.iter().all(|a| {
            a.quota
                .as_ref()
                .and_then(|q| source::member(q, "usage"))
                .as_ref()
                .and_then(gateway_claude_usage_protocol::state::read)
                .is_some_and(|u| u.fresh(at))
        });
        let fresh = cached["cache_state"] != "unavailable"
            && gateway_capacity_projection::scalars::safe_i64(&cached["snapshot_at_ms"])
                .is_some_and(|t| at - t < 30000);
        let mut body = if !live && fresh && usage_fresh {
            cached
        } else {
            match self.capacity().refresh_capacity_view(live).await {
                Ok(v) => v,
                Err(e)
                    if e.envelope.error.code.as_deref() == Some("admin_capability_unavailable") =>
                {
                    return Err(e);
                }
                Err(_) => cached,
            }
        };
        // Rebuild after collection; the same response includes the newly accepted evidence.
        let inventory = self.host.provider_inventory(&context).await?;
        let at = data::now(self.host.as_ref())?;
        let mut source_accounts = Vec::new();
        for account in &inventory.claude.accounts {
            let health = self
                .health()
                .provider_health(&account.target, at)
                .await
                .unwrap_or_else(|_| health::unknown_health());
            source_accounts.push(claude_source(account, &health, at, &|s| {
                self.host.parse_claude_usage_date(s)
            })?);
        }
        let source_active = inventory
            .claude
            .accounts
            .iter()
            .position(|a| {
                Some(a.account_id.as_str()) == inventory.claude.active_account_id.as_deref()
            })
            .and_then(|i| source_accounts.get(i))
            .cloned()
            .unwrap_or(SourceJson::from_serializable(&unavailable_claude(at))?);
        let source_history = self.claude_capacity_history(at).await;
        body["prompt_cache"] = self.prompt().prompt_analytics(&[]).await?;
        body["ledger_growth"] = self.ledger().ledger_growth(at).await;
        let remote = self
            .events()
            .remote_capacity_observations(at)
            .await
            .unwrap_or_default();
        if !remote.is_empty() {
            let local = body["history"].as_array().cloned().unwrap_or_default();
            body["history"] = json!(gateway_capacity_projection::math::merge_remote_history(
                &local,
                &remote,
                |a, b| self.host.compare_labels(a, b)
            )?);
        }
        let mut document = gateway_observability_contracts::source_object::SourceObject::parse(
            &gateway_source::source_json::SourceJson::from_serializable(&body)?,
        )?;
        document.set("claude_accounts", &source_accounts)?;
        document.set("claude", &source_active)?;
        document.set("claude_history", &source_history)?;
        Ok(AuthResponse::source_json(200, document.source_json()?))
    }
    pub async fn capacity_rollups_response(
        &self,
        window_days: u32,
    ) -> Result<AuthResponse, GatewayError> {
        let days = if window_days == 0 {
            90
        } else {
            window_days.min(365)
        };
        let at = data::now(self.host.as_ref())?;
        let window = i64::from(days) * DAY;
        let points = self
            .capacity()
            .capacity_rollup_points((at - window).max(0), at)
            .await
            .ok();
        let claude = self.claude_capacity_rollups((at - window).max(0), at).await;
        let claude = SourceObject::parse(&claude)?;
        let usage = self.claude_usage_rollups((at - window).max(0), at).await;
        let mut document = SourceObject::parse(&SourceJson::from_serializable(
            &json!({"snapshot_at_ms":at,"window_days":days,"retention":{"rollup_bucket_ms":shape::HOUR_MS,"rollup_window_ms":window},"rollup_scan":if points.is_some(){"ok"}else{"unavailable"},"rollups":points.unwrap_or_default()}),
        )?)?;
        document.set("claude_usage_rollups", &usage)?;
        for (output, input) in [
            ("claude_rollup_scan", "scan"),
            ("claude_rollup_truncated", "truncated"),
            ("claude_rollups", "points"),
        ] {
            document.set(output, &claude.get(input))?;
        }
        Ok(AuthResponse::source_json(200, document.source_json()?))
    }
}
