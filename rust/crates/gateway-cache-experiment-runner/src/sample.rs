//! Exact fetch/observe/current/read and single original probe settlement order.
use crate::{
    CacheRunOperations,
    cycle::{CycleRunner, SamplePlan},
    io::ExperimentIo,
};
use gateway_authority_contracts::add;
use gateway_cache_experiment_dispatch::Session;
use gateway_cache_experiment_records::model::{Reason, SAMPLE_MS, Sample, TargetBinding};
use gateway_source::error::GatewayError;
impl<R: CacheRunOperations + ?Sized, H: ExperimentIo> CycleRunner<'_, R, H> {
    pub(crate) async fn sample(
        &self,
        plan: &SamplePlan,
        target: &mut TargetBinding,
        session: &Session,
        index: usize,
        slot: u8,
    ) -> Result<Result<Sample, Reason>, GatewayError> {
        self.realm
            .renew_cache_cycle(self.permit, self.service.fresh_time(self.now)?)
            .await?;
        match self
            .realm
            .current_cache_target(target, self.service.fresh_time(self.now)?)
            .await?
        {
            Ok(current) => *target = current,
            Err(reason) => {
                return Ok(Err(reason));
            }
        }
        let sample_deadline = plan
            .deadline
            .min(add(self.service.fresh_time(self.now)?, SAMPLE_MS)?);
        let dispatch = self
            .realm
            .cache_dispatch(session, target, slot, self.service.fresh_time(self.now)?)
            .await?;
        let conversation = if index == 7 || index == 8 {
            &plan.conversation_b
        } else {
            &plan.conversation_a
        };
        let fetched = self
            .io
            .fetch(
                &dispatch,
                &plan.body,
                conversation,
                target.catalog_client_version.as_deref(),
                sample_deadline,
            )
            .await;
        let mut response = match fetched {
            Ok(value) => value,
            Err(error) => {
                let _ = self
                    .realm
                    .finish_cache_dispatch(&dispatch, false, self.service.fresh_time(self.now)?)
                    .await;
                return Err(error);
            }
        };
        let observed = self.io.observed_response(&dispatch, &mut response).await;
        if let Err(error) = observed.and(
            self.realm
                .cache_session_current(session, self.service.fresh_time(self.now)?)
                .await,
        ) {
            self.io.discard(response);
            let _ = self
                .realm
                .finish_cache_dispatch(&dispatch, false, self.service.fresh_time(self.now)?)
                .await;
            return Err(error);
        }
        let read = self
            .io
            .read(response, slot, &target.model, index, sample_deadline)
            .await;
        let completed = read.as_ref().is_ok_and(|r| r.completed);
        let _ = self
            .realm
            .finish_cache_dispatch(&dispatch, completed, self.service.fresh_time(self.now)?)
            .await;

        Ok(read?.value)
    }
}
