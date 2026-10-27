//! Closed read-only cache evidence. Every actual observed revision joins its consumer's original effect CAS.
mod cache;
mod campaign;
mod inventory;
mod refresh;
use gateway_cache_experiment_records::{
    RefreshContext,
    model::{Evidence, Reason, State, TargetBinding},
};
use gateway_core::{
    error::GatewayError,
    state::{AuthorityStore, RevisionCheck},
};
pub struct InventoryData {
    targets: Vec<TargetBinding>,
    checks: Vec<RevisionCheck>,
}
impl InventoryData {
    pub fn targets(&self) -> &[TargetBinding] {
        &self.targets
    }
    pub fn revision_checks(&self) -> &[RevisionCheck] {
        &self.checks
    }
}
pub struct TargetResolutionData {
    target: Result<TargetBinding, Reason>,
    checks: Vec<RevisionCheck>,
}
impl TargetResolutionData {
    pub fn target(&self) -> &Result<TargetBinding, Reason> {
        &self.target
    }
    pub fn revision_checks(&self) -> &[RevisionCheck] {
        &self.checks
    }
}
pub struct SelectedTargetData {
    target: TargetBinding,
    checks: Vec<RevisionCheck>,
}
impl SelectedTargetData {
    pub fn target(&self) -> &TargetBinding {
        &self.target
    }
    pub fn revision_checks(&self) -> &[RevisionCheck] {
        &self.checks
    }
}
pub struct CampaignRecordsData {
    state: Option<State>,
    evidence: Option<Evidence>,
    checks: Vec<RevisionCheck>,
}
impl CampaignRecordsData {
    pub fn state(&self) -> Option<&State> {
        self.state.as_ref()
    }
    pub fn evidence(&self) -> Option<&Evidence> {
        self.evidence.as_ref()
    }
    pub fn revision_checks(&self) -> &[RevisionCheck] {
        &self.checks
    }
}
pub struct RefreshContextData {
    checks: Vec<RevisionCheck>,
}
impl RefreshContextData {
    pub fn revision_checks(&self) -> &[RevisionCheck] {
        &self.checks
    }
}
pub struct CacheEvidenceReader<'a, S> {
    cache: cache::ReadCache<'a, S>,
}
impl<'a, S: AuthorityStore> CacheEvidenceReader<'a, S> {
    pub fn new(store: &'a S, now: i64) -> Result<Self, GatewayError> {
        Ok(Self {
            cache: cache::ReadCache::new(store, now)?,
        })
    }
    pub async fn inventory(mut self) -> Result<InventoryData, GatewayError> {
        let inventory = inventory::inventory(&mut self.cache).await?;
        Ok(InventoryData {
            targets: inventory.targets,
            checks: self.cache.checks(),
        })
    }
    pub async fn resolve(
        mut self,
        expected: &TargetBinding,
    ) -> Result<TargetResolutionData, GatewayError> {
        let target = inventory::resolve(&mut self.cache, expected).await?;
        Ok(TargetResolutionData {
            target,
            checks: self.cache.checks(),
        })
    }
    pub async fn select_target(
        mut self,
        targets: Vec<TargetBinding>,
    ) -> Result<SelectedTargetData, GatewayError> {
        let target = campaign::select_target(&mut self.cache, targets).await?;
        Ok(SelectedTargetData {
            target,
            checks: self.cache.checks(),
        })
    }
    pub async fn records(
        mut self,
        target: &TargetBinding,
    ) -> Result<CampaignRecordsData, GatewayError> {
        let (state, evidence) = campaign::records(&mut self.cache, target).await?;
        Ok(CampaignRecordsData {
            state,
            evidence,
            checks: self.cache.checks(),
        })
    }
    pub async fn validate_refresh_context(
        mut self,
        context: &RefreshContext,
        current_at_ms: i64,
    ) -> Result<RefreshContextData, GatewayError> {
        refresh::validate_refresh_context(&mut self.cache, context, current_at_ms).await?;
        Ok(RefreshContextData {
            checks: self.cache.checks(),
        })
    }
}
