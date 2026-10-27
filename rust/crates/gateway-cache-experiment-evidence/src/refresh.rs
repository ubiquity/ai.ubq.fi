//! Exact original refresh prefix, campaign, lease and inventory predicate over the same snapshot.
use gateway_cache_experiment_records::{
    RefreshContext,
    source::{campaign_owned, owns, record_key, release_valid},
};
pub(crate) async fn validate_refresh_context<S: gateway_core::state::AuthorityStore>(
    tx: &mut crate::cache::ReadCache<'_, S>,
    context: &RefreshContext,
    now: i64,
) -> Result<(), gateway_core::error::GatewayError> {
    use gateway_cache_experiment_records::model::{failed, sample_signal};
    context.target.validate()?;
    if context.v != 1
        || !release_valid(&context.release_git_sha)
        || context.slot != 1
        || !(1..=3).contains(&context.cycle)
        || context.source_pool_revision != context.target.auth_pool_versionstamp
    {
        return Err(failed(
            "Prompt-cache scope experiment refresh context is invalid.",
        ));
    }
    for (index, sample) in context.prefix.iter().enumerate() {
        if !sample.valid(index) || sample_signal(&context.prefix[..index], sample, index).is_err() {
            return Err(failed(
                "Prompt-cache scope experiment refresh prefix is invalid.",
            ));
        }
    }
    let (state, _) = crate::campaign::records(tx, &context.target).await?;
    let state = state.ok_or_else(|| failed("Prompt-cache scope experiment lost its lease."))?;
    if state.next_cycle != context.cycle
        || state.campaign_owner != context.campaign_owner
        || state.auth_pool_versionstamp != context.source_pool_revision
        || !state.target.same_definition(&context.target)
        || state.expires_at_ms <= now
    {
        return Err(failed(
            "Prompt-cache scope experiment refresh campaign changed.",
        ));
    }
    let state_key = record_key("state", &context.target);
    let cycle_key = record_key("cycle_lease", &context.target);
    let campaign_key = record_key("campaign", &context.target);
    let cycle = tx.read::<serde_json::Value>(&cycle_key).await?;
    let campaign = tx.read::<serde_json::Value>(&campaign_key).await?;
    if tx.revision(&state_key).as_deref() != Some(context.state_revision.as_str())
        || tx.revision(&cycle_key).as_deref() != Some(context.cycle_lease_revision.as_str())
        || tx.revision(&campaign_key).as_deref() != Some(context.campaign_lease_revision.as_str())
        || !owns(&cycle, &context.cycle_owner, now)
        || !campaign_owned(&campaign, &state, now)
    {
        return Err(failed("Prompt-cache scope experiment lost its lease."));
    }
    let inventory = crate::inventory::inventory(tx).await?;
    if !inventory.targets.iter().any(|t| {
        t.same_core(&context.target)
            && t.inventory_fingerprint == context.target.inventory_fingerprint
    }) {
        return Err(failed(
            "Prompt-cache scope experiment target changed during refresh.",
        ));
    }
    Ok(())
}
