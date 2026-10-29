use super::*;
use gateway_cache_experiment_campaign::CacheCampaignOperations;
use gateway_cache_experiment_dispatch::CacheDispatchOperations;
use gateway_cache_experiment_records::model::{STEPS, Sample, Usage};
use gateway_cache_experiment_runner::CacheRunOperations;
pub(super) async fn prepare(
    case: &Case,
    actor: &gateway_authority_actor_currentness::source_actor::SourceNativeRenewalActor<
        '_,
        RemoteAuthorityStore,
    >,
) -> Result<gateway_authority_native_preparation::SourceNativeRenewalAction, GatewayError> {
    let now = TokenClock.now_ms()?;
    let target = case
        .authority
        .prepare_cache_experiment(&"1".repeat(40), now)
        .await?;
    let binding = target.binding();
    let cycle = case
        .authority
        .acquire_cache_cycle(binding, &binding.auth_pool_versionstamp, now)
        .await?;
    let prefix = std::array::from_fn(|index| {
        let write = index == 0 || index == 2;
        let usage = Usage {
            input_tokens: 2560,
            cached_tokens: if write { 0 } else { 2560 },
            cache_write_tokens: if write { 2560 } else { 0 },
            output_tokens: 12,
            total_tokens: 2572,
        };
        Sample {
            step: STEPS[index].into(),
            slot: 1,
            raw_usage: usage.clone(),
            usage,
            elapsed_ms: 12,
        }
    });
    let permit = case
        .authority
        .cache_refresh_permit(&cycle, binding, prefix, &"1".repeat(40), now)
        .await?;
    case.authority
        .source_native_renewals(
            &case.host,
            &case.config,
            &case.identity,
            &case.owner,
            &Cancel,
        )
        .prepare_source_cache_experiment_renewal(actor, &permit)
        .await
}
