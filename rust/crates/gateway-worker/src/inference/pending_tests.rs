//! Routing error selection stays with the Worker lifetime owner.
use super::stream::cleanup_error;
use gateway_core::error::GatewayError;
use gateway_routing_runtime::types::{RoutingError, SourceRefusalBody};

#[test]
fn cleanup_precedence_preserves_complete_original_or_actual_cleanup_error() {
    let mut original: RoutingError =
        GatewayError::new(503, "original", Some("original_failure")).into();
    original
        .headers
        .insert("x-original".into(), "preserved".into());
    original.source_body = Some(SourceRefusalBody::InternalServerError);
    let kept = cleanup_error(original.clone(), Ok(()));
    assert_eq!(kept.error, original.error);
    assert_eq!(kept.headers, original.headers);
    assert_eq!(
        kept.source_body.as_ref().map(SourceRefusalBody::as_bytes),
        original
            .source_body
            .as_ref()
            .map(SourceRefusalBody::as_bytes)
    );
    let cleanup: RoutingError =
        GatewayError::new(502, "actual cleanup", Some("cleanup_failure")).into();
    let chosen = cleanup_error(original, Err(cleanup.clone()));
    assert_eq!(chosen.error, cleanup.error);
    assert_eq!(chosen.headers, cleanup.headers);
    assert!(chosen.source_body.is_none());
}
