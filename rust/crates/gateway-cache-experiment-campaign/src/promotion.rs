//! One source catalog/runtime publication CAS and the original awaited final evidence transition.
use crate::{CacheCampaignFactory, CacheCampaignOperations, CyclePermit};
use gateway_auth_credentials::provider_credentials::codex_source_pool_key;
use gateway_authority_contracts::add;
use gateway_cache_experiment_records::{
    model::{LEASE_MS, Outcome, PROVIDER, Reason, Scope, TargetBinding, failed},
    source::{catalog_key, json as source_json, owns, record_key, runtime_key, set_member},
};
use gateway_contracts::model::ModelId;
use gateway_schema::{
    runtime_config::{
        build_runtime_config, default_model, default_reasoning_effort, normalize_runtime_config,
    },
    snapshot::ModelSnapshot,
};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde_json::{Value, json};
pub trait CachePromotionOperations: CacheCampaignFactory {
    fn promote_cache_scope(
        &self,
        permit: &CyclePermit,
        target: &TargetBinding,
        promoted: &mut bool,
        now: i64,
    ) -> impl std::future::Future<Output = Result<Value, GatewayError>> {
        async move {
            let service = self.cache_campaign();

            let Some(scope) = &permit.state.pending_scope else {
                return service
                    .finalize_cache_cycle(
                        permit,
                        Outcome::Inconclusive,
                        None,
                        Some(Reason::CycleDisagreement),
                        now,
                    )
                    .await;
            };
            for _ in 0..3 {
                let mut tx = service.transaction(now)?;
                let catalog_key = catalog_key();
                let runtime_key = runtime_key();
                let lease_key = record_key("cycle_lease", target);
                let raw = tx.read_source_control::<SourceJson>(&catalog_key).await?;
                let runtime = tx.read_source_control::<SourceJson>(&runtime_key).await?;
                let lease = tx.read::<Value>(&lease_key).await?;
                let _: Option<Value> = tx.read(&codex_source_pool_key()).await?;
                let current = service.fresh_time(now)?;
                let snapshot = raw
                    .as_ref()
                    .and_then(|v| v.deserialize::<ModelSnapshot>().ok());
                let normalized = runtime.as_ref().and_then(normalize_runtime_config);
                let reason = if !owns(&lease, &permit.cycle_owner, current) {
                    Some(Reason::PromotionConflict)
                } else if tx.revision(&codex_source_pool_key()).as_deref()
                    != Some(target.auth_pool_versionstamp.as_str())
                {
                    Some(Reason::AuthPoolDrift)
                } else if snapshot.as_ref().is_none_or(|s| {
                    gateway_schema::reasoning::js_trim_source(&s.source)
                        .as_units()
                        .is_empty()
                        || s.updated_at_ms <= 0
                }) {
                    Some(Reason::PromotionConflict)
                } else if tx.revision(&catalog_key).as_deref()
                    != Some(target.catalog_versionstamp.as_str())
                    || snapshot.as_ref().is_none_or(|s| {
                        s.unique(&ModelId::from_scalar(&target.model)).is_none()
                            || !s.scope_experiment_eligible(&ModelId::from_scalar(&target.model))
                    })
                {
                    Some(Reason::TargetCatalogDrift)
                } else if normalized.is_none() {
                    Some(Reason::PromotionConflict)
                } else if tx.revision(&runtime_key).as_deref()
                    != Some(target.runtime_versionstamp.as_str())
                {
                    Some(Reason::RuntimeDrift)
                } else {
                    None
                };
                if let Some(reason) = reason {
                    return service
                        .finalize_cache_cycle(
                            permit,
                            Outcome::Inconclusive,
                            None,
                            Some(reason),
                            current,
                        )
                        .await;
                }
                let snapshot = snapshot.ok_or_else(|| {
                    failed("Prompt-cache scope experiment snapshot is unavailable.")
                })?;
                let normalized = normalized.ok_or_else(|| {
                    failed("Prompt-cache scope experiment runtime is unavailable.")
                })?;
                let (next_snapshot, next_runtime) =
                    publication_plan(&snapshot, &normalized, scope, target, current)?;
                let (Some(next_snapshot), Some(next_runtime)) = (next_snapshot, next_runtime)
                else {
                    return service
                        .finalize_cache_cycle(
                            permit,
                            Outcome::Inconclusive,
                            None,
                            Some(Reason::PromotionConflict),
                            current,
                        )
                        .await;
                };
                let mut next_raw = raw.ok_or_else(|| {
                    failed("Prompt-cache scope experiment snapshot is unavailable.")
                })?;
                set_member(&mut next_raw, "models", source_json(&next_snapshot.models))?;
                tx.put_source_control(&catalog_key, &next_raw, None)?;
                tx.put_source_control(&runtime_key, &next_runtime, None)?;
                tx.put(
                    &lease_key,
                    &json!({"owner":permit.cycle_owner,"lease_until_ms":add(current,LEASE_MS)?}),
                    Some(add(current, LEASE_MS * 2)?),
                )?;
                if tx.commit().await? {
                    *promoted = true;
                    return service
                        .finalize_cache_cycle(
                            permit,
                            Outcome::Completed,
                            None,
                            None,
                            service.fresh_time(now)?,
                        )
                        .await;
                }
            }
            service
                .finalize_cache_cycle(
                    permit,
                    Outcome::Inconclusive,
                    None,
                    Some(Reason::PromotionConflict),
                    service.fresh_time(now)?,
                )
                .await
        }
    }
}
impl<T: CacheCampaignFactory> CachePromotionOperations for T {}
fn publication_plan(
    snapshot: &ModelSnapshot,
    normalized: &SourceJson,
    scope: &Scope,
    target: &TargetBinding,
    current: i64,
) -> Result<(Option<ModelSnapshot>, Option<SourceJson>), GatewayError> {
    let default = snapshot.preserve_default(default_model(normalized).as_ref());
    let mut published = serde_json::to_value(scope)
        .map_err(|_| failed("Prompt-cache scope experiment scope is invalid."))?;
    published["reproducible_cycles"] = json!(3);
    published["source"] = json!("live_probe");
    published["verified_at_ms"] = json!(current);
    let next_snapshot = snapshot.with_prompt_cache_scope(
        &ModelId::from_scalar(&target.model),
        PROVIDER,
        &source_json(&published),
    );
    let next_runtime =
        default
            .as_ref()
            .zip(next_snapshot.as_ref())
            .and_then(|(default, snapshot)| {
                build_runtime_config(
                    snapshot,
                    Some(default),
                    default_reasoning_effort(normalized).as_ref(),
                    current,
                )
                .ok()
            });
    Ok((next_snapshot, next_runtime))
}
