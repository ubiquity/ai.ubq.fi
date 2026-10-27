//! Actual prepared-target mint retains its original observation and fresh recheck order.
use super::PreparedTarget;
use crate::store::{Authority, Transaction};
use gateway_authority_contracts::TokenSource;
use gateway_cache_experiment_evidence::{CacheEvidenceReader, InventoryData};
use gateway_cache_experiment_records::model::{Reason, TargetBinding, unavailable};
use gateway_core::state::AuthorityStore;
use gateway_source::error::GatewayError;
pub(super) async fn inventory<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
) -> Result<InventoryData, GatewayError> {
    let observed_at_ms = tx.now;
    let evidence = {
        let reader = tx.routing_read_store();
        CacheEvidenceReader::new(&reader, observed_at_ms)?
            .inventory()
            .await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(gateway_authority_contracts::invalid(
                "Cache evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok(evidence)
}
pub(super) async fn resolve<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    expected: &TargetBinding,
) -> Result<Result<TargetBinding, Reason>, GatewayError> {
    let observed_at_ms = tx.now;
    let evidence = {
        let reader = tx.routing_read_store();
        CacheEvidenceReader::new(&reader, observed_at_ms)?
            .resolve(expected)
            .await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(gateway_authority_contracts::invalid(
                "Cache evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok(evidence.target().clone())
}
impl<S: AuthorityStore, I: TokenSource> Authority<S, I> {
    pub async fn prepare_cache_experiment(
        &self,
        release_git_sha: &str,
        now: i64,
    ) -> Result<PreparedTarget, GatewayError> {
        if !super::release_valid(release_git_sha) {
            return Err(unavailable(
                "Prompt-cache scope experiment requires current-release telemetry.",
            ));
        }
        let mut tx = self.transaction(now)?;
        let current = inventory(&mut tx).await?;
        let target = super::state::select_target(&mut tx, current.targets().to_vec()).await?;
        let mut recheck = self.transaction(self.fresh_time(now)?)?;
        let target=resolve(&mut recheck,&target).await?.map_err(|_| unavailable("Prompt-cache scope experiment target changed before its Stage 0 baseline could be checked."))?;
        Ok(PreparedTarget {
            target,
            release_git_sha: release_git_sha.into(),
        })
    }
}
