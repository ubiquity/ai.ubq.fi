//! One original awaited cycle, with private phase grouping only.
use crate::io::{ExperimentIo, request_body};
use crate::{CacheRunOperations, CacheRunService};
use gateway_authority_contracts::add;
use gateway_cache_experiment_campaign::{CacheCampaignFactory, CyclePermit};
use gateway_cache_experiment_dispatch::Session;
use gateway_cache_experiment_records::model::{
    CYCLE_MS, Cycle, Outcome, Reason, SLOTS, Sample, TargetBinding, classify, failed, sample_signal,
};
use gateway_source::error::GatewayError;
use serde_json::Value;
pub(crate) struct SamplePlan {
    pub(crate) deadline: i64,
    pub(crate) body: String,
    pub(crate) conversation_a: String,
    pub(crate) conversation_b: String,
}
pub(crate) struct CycleRunner<'a, R: CacheRunOperations + ?Sized, H: ExperimentIo> {
    pub(crate) realm: &'a R,
    pub(crate) service: CacheRunService<'a, <R as CacheCampaignFactory>::Store>,
    pub(crate) permit: &'a CyclePermit,
    pub(crate) io: &'a H,
    pub(crate) promoted: &'a mut bool,
    pub(crate) now: i64,
}
impl<R: CacheRunOperations + ?Sized, H: ExperimentIo> CycleRunner<'_, R, H> {
    pub(crate) async fn run(
        self,
        mut target: TargetBinding,
        mut session: Session,
    ) -> Result<Value, GatewayError> {
        let state = self.permit.state();
        let guard = if state.expires_at_ms <= self.service.fresh_time(self.now)? {
            Some(Reason::SessionExpired)
        } else if state.auth_pool_versionstamp.as_str() != session.revision() {
            Some(Reason::AuthPoolDrift)
        } else if !state.target.same_core(&target) {
            Some(Reason::TargetCatalogDrift)
        } else {
            None
        };
        if let Some(reason) = guard {
            return self
                .realm
                .finalize_cache_cycle(
                    self.permit,
                    Outcome::Inconclusive,
                    None,
                    Some(reason),
                    self.service.fresh_time(self.now)?,
                )
                .await;
        }
        if state.next_cycle == 4 {
            return self
                .realm
                .promote_cache_scope(
                    self.permit,
                    &target,
                    self.promoted,
                    self.service.fresh_time(self.now)?,
                )
                .await;
        }
        let deadline = add(self.service.fresh_time(self.now)?, CYCLE_MS)?;
        self.io.begin_cycle(deadline)?;
        let cycle_nonce = self.io.uuid()?;
        let cache_key = format!("uos-cache-scope-v5-{}", self.io.uuid()?);
        let body = request_body(&target.model, &cycle_nonce, &cache_key);
        let conversation_a = self.io.uuid()?;
        let conversation_b = self.io.uuid()?;
        let plan = SamplePlan {
            deadline,
            body,
            conversation_a,
            conversation_b,
        };
        let mut cycle = Cycle {
            cycle: state.next_cycle,
            samples: vec![],
            classification: None,
            inconclusive_reason: None,
        };
        for (index, &slot) in SLOTS.iter().enumerate() {
            let current = self.service.fresh_time(self.now)?;
            if current >= deadline {
                return Err(failed("Prompt-cache scope experiment cycle failed."));
            }
            if index == 5
                && !self
                    .refresh_cycle(&mut target, &mut session, &mut cycle, current)
                    .await?
            {
                break;
            }
            match self
                .sample(&plan, &mut target, &session, index, slot)
                .await?
            {
                Err(reason) => {
                    cycle.inconclusive_reason = Some(reason);
                    break;
                }
                Ok(sample) => {
                    if !sample.valid(index) {
                        cycle.inconclusive_reason = Some(Reason::SlotDrift);
                        break;
                    }
                    if let Err(reason) = sample_signal(&cycle.samples, &sample, index) {
                        cycle.inconclusive_reason = Some(reason);
                        break;
                    }
                    cycle.samples.push(sample);
                }
            }
        }
        self.finish_cycle(target, session, cycle).await
    }
    async fn finish_cycle(
        self,
        target: TargetBinding,
        session: Session,
        mut cycle: Cycle,
    ) -> Result<Value, GatewayError> {
        let state = self.permit.state();
        cycle.classification = if cycle.inconclusive_reason.is_none() {
            classify(&target.model, &cycle.samples)
        } else {
            None
        };
        let Some(scope) = &cycle.classification else {
            let reason = cycle.inconclusive_reason.unwrap_or(Reason::SlotDrift);
            cycle.inconclusive_reason = Some(reason);
            return self
                .realm
                .finalize_cache_cycle(
                    self.permit,
                    Outcome::Inconclusive,
                    Some(&cycle),
                    Some(reason),
                    self.service.fresh_time(self.now)?,
                )
                .await;
        };
        let mut next = state.clone();
        next.target = target;
        next.auth_pool_versionstamp = session.revision().to_owned();
        next.next_cycle += 1;
        next.classifications.push(scope.clone());
        if next.next_cycle == 4 {
            if !next.classifications.iter().all(|s| s == scope) {
                return self
                    .realm
                    .finalize_cache_cycle(
                        self.permit,
                        Outcome::Inconclusive,
                        Some(&cycle),
                        Some(Reason::CycleDisagreement),
                        self.service.fresh_time(self.now)?,
                    )
                    .await;
            }
            next.pending_scope = Some(scope.clone());
        }
        let intermediate = self
            .realm
            .persist_cache_cycle(
                self.permit,
                &next,
                &cycle,
                self.service.fresh_time(self.now)?,
            )
            .await?;
        if next.next_cycle <= 3 {
            return Ok(intermediate.into_response());
        }
        let target = next.target.clone();
        let pending = intermediate.permit();
        self.realm
            .promote_cache_scope(
                pending,
                &target,
                self.promoted,
                self.service.fresh_time(self.now)?,
            )
            .await
    }
}
