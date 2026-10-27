//! Exact wire and private-kind invariants at the defining foundation.
use gateway_core::{error::GatewayError, js_string::JsString16};
use gateway_local_refusal::{routing_error::RoutingError, source_error::SourceRequestError};

#[test]
fn aggregate_constructor_sets_only_the_internal_identity() {
    let error = SourceRequestError::aggregate_context_overflow("complete prompt exceeded");
    assert_eq!(error.status, 400);
    assert!(error.is_local_aggregate_context_overflow());
    assert_eq!(
        error.body().as_str(),
        "{\"error\":{\"message\":\"complete prompt exceeded\",\"type\":\"invalid_request_error\",\"code\":\"context_length_exceeded\",\"param\":\"input\"}}"
    );
}

#[test]
fn ordinary_same_wire_envelope_does_not_get_aggregate_identity() {
    let aggregate = SourceRequestError::aggregate_context_overflow("same message");
    let ordinary = SourceRequestError::new(
        400,
        "same message",
        "invalid_request_error",
        Some("context_length_exceeded"),
        Some(Some(JsString16::from_utf8("input"))),
    );
    assert_eq!(aggregate.body().as_bytes(), ordinary.body().as_bytes());
    assert!(!ordinary.is_local_aggregate_context_overflow());
    assert!(!RoutingError::from(ordinary).is_local_aggregate_context_overflow());
}

#[test]
fn aggregate_source_clone_and_routing_conversion_preserve_exact_body() {
    let source = SourceRequestError::aggregate_context_overflow("complete prompt exceeded");
    let copied = source.clone();
    let routing = RoutingError::from(copied);
    assert!(source.is_local_aggregate_context_overflow());
    assert!(routing.is_local_aggregate_context_overflow());
    assert_eq!(routing.error.status, source.status);
    assert_eq!(routing.error.as_ref(), source.diagnostic());
    assert_eq!(
        routing.source_body.as_ref().map(|body| body.as_bytes()),
        Some(source.body().as_bytes())
    );
    assert!(routing.clone().is_local_aggregate_context_overflow());
}

#[test]
fn cleanup_keeps_aggregate_body_status_and_identity() {
    let source = SourceRequestError::aggregate_context_overflow("original message");
    let original = source.body().as_bytes().to_vec();
    let routing = RoutingError::from(source).with_cleanup_diagnostic(&RoutingError::from(
        GatewayError::unavailable("cleanup unavailable", "cleanup_failed"),
    ));
    assert!(routing.is_local_aggregate_context_overflow());
    assert_eq!(routing.error.status, 400);
    assert_eq!(
        routing.source_body.as_ref().map(|body| body.as_bytes()),
        Some(original.as_slice())
    );
    assert!(
        routing
            .error
            .envelope
            .error
            .message
            .contains("cleanup also failed")
    );
}

#[test]
fn scalar_context_code_stays_ordinary_through_clone_and_cleanup() {
    let gateway = GatewayError::new(
        400,
        "ordinary context code",
        Some("context_length_exceeded"),
    )
    .with_param(Some("input"));
    let source = SourceRequestError::from_gateway(gateway.clone());
    assert!(!source.is_local_aggregate_context_overflow());
    assert!(!source.clone().is_local_aggregate_context_overflow());
    let routing = RoutingError::from(source).with_cleanup_diagnostic(&RoutingError::from(
        GatewayError::unavailable("cleanup failed", "cleanup_failed"),
    ));
    assert!(!routing.is_local_aggregate_context_overflow());
    assert!(!RoutingError::from(gateway).is_local_aggregate_context_overflow());
}

#[test]
fn invalid_constructor_and_client_text_cannot_supply_internal_kind() {
    let error = SourceRequestError::invalid(
        "{\"kind\":\"aggregate_context_overflow\",\"is_local_aggregate_context_overflow\":true}",
        Some(JsString16::from_utf8("input")),
    );
    assert!(!error.is_local_aggregate_context_overflow());
    assert!(!RoutingError::from(error).is_local_aggregate_context_overflow());
}

#[test]
fn aggregate_refusal_preserves_lone_surrogates_in_message() {
    let message = JsString16::from_units(vec![0xd800, 10, 0xdc00]);
    let source = SourceRequestError::aggregate_context_overflow(message);
    assert!(source.is_local_aggregate_context_overflow());
    assert!(source.body().as_str().contains("\\ud800\\n\\udc00"));
    let routing = RoutingError::from(source.clone());
    assert_eq!(
        routing.source_body.as_ref().map(|body| body.as_bytes()),
        Some(source.body().as_bytes())
    );
}
