//! Source isolate-local admission. This state holds no platform I/O or credentials.
use gateway_admission::{
    AdmissionClock, AdmissionController, AdmissionRequest, AdmissionResult, Permit,
};
use gateway_auth_contracts::types::AuthPrincipal;
use gateway_source::error::GatewayError;
use gateway_worker_platform::{cancellation::Cancellation, clock::monotonic};
use std::{rc::Rc, time::Duration};

struct Clock;

impl AdmissionClock for Clock {
    fn now(&self) -> Duration {
        // Requests validate this clock before queueing. A later platform failure
        // expires waits rather than supplying a fictitious earlier timestamp.
        monotonic().unwrap_or(Duration::MAX)
    }
}

thread_local! {
    static CONTROLLER: AdmissionController = AdmissionController::new(Rc::new(Clock));
}

pub async fn acquire(
    principal: &AuthPrincipal,
    request_id: &str,
    route: &str,
    cancellation: Rc<Cancellation>,
) -> Result<Option<Permit>, GatewayError> {
    monotonic()?;
    let principal = crate::lifecycle::principal_id(principal).to_source_json()?;
    let request = AdmissionRequest {
        principal: Some(principal.as_str().to_owned()),
        request_id: request_id.to_owned(),
        route: route.to_owned(),
        cancellation: cancellation.clone(),
    };
    let controller = CONTROLLER.with(Clone::clone);
    match controller.acquire(request, cancellation.as_ref()).await {
        AdmissionResult::Admitted { permit, .. } => Ok(Some(permit)),
        AdmissionResult::Bypassed => Ok(None),
        AdmissionResult::LocalOverload { cause, .. } => {
            Err(gateway_admission::overload_error(cause))
        }
        AdmissionResult::CallerAborted { .. } => Err(gateway_admission::cancellation_error()),
    }
}
