//! Source-wire and private snapshot vectors; no provider or host is invoked.
use gateway_local_refusal::{
    routing_error::{RoutingError, SourceRefusalBody},
    source_error::SourceRequestError,
};
use gateway_source::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use std::collections::BTreeMap;

#[test]
fn closed_error_preserves_untrimmed_type_and_all_optional_field_forms() {
    let cases = [
        (None, None, r#"{"error":{"message":"m","type":" vendor "}}"#),
        (
            None,
            Some(None),
            r#"{"error":{"message":"m","type":" vendor ","param":null}}"#,
        ),
        (
            None,
            Some(Some("")),
            r#"{"error":{"message":"m","type":" vendor ","param":""}}"#,
        ),
        (
            Some("c"),
            None,
            r#"{"error":{"message":"m","type":" vendor ","code":"c"}}"#,
        ),
        (
            Some("c"),
            Some(None),
            r#"{"error":{"message":"m","type":" vendor ","code":"c","param":null}}"#,
        ),
        (
            Some("c"),
            Some(Some("p")),
            r#"{"error":{"message":"m","type":" vendor ","code":"c","param":"p"}}"#,
        ),
    ];
    for (code, param, expected) in cases {
        let error = SourceRequestError::new(
            422,
            "m",
            " vendor ",
            code,
            param.map(|value| value.map(JsString16::from_utf8)),
        );
        assert_eq!(error.body().as_str(), expected);
        assert_eq!(error.body().error_type(), " vendor ");
        assert_eq!(error.body().code(), code);
        assert_eq!(error.diagnostic().envelope.error.error_type, "vendor");
        assert_eq!(error.status, 422);
        assert!(!error.is_local_aggregate_context_overflow());
    }
}

#[test]
fn closed_error_wire_preserves_control_quotes_and_surrogate_units()
-> Result<(), Box<dyn std::error::Error>> {
    let message = JsString16::from_units(vec![
        0, 8, 9, 10, 12, 13, 34, 92, 0xd800, 0xd83d, 0xde00, 0xdc00,
    ]);
    let param = JsString16::from_units(vec![0xdfff, 0]);
    let error = SourceRequestError::new(
        400,
        message.clone(),
        " t ",
        Some("q\""),
        Some(Some(param.clone())),
    );
    assert_eq!(
        error.body().as_str(),
        r#"{"error":{"message":"\u0000\b\t\n\f\r\"\\\ud800😀\udc00","type":" t ","code":"q\"","param":"\udfff\u0000"}}"#
    );
    let parsed = SourceJson::parse(error.body().as_str())?;
    let fields = source::member(&parsed, "error").ok_or("missing error")?;
    let decoded = source::member(&fields, "message").ok_or("missing message")?;
    assert_eq!(source::string(&decoded), Some(message));
    let decoded = source::member(&fields, "param").ok_or("missing param")?;
    assert_eq!(source::string(&decoded), Some(param));
    Ok(())
}

#[test]
fn scalar_snapshot_matches_original_wire_and_retains_null_or_omission()
-> Result<(), Box<dyn std::error::Error>> {
    for param in [None, Some(None), Some(Some("p\0\"".to_owned()))] {
        let mut original = GatewayError::new(409, "m\0\\\"\n😀", Some("c\0"));
        original.envelope.error.error_type = " vendor \t".to_owned();
        original.envelope.error.param = param;
        let wire = serde_json::to_vec(&original.envelope)?;
        let source = SourceRequestError::from_gateway(original.clone());
        assert_eq!(source.body().as_bytes(), wire);
        assert_eq!(source.diagnostic(), &original);
        assert_eq!(source.status, original.status);
        assert!(!source.is_local_aggregate_context_overflow());
        let expected = original
            .clone()
            .with_type(&original.envelope.error.error_type);
        assert_eq!(source.into_scalar(), expected);
    }
    Ok(())
}

#[test]
fn cleanup_snapshots_original_scalar_before_each_diagnostic_mutation()
-> Result<(), Box<dyn std::error::Error>> {
    let original = GatewayError::new(429, "original m", Some("original_code"))
        .with_type("rate_limit_error")
        .with_param(None);
    let wire = serde_json::to_vec(&original.envelope)?;
    let headers = BTreeMap::from([("retry-after".to_owned(), "7".to_owned())]);
    let failure = RoutingError::new(Box::new(original), headers.clone(), None);
    let cleanup = RoutingError::from(GatewayError::unavailable("first cleanup", "cleanup_failed"));
    let kept = failure.with_cleanup_diagnostic(&cleanup);
    assert_eq!(kept.error.status, 429);
    assert_eq!(kept.headers, headers);
    assert!(!kept.is_local_aggregate_context_overflow());
    let Some(SourceRefusalBody::ClosedError(body)) = &kept.source_body else {
        return Err("cleanup did not retain a closed snapshot".into());
    };
    assert_eq!(body.as_bytes(), wire);
    assert_eq!(body.message().try_utf8()?, "original m");
    assert_eq!(body.code(), Some("original_code"));
    assert_eq!(body.param(), Some(None));
    assert!(
        kept.error
            .envelope
            .error
            .message
            .contains("cleanup also failed")
    );
    let kept = kept.with_cleanup_diagnostic(&RoutingError::from(GatewayError::unavailable(
        "second cleanup",
        "second_failure",
    )));
    let body = kept.source_body.as_ref().ok_or("missing snapshot")?;
    assert_eq!(body.as_bytes(), wire);
    assert_eq!(body.content_type(), "application/json");
    assert!(kept.error.envelope.error.message.contains("second_failure"));
    Ok(())
}

#[test]
fn scalar_adapter_keeps_original_nonscalar_diagnostic_disposition() {
    let message = JsString16::from_units(vec![0xd800]);
    let source = SourceRequestError::new(400, message, " vendor ", Some("c"), Some(None));
    let expected = source.diagnostic().clone();
    assert_eq!(source.into_scalar(), expected);
    let param = JsString16::from_units(vec![0xdc00]);
    let source = SourceRequestError::new(400, "scalar message", "vendor", None, Some(Some(param)));
    let expected = source.diagnostic().clone();
    assert_eq!(source.into_scalar(), expected);
    let source = SourceRequestError::new(400, "m", "vendor", None, Some(None));
    assert_eq!(
        source.into_scalar(),
        GatewayError::new(400, "m", None)
            .with_type("vendor")
            .with_param(None)
    );
}

#[test]
fn ordinary_closed_carrier_cannot_forge_the_private_aggregate_kind() {
    let aggregate = SourceRequestError::aggregate_context_overflow("same");
    let ordinary = SourceRequestError::new(
        400,
        "same",
        "invalid_request_error",
        Some("context_length_exceeded"),
        Some(Some(JsString16::from_utf8("input"))),
    );
    assert_eq!(aggregate.body().as_bytes(), ordinary.body().as_bytes());
    let routing = RoutingError::new(
        Box::new(ordinary.diagnostic().clone()),
        BTreeMap::new(),
        Some(SourceRefusalBody::ClosedError(ordinary.body().clone())),
    );
    assert!(!routing.is_local_aggregate_context_overflow());
    let routing = routing.with_cleanup_diagnostic(&RoutingError::from(GatewayError::unavailable(
        "cleanup",
        "cleanup_failed",
    )));
    assert!(!routing.is_local_aggregate_context_overflow());
    assert!(RoutingError::from(aggregate).is_local_aggregate_context_overflow());
}

#[test]
fn upstream_json_and_plain_failure_carriers_keep_original_bytes()
-> Result<(), Box<dyn std::error::Error>> {
    let json = SourceJson::parse(r#" { "error": {"message":"\ud800"}, "vendor": true } "#)?;
    let bytes = json.as_bytes().to_vec();
    let failure = RoutingError::new(
        Box::new(GatewayError::unavailable("original", "original")),
        BTreeMap::new(),
        Some(SourceRefusalBody::Json(json)),
    );
    let failure = failure.with_cleanup_diagnostic(&RoutingError::from(GatewayError::unavailable(
        "cleanup",
        "cleanup_failed",
    )));
    let body = failure
        .source_body
        .as_ref()
        .ok_or("missing upstream body")?;
    assert_eq!(body.as_bytes(), bytes);
    assert_eq!(body.content_type(), "application/json");
    let body = SourceRefusalBody::InternalServerError;
    assert_eq!(body.as_bytes(), b"Internal Server Error");
    assert_eq!(body.content_type(), "text/plain; charset=utf-8");
    Ok(())
}

#[test]
fn every_utf16_unit_survives_closed_wire_through_the_defining_source_parser()
-> Result<(), Box<dyn std::error::Error>> {
    let units = (0..=u16::MAX).collect::<Vec<_>>();
    let error = SourceRequestError::new(
        400,
        JsString16::from_units(units.clone()),
        "invalid_request_error",
        None,
        None,
    );
    let body = SourceJson::parse_bytes(error.body().as_bytes())?;
    let fields = source::member(&body, "error").ok_or("missing error")?;
    let message = source::member(&fields, "message").ok_or("missing message")?;
    let message = source::string(&message).ok_or("message is not a source string")?;
    assert_eq!(message.as_units(), units.as_slice());
    assert_eq!(error.body().message(), &message);
    assert!(!format!("{:?}", error.body()).contains("message"));
    Ok(())
}

#[test]
fn empty_wire_type_and_code_remain_verbatim_while_diagnostic_type_defaults() {
    for (kind, expected) in [
        ("", r#"{"error":{"message":"m","type":"","code":""}}"#),
        (
            " \t\r\n",
            r#"{"error":{"message":"m","type":" \t\r\n","code":""}}"#,
        ),
    ] {
        let source = SourceRequestError::new(400, "m", kind, Some(""), None);
        assert_eq!(source.body().as_str(), expected);
        assert_eq!(source.body().error_type(), kind);
        assert_eq!(source.body().code(), Some(""));
        assert_eq!(
            source.diagnostic().envelope.error.error_type,
            "invalid_request_error"
        );
        assert_eq!(source.into_scalar(), GatewayError::new(400, "m", Some("")));
    }
}
