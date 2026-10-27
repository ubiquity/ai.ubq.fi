//! The source-owned cache campaign shares the existing transactional authority.
//! A campaign is not inference admission, account election or another paid ledger.
pub mod run;
mod state;
mod target;
use gateway_cache_experiment_records::{
    RefreshContext, model::TargetBinding, source::release_valid,
};

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

/// Source/normalized credential publication fences are checked separately by
/// native_control in each phase. Its own proven publication may advance the
/// source revision, while these immutable campaign and account-membership facts remain.
pub(crate) async fn validate_refresh_context<
    S: gateway_core::state::AuthorityStore,
    I: gateway_authority_contracts::TokenSource,
>(
    tx: &mut crate::store::Transaction<'_, S, I>,
    context: &RefreshContext,
    now: i64,
) -> Result<(), gateway_core::error::GatewayError> {
    let observed_at_ms = tx.now;
    let evidence = {
        let reader = tx.routing_read_store();
        gateway_cache_experiment_evidence::CacheEvidenceReader::new(&reader, observed_at_ms)?
            .validate_refresh_context(context, now)
            .await?
    };
    for check in evidence.revision_checks() {
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(gateway_authority_contracts::invalid(
                "Cache evidence conflicts with an existing transaction fence.",
            ));
        }
    }
    Ok(())
}
