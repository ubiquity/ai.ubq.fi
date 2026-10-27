mod runtime_support;
use gateway_core::lifecycle::SemanticTerminal;
use gateway_extras_contract::types::{
    DispatchFailure, DispatchFailureOrigin, ExtrasHost, ExtrasRoute, HealthKind, ImageKind,
};
use gateway_extras_runtime::ExtrasRuntime;
use gateway_extras_runtime_test_support::fixture::error;
use gateway_source::{error::GatewayError, js_string::JsString16};
use runtime_support::{request, run, scalar_source, setup};
use serde_json::json;

#[test]
fn systemone_uses_authoritative_provider_selection_and_normalizes_actual_usage()
-> Result<(), Box<dyn std::error::Error>> {
    let (store, host, auth, ctx) = setup();
    let rt = ExtrasRuntime::new(store.clone(), host.clone());
    let body = json!({"state":{},"questions":{"go":{"type":"noul"}}});
    store.seed(
        json!(["uos_ai", "provider_selection", "v1"]),
        json!({"provider_ids":["codex"],"updated_at_ms":host.now_ms()}),
        None,
    );
    let disabled =
        run(rt.handle(request(ExtrasRoute::SystemOne, body.clone(), &auth, &ctx))).unwrap();
    assert_eq!(
        scalar_source(&disabled.body)?["error"]["code"],
        "systemone_provider_disabled"
    );
    assert_eq!(host.count(), 0);
    store.seed(
        json!(["uos_ai", "provider_selection", "v1"]),
        json!({"provider_ids":[],"updated_at_ms":host.now_ms()}),
        None,
    );
    let r = run(rt.handle(request(ExtrasRoute::SystemOne, body, &auth, &ctx))).unwrap();
    assert_eq!(scalar_source(&r.body)?["answers"]["go"], true);
    assert_eq!(r.usage.unwrap()["input_tokens_details"]["cached_tokens"], 0);
    assert_eq!(r.health[0].kind, HealthKind::Success);
    Ok(())
}

#[test]
fn systemone_maps_actual_upstream_errors_and_does_not_echo_private_bodies() {
    let (store, host, auth, ctx) = setup();
    let rt = ExtrasRuntime::new(store, host.clone());
    host.reply(401, json!({"private":"never_echo"}));
    let r = run(rt.handle(request(
        ExtrasRoute::SystemOne,
        json!({"state":{},"questions":{"go":{"type":"noul"}}}),
        &auth,
        &ctx,
    )))
    .unwrap();
    assert_eq!(r.status, 502);
    assert_eq!(r.health[0].kind, HealthKind::AuthInvalid);
    assert_eq!(r.health[0].upstream_status, Some(401));
    assert!(!r.body.as_str().contains("never_echo"));
    assert!(!r.upstream_errors.is_empty());
}

#[test]
fn image_fanout_uses_one_original_scope_exact_child_ids_and_common_output_metadata()
-> Result<(), Box<dyn std::error::Error>> {
    let (store, host, auth, ctx) = setup();
    let rt = ExtrasRuntime::new(store, host.clone());
    let mut q = request(
        ExtrasRoute::Images(ImageKind::Generations),
        json!({"prompt":"image","n":3}),
        &auth,
        &ctx,
    );
    q.idempotency_key = Some("outer-not-forwarded".into());
    let r = run(rt.handle(q)).unwrap();
    assert_eq!(r.status, 200);
    assert_eq!(scalar_source(&r.body)?["data"].as_array().unwrap().len(), 3);
    assert_eq!(scalar_source(&r.body)?["output_format"], "png");
    assert_eq!(r.semantic_terminal, Some(SemanticTerminal::Completed));
    assert_eq!(host.0.begins.get(), 1);
    assert_eq!(host.0.finished.get(), 1);
    assert_eq!(&*host.0.children.borrow(), &[0, 1, 2]);
    let ids: Vec<_> = host
        .0
        .calls
        .borrow()
        .iter()
        .map(|(id, _)| id.clone())
        .collect();
    assert_eq!(
        ids,
        [
            "original",
            "original:image:generations:2",
            "original:image:generations:3"
        ]
    );
    assert!(
        host.0
            .calls
            .borrow()
            .iter()
            .all(|(_, b)| b["model"] == "gpt-fixture-host")
    );
    Ok(())
}

#[test]
fn image_failure_aborts_siblings_but_settles_every_logical_child_once()
-> Result<(), Box<dyn std::error::Error>> {
    let (store, host, auth, ctx) = setup();
    let rt = ExtrasRuntime::new(store, host.clone());
    host.reply(
        200,
        json!({"output":[{"type":"image_generation_call","result":"AA=="}]}),
    );
    host.reply(429, json!({"error":{"code":"codex_quota_blocked"}}));
    let r = run(rt.handle(request(
        ExtrasRoute::Images(ImageKind::Generations),
        json!({"prompt":"image","n":3}),
        &auth,
        &ctx,
    )))
    .unwrap();
    assert_eq!(r.status, 429);
    assert_eq!(
        scalar_source(&r.body)?["error"]["code"],
        "codex_quota_blocked"
    );
    assert!(r.semantic_terminal.is_none());
    assert!(host.0.aborted.get());
    assert_eq!(host.0.children.borrow().len(), 3);
    assert_eq!(host.0.finished.get(), 1);
    Ok(())
}

#[test]
fn gateway_origin_preserves_admission_and_started_ledger_failures_without_provider_health() {
    for before_transport in [true, false] {
        let (store, host, auth, ctx) = setup();
        let mut failure = DispatchFailure::gateway(
            GatewayError::unavailable("ledger unavailable", "ledger_unavailable"),
            before_transport,
        );
        failure.headers.insert("Retry-After".into(), "3".into());
        host.0.replies.borrow_mut().push_back(Err(failure));
        let result = run(ExtrasRuntime::new(store, host).handle(request(
            ExtrasRoute::SystemOne,
            json!({"state":{},"questions":{"go":{"type":"noul"}}}),
            &auth,
            &ctx,
        )))
        .unwrap();
        assert_eq!(result.status, 503);
        assert_eq!(result.headers["Retry-After"], "3");
        assert!(result.health.is_empty());
        assert_eq!(
            result.upstream_errors[0].origin,
            DispatchFailureOrigin::Gateway
        );
        assert_eq!(result.upstream_errors[0].before_transport, before_transport);
        assert!(!result.upstream_errors[0].quota_refusal);
    }
}

#[test]
fn actual_local_quota_transport_fault_and_upstream_refusal_keep_separate_origins() {
    let (store, host, auth, ctx) = setup();
    let mut quota = DispatchFailure::gateway(
        GatewayError::new(429, "quota", Some("api_key_quota_exceeded")),
        true,
    );
    quota.quota_refusal = true;
    host.0.replies.borrow_mut().push_back(Err(quota));
    let result = run(ExtrasRuntime::new(store, host).handle(request(
        ExtrasRoute::SystemOne,
        json!({"state":{},"questions":{"go":{"type":"noul"}}}),
        &auth,
        &ctx,
    )))
    .unwrap();
    assert_eq!(result.status, 429);
    assert!(result.health.is_empty());
    assert!(result.upstream_errors[0].quota_refusal);
    let (store, host, auth, ctx) = setup();
    host.0
        .replies
        .borrow_mut()
        .push_back(Err(DispatchFailure::transport(error("network"))));
    let result = run(ExtrasRuntime::new(store, host).handle(request(
        ExtrasRoute::SystemOne,
        json!({"state":{},"questions":{"go":{"type":"noul"}}}),
        &auth,
        &ctx,
    )))
    .unwrap();
    assert_eq!(result.status, 502);
    assert_eq!(result.health[0].kind, HealthKind::UpstreamError);
    assert_eq!(
        result.upstream_errors[0].origin,
        DispatchFailureOrigin::Transport
    );
}

#[test]
fn finite_response_test_view_keeps_wire_and_rejects_nonscalar_json()
-> Result<(), Box<dyn std::error::Error>> {
    let error = gateway_local_refusal::source_error::SourceRequestError::new(
        400,
        JsString16::from_units(vec![0xd800, 0, 0xdc00]),
        " raw ",
        None,
        Some(None),
    );
    let bytes = error.body().as_bytes().to_vec();
    let response = gateway_extras_contract::types::ExtrasResponse::source_error(error);
    assert_eq!(response.body.as_bytes(), bytes);
    assert_eq!(response.headers["content-type"], "application/json");
    assert!(scalar_source(&response.body).is_err());
    let scalar = gateway_extras_contract::types::ExtrasResponse::source_error(
        gateway_local_refusal::source_error::SourceRequestError::scalar("original", None),
    );
    assert_eq!(scalar_source(&scalar.body)?["error"]["message"], "original");
    Ok(())
}
