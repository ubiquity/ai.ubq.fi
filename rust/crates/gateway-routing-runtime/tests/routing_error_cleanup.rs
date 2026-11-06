use gateway_core::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use gateway_routing_runtime::types::RoutingError;
use gateway_schema::source_request::error::SourceRequestError;

#[test]
fn cleanup_diagnostic_never_overwrites_original_source_wire_truth() {
    let source = SourceRequestError::new(
        400,
        JsString16::from_units(vec![0xd800]),
        "invalid_request_error",
        Some("source_invalid"),
        Some(Some(JsString16::from_units(vec![0xdfff]))),
    );
    let body = source.body().clone();
    let error = RoutingError::from(source).with_cleanup_diagnostic(
        &GatewayError::unavailable("cleanup detail", "cleanup_failed").into(),
    );
    assert_eq!(error.error.status, 400);
    let actual = error.source_body.as_ref().map(|body| body.as_bytes());
    assert_eq!(actual, Some(body.as_bytes()));
    assert_eq!(error.to_string(), error.error.to_string());
    let message = &error.error.envelope.error.message;
    assert!(message.contains("cleanup also failed"));
    assert!(message.len() < 160);
}

#[test]
fn scalar_original_error_gets_exact_body_before_internal_cleanup_diagnostic()
-> Result<(), GatewayError> {
    let source =
        GatewayError::new(409, "original conflict", Some("original_conflict")).with_param(None);
    let body = SourceJson::from_serializable(&source.envelope)?;
    let error = RoutingError::from(source).with_cleanup_diagnostic(
        &GatewayError::unavailable("cleanup detail", "cleanup_failed").into(),
    );
    assert_eq!(error.error.status, 409);
    let actual = error.source_body.as_ref().map(|body| body.as_bytes());
    assert_eq!(actual, Some(body.as_bytes()));
    Ok(())
}
