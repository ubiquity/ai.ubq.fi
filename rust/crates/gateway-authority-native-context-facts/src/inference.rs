//! Actual inference composition on this same private cache; returned DATA never becomes a phase.
use crate::{Control, SourceNativeContextFactsData, SourceNativeContextFactsReader, checks_digest};
use gateway_authority_actor_currentness::source_actor::SourceActorCurrentData;
use gateway_authority_actor_records::source::StoredNativeActorV2;
use gateway_authority_contracts::NativeRenewalCancellation;
use gateway_authority_native_inference_facts::SourceNativeInferenceFactsReader;
use gateway_authority_native_records::rows::fail;
use gateway_contracts::identity::RequestContext;
use gateway_core::state::AuthorityStore;
use gateway_native_authority_wire::context::NativeRenewalContext;
use gateway_source::error::GatewayError;
impl<S: AuthorityStore, C: NativeRenewalCancellation> SourceNativeContextFactsReader<'_, S, C> {
    pub async fn read_inference(
        self,
        actor: &StoredNativeActorV2,
        original: &RequestContext,
        phase: &SourceActorCurrentData,
    ) -> Result<SourceNativeContextFactsData, GatewayError> {
        self.inference_context(actor, original, phase, None).await
    }
    pub(super) async fn inference_context(
        mut self,
        actor: &StoredNativeActorV2,
        original: &RequestContext,
        phase: &SourceActorCurrentData,
        expected: Option<&NativeRenewalContext>,
    ) -> Result<SourceNativeContextFactsData, GatewayError> {
        self.cache.now = self.time(self.cache.now)?;
        let now = self.cache.now;
        let data = {
            let snapshot = self.cache.fixed_store();
            SourceNativeInferenceFactsReader::new(&snapshot, self.identity, self.registered, now)?
                .read(actor, original, phase, expected)
                .await?
        };
        for check in data.revision_checks() {
            if !self.cache.expect(&check.key, &check.expected_revision) {
                return Err(fail());
            }
        }
        self.cache.now = self.time(data.observed_at_ms())?;
        self.lifetime(actor, phase, self.cache.now)?;
        Ok(SourceNativeContextFactsData {
            fence: data.fence().clone(),
            checks_digest: checks_digest(data.revision_checks())?,
            control_checks_digest: checks_digest(data.control_revision_checks())?,
            account_digests: data.account_digests().to_vec(),
            checks: self.cache.checks(),
            observed_at_ms: self.cache.now,
        })
    }
    pub(super) async fn inference_control(
        &mut self,
        actor: &StoredNativeActorV2,
        original: &RequestContext,
        phase: &SourceActorCurrentData,
        context: &NativeRenewalContext,
    ) -> Result<Control, GatewayError> {
        self.cache.now = self.time(self.cache.now)?;
        let now = self.cache.now;
        let data = {
            let snapshot = self.cache.fixed_store();
            SourceNativeInferenceFactsReader::new(&snapshot, self.identity, self.registered, now)?
                .read_control(actor, original, phase, Some(context))
                .await?
        };
        for check in data.revision_checks() {
            if !self.cache.expect(&check.key, &check.expected_revision) {
                return Err(fail());
            }
        }
        self.cache.now = self.time(data.observed_at_ms())?;
        self.lifetime(actor, phase, self.cache.now)?;
        Ok(Control {
            principal: data.principal().clone(),
            policy_digest: data.policy_digest().into(),
            account_hash: data.account_hash().into(),
            checks_digest: checks_digest(data.revision_checks())?,
        })
    }
}
