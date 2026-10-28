//! Complete original published source/normalized/unchanged-sibling/native credential predicate.
use crate::{SourceNativeContextFactsReader, legacy_digest};
use gateway_auth_credentials::provider_credentials::{
    MANAGED_NATIVE_SOURCE_VERSION, codex_access_expiry_ms, codex_source_pool_key,
    parse_codex_source_pool,
};
use gateway_authority_actor_currentness::source_actor::SourceActorCurrentData;
use gateway_authority_contracts::NativeRenewalCancellation;
use gateway_authority_keys as keys;
use gateway_authority_native_records::{
    rows::{TerminalPublication, fail},
    source_rows::SourceRenewalActionRowV3,
};
use gateway_authority_routing_records::types::SubscriptionPool;
use gateway_core::state::{AuthorityStore, RevisionCheck};
use gateway_native_authority_wire::source_fence::SourceNativeRenewalFenceV3;
use gateway_source::error::GatewayError;
use serde_json::Value;
pub struct SourceNativeTerminalFactsData {
    fence: SourceNativeRenewalFenceV3,
    checks: Vec<RevisionCheck>,
    observed_at_ms: i64,
}
impl SourceNativeTerminalFactsData {
    pub fn fence(&self) -> &SourceNativeRenewalFenceV3 {
        &self.fence
    }
    pub fn revision_checks(&self) -> &[RevisionCheck] {
        &self.checks
    }
    pub fn observed_at_ms(&self) -> i64 {
        self.observed_at_ms
    }
}
impl<S: AuthorityStore, C: NativeRenewalCancellation> SourceNativeContextFactsReader<'_, S, C> {
    pub async fn read_terminal(
        mut self,
        action: &SourceRenewalActionRowV3,
        phase: &SourceActorCurrentData,
        terminal: &TerminalPublication,
    ) -> Result<SourceNativeTerminalFactsData, GatewayError> {
        self.cache.now = self.time(action.created_at_ms)?;
        if self.cache.now >= action.deadline_ms {
            return Err(fail());
        }
        let control_digest = self
            .live_control(&action.actor, &action.context, phase, &action.fence.context)
            .await?
            .checks_digest;
        if control_digest != action.control_checks_digest {
            return Err(fail());
        }
        let source_key = codex_source_pool_key();
        let source: Value = self.cache.read(&source_key).await?.ok_or_else(fail)?;
        let pool = parse_codex_source_pool(&source)?.ok_or_else(fail)?;
        let normalized: SubscriptionPool =
            self.cache.read(&keys::pool()).await?.ok_or_else(fail)?;
        let slot = pool
            .accounts
            .iter()
            .position(|a| a.identity().routing_account_hash.0 == action.fence.account_id_hash)
            .ok_or_else(fail)?;
        if self.cache.revision(&source_key).as_deref() != Some(&terminal.source_revision)
            || self.cache.revision(&keys::pool()).as_deref() != Some(&terminal.normalized_revision)
            || normalized.pool_versionstamp.0 != terminal.source_revision
            || pool.accounts.len() != action.account_digests.len()
            || normalized.accounts.len() != pool.accounts.len()
        {
            return Err(fail());
        }
        for (index, account) in pool.accounts.iter().enumerate() {
            let id = account.identity();
            let projected = &normalized.accounts[index];
            if id.routing_account_hash != projected.account_id_hash
                || id.credential_version != projected.credential_version
                || id.administrative_account_hash != projected.admin_account_hash
            {
                return Err(fail());
            }
            if index != slot
                && legacy_digest(&account.private_source_value())? != action.account_digests[index]
            {
                return Err(fail());
            }
        }
        let account = pool.accounts.get(slot).ok_or_else(fail)?;
        let native = account.native_owner.as_ref().ok_or_else(fail)?;
        let physical = native
            .managed_source
            .as_ref()
            .filter(|v| v.v == MANAGED_NATIVE_SOURCE_VERSION)
            .ok_or_else(fail)?;
        let id = account.identity();
        let expiry = codex_access_expiry_ms(&account.access_token).ok_or_else(fail)?;
        self.cache.now = self.time(self.cache.now)?;
        self.lifetime(&action.actor, phase, self.cache.now)?;
        if self.cache.now >= action.deadline_ms
            || expiry <= self.cache.now.saturating_add(120_000) as f64
            || native.codex_home != self.registered.registered_home()
            || native.external_user_id.is_some()
            || !account.refresh_token.expose().is_empty()
            || id.routing_account_hash.0 != action.fence.account_id_hash
            || id.credential_version.0 != terminal.credential_version
            || id.native_generation_hash != terminal.access_generation
            || native.generation_hash != terminal.access_generation
            || physical.generation_hash != terminal.native_source_generation
        {
            return Err(fail());
        }
        let mut fence = action.fence.clone();
        fence.source_revision = terminal.source_revision.clone();
        fence.normalized_revision = terminal.normalized_revision.clone();
        fence.source_native_generation = terminal.native_source_generation.clone();
        fence.access_generation = terminal.access_generation.clone();
        fence.credential_version = terminal.credential_version.clone();
        fence.native_refreshed_at = native.native_refreshed_at.clone();
        fence.access_expires_at_ms = Some(expiry as i64);
        Ok(SourceNativeTerminalFactsData {
            fence,
            checks: self.cache.checks(),
            observed_at_ms: self.cache.now,
        })
    }
}
