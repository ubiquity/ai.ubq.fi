//! Complete release-baseline preparation and original awaited experiment orchestration.
mod cycle;
pub mod io;
mod refresh;
mod sample;
use gateway_authority_contracts::{AuthorityClock, validate_time};
use gateway_authority_routing_signal::SignalFactory;
use gateway_cache_experiment_campaign::{
    CacheCampaignFactory, CacheCampaignOperations, promotion::CachePromotionOperations,
};
use gateway_cache_experiment_dispatch::{CacheDispatchFactory, CacheDispatchOperations};
use gateway_cache_experiment_evidence::CacheEvidenceReader;
use gateway_cache_experiment_records::{
    model::{Outcome, TargetBinding, failed, unavailable},
    source::release_valid,
};
use gateway_core::state::AuthorityStore;
use gateway_source::error::GatewayError;
use io::ExperimentIo;
use serde_json::Value;
/// Prepared immediately before the immutable-release telemetry gate. Never caller JSON.
pub struct PreparedTarget {
    target: TargetBinding,
    release_git_sha: String,
}
impl PreparedTarget {
    pub fn binding(&self) -> &TargetBinding {
        &self.target
    }
    pub fn telemetry_provider(&self) -> &str {
        &self.target.telemetry_provider
    }
    pub fn model(&self) -> &str {
        &self.target.model
    }
    pub fn release_git_sha(&self) -> &str {
        &self.release_git_sha
    }
}

pub struct CacheRunService<'a, S> {
    store: &'a S,
    clock: Option<&'a dyn AuthorityClock>,
}
impl<'a, S: AuthorityStore> CacheRunService<'a, S> {
    pub fn new(store: &'a S, clock: Option<&'a dyn AuthorityClock>) -> Self {
        Self { store, clock }
    }
    fn fresh_time(&self, started_at: i64) -> Result<i64, GatewayError> {
        let now = self
            .clock
            .as_ref()
            .ok_or_else(|| {
                gateway_authority_contracts::unavailable(
                    "authority_clock_unavailable",
                    "Time-sensitive authority actions require a trusted current clock.",
                )
            })?
            .now_ms()?;
        validate_time(now)?;
        if now < started_at {
            return Err(gateway_authority_contracts::unavailable(
                "invalid_clock",
                "Authority clock moved backwards.",
            ));
        }
        Ok(now)
    }
}
pub trait CacheRunFactory:
    CacheCampaignFactory
    + CacheDispatchFactory
    + SignalFactory<
        Store = <Self as CacheCampaignFactory>::Store,
        Tokens = <Self as CacheCampaignFactory>::Tokens,
    >
{
    fn cache_runner(&self) -> CacheRunService<'_, <Self as CacheCampaignFactory>::Store>;
}
pub trait CacheRunOperations:
    CacheRunFactory + CacheCampaignOperations + CachePromotionOperations + CacheDispatchOperations
{
    fn prepare_cache_experiment(
        &self,
        release_git_sha: &str,
        now: i64,
    ) -> impl std::future::Future<Output = Result<PreparedTarget, GatewayError>> {
        async move {
            let service = self.cache_runner();
            if !release_valid(release_git_sha) {
                return Err(unavailable(
                    "Prompt-cache scope experiment requires current-release telemetry.",
                ));
            }
            let observed = CacheEvidenceReader::new(service.store, now)?
                .select_current_target()
                .await?;
            let target = observed.target().clone();
            let current = CacheEvidenceReader::new(service.store, service.fresh_time(now)?)?
                .resolve(&target)
                .await?;
            let target=current.target().clone().map_err(|_|unavailable("Prompt-cache scope experiment target changed before its Stage 0 baseline could be checked."))?;
            Ok(PreparedTarget {
                target,
                release_git_sha: release_git_sha.into(),
            })
        }
    }
    fn run_cache_experiment<H: ExperimentIo>(
        &self,
        prepared: PreparedTarget,
        io: &H,
        now: i64,
    ) -> impl std::future::Future<Output = Result<Value, GatewayError>> {
        async move {
            let service = self.cache_runner();

            if prepared.release_git_sha != io.release_git_sha() {
                return Err(unavailable(
                    "Prompt-cache scope experiment requires a current-release Stage 0 telemetry baseline.",
                ));
            }
            let target=self.current_cache_target(&prepared.target,service.fresh_time(now)?).await?.map_err(|_|unavailable("Prompt-cache scope experiment target changed after its Stage 0 telemetry baseline."))?;
            let session = self.cache_session(service.fresh_time(now)?).await?;
            let permit = self
                .acquire_cache_cycle(&target, session.revision(), service.fresh_time(now)?)
                .await?;
            let mut promoted = false;
            let result = cycle::CycleRunner {
                realm: self,
                service: CacheRunService::new(service.store, service.clock),
                permit: &permit,
                io,
                promoted: &mut promoted,
                now,
            }
            .run(target, session)
            .await;
            match result {
                Ok(result) => Ok(result),
                Err(error) => {
                    if !promoted
                        && !matches!(
                            error.envelope.error.code.as_deref(),
                            Some(
                                "prompt_cache_scope_experiment_busy"
                                    | "prompt_cache_scope_experiment_unavailable"
                            )
                        )
                    {
                        let _ = self
                            .finalize_cache_cycle(
                                &permit,
                                Outcome::Failed,
                                None,
                                None,
                                service.fresh_time(now)?,
                            )
                            .await;
                    }
                    if matches!(
                        error.envelope.error.code.as_deref(),
                        Some(
                            "prompt_cache_scope_experiment_busy"
                                | "prompt_cache_scope_experiment_unavailable"
                        )
                    ) {
                        Err(error)
                    } else {
                        Err(failed(&error.envelope.error.message))
                    }
                }
            }
        }
    }
}
impl<
    T: CacheRunFactory + CacheCampaignOperations + CachePromotionOperations + CacheDispatchOperations,
> CacheRunOperations for T
{
}
