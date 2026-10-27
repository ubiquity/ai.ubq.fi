//! Exact original fifth-row refresh and source publication checks.
use crate::{CacheRunOperations, cycle::CycleRunner, io::ExperimentIo};
use gateway_cache_experiment_dispatch::Session;
use gateway_cache_experiment_records::model::{Cycle, Reason, Sample, TargetBinding, failed};
use gateway_source::error::GatewayError;
impl<R: CacheRunOperations + ?Sized, H: ExperimentIo> CycleRunner<'_, R, H> {
    pub(crate) async fn refresh_cycle(
        &self,
        target: &mut TargetBinding,
        session: &mut Session,
        cycle: &mut Cycle,
        current: i64,
    ) -> Result<bool, GatewayError> {
        match self.realm.current_cache_target(target, current).await? {
            Ok(current) => *target = current,
            Err(reason) => {
                cycle.inconclusive_reason = Some(reason);
                return Ok(false);
            }
        }
        self.realm
            .renew_cache_cycle(self.permit, self.service.fresh_time(self.now)?)
            .await?;
        let prefix: [Sample; 5] = cycle
            .samples
            .clone()
            .try_into()
            .map_err(|_| failed("Prompt-cache scope experiment refresh prefix is invalid."))?;
        let refresh = self
            .realm
            .cache_refresh_permit(
                self.permit,
                target,
                prefix,
                self.io.release_git_sha(),
                self.service.fresh_time(self.now)?,
            )
            .await?;
        let publication = match self.io.refresh(refresh).await {
            Ok(publication) => publication,
            Err(error) => {
                if self
                    .realm
                    .cache_pool_changed_after_refresh_error(
                        session,
                        self.service.fresh_time(self.now)?,
                    )
                    .await
                    .unwrap_or(false)
                {
                    cycle.inconclusive_reason = Some(Reason::AuthPoolDrift);
                    return Ok(false);
                }
                return Err(error);
            }
        };
        match self
            .realm
            .cache_session_after_refresh(session, &publication, self.service.fresh_time(self.now)?)
            .await?
        {
            Ok(next) => {
                *session = next;
                session
                    .revision()
                    .clone_into(&mut target.auth_pool_versionstamp);
            }
            Err(reason) => {
                cycle.inconclusive_reason = Some(reason);
                return Ok(false);
            }
        }

        Ok(true)
    }
}
