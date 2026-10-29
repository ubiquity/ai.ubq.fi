use super::*;
use gateway_routing_plan::input::RoutingInput;
use gateway_routing_runtime::{
    session::RoutingSession,
    types::{AttemptFailure, FailureOrigin, TransportState},
};
use std::{
    future::Future,
    task::{Context as TaskContext, Poll, Waker},
};
thread_local! {static GATE:std::cell::RefCell<Option<std::rc::Rc<routing::Pause>>>=const{std::cell::RefCell::new(None)};}
pub(super) async fn release_and_settle() -> Result<(), GatewayError> {
    let pause = GATE.with(|gate| gate.borrow_mut().take());
    if let Some(pause) = pause {
        pause.release();
        pause.settlement().await
    } else {
        Ok(())
    }
}
pub(super) async fn prepare(
    case: &Case,
    context: &Context,
    request: &Request,
) -> Result<gateway_authority_native_preparation::SourceNativeRenewalAction, GatewayError> {
    let principal =
        gateway_auth::resolve::AuthService::new(case.authority.store(), &case.host, &case.config)
            .authenticate_client(&case.original)
            .await?;
    let raw = SourceJson::parse(
        r#"{"model":"gpt-test","input":"source external fixture","reasoning":{"effort":"none"}}"#,
    )?;
    let document = gateway_request_normalization::document::SourceRequestDocument::new(raw)
        .map_err(|e| e.diagnostic().clone())?
        .for_surface(gateway_schema_values::request::RequestSurface::Responses);
    let mut catalog = gateway_catalog_input::catalog::CatalogInput::default();
    catalog.full_snapshot = Some(gateway_model_snapshot::snapshot::ModelSnapshot {
        models: vec![SourceJson::parse(
            r#"{"slug":"gpt-test","supported_reasoning_levels":["none"],"default_reasoning_level":"none"}"#,
        )?],
        source: "fixture".into(),
        updated_at_ms: TokenClock.now_ms()?,
        client_version: Some("0.163.0".into()),
    });
    let input = RoutingInput::from_source_request(
        gateway_routing_plan::input::LocalRequestMetadata {
            context: case.context.clone(),
            principal,
            catalog,
            runtime: None,
            client_version: "0.163.0".into(),
            native_session_id: None,
            retry_prohibited: false,
        },
        document,
    );
    let pause = std::rc::Rc::new(routing::Pause::default());
    let host = routing::CodexHost {
        store: case.authority.store().clone(),
        pause: pause.clone(),
        cancellation: std::rc::Rc::new(
            gateway_worker_platform::cancellation::Cancellation::from_request(request)
                .map_err(|_| invalid())?,
        ),
    };
    let session = RoutingSession::begin(
        input,
        case.authority.store().clone(),
        host,
        TokenClock,
        TokenClock,
    )
    .await
    .map_err(|e| *e.error)?;
    let attempt = session
        .next_attempt()
        .await
        .map_err(|e| *e.error)?
        .ok_or_else(invalid)?;
    session
        .before_dispatch(&attempt)
        .await
        .map_err(|e| *e.error)?;
    session
        .transport_started(&attempt)
        .await
        .map_err(|e| *e.error)?;
    let failure = AttemptFailure {
        origin: FailureOrigin::Provider,
        status: Some(401),
        headers: Default::default(),
        body: Vec::new(),
        body_complete: true,
        failure: gateway_provider_policy::policy::HopFailure::Status(401),
        transport: TransportState::Started,
        semantic_committed: false,
        cancelled: false,
    };
    let completion = pause.clone();
    let mut work = Box::pin(async move {
        let result = session
            .refusal(&attempt, failure)
            .await
            .map(|_| ())
            .map_err(|error| *error.error);
        completion.finish(result);
    });
    let mut cx = TaskContext::from_waker(Waker::noop());
    if let Poll::Ready(()) = work.as_mut().poll(&mut cx) {
        return Err(invalid());
    }
    GATE.with(|gate| *gate.borrow_mut() = Some(pause));
    context.wait_until(work);
    let actor = ActorService::new(case.authority.store(), &case.host, &case.config)
        .authenticate_source_client_actor(&case.original, &case.context)
        .await?;
    case.authority
        .source_native_renewals(
            &case.host,
            &case.config,
            &case.identity,
            &case.owner,
            &Cancel,
        )
        .prepare_source_inference_renewal(&actor)
        .await
}
