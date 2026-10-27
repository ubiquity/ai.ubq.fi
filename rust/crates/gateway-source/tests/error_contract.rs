use gateway_source::{
    error::{GatewayError, OpenAiError, OpenAiErrorEnvelope},
    js_string::JsString16,
    source_json::SourceJson,
};

#[test]
fn unchanged_error_envelope_has_exact_fields_codes_status_and_bytes()
-> Result<(), Box<dyn std::error::Error>> {
    let error = GatewayError::new(401, "a\0b", Some("auth_failed"));
    assert_eq!(error.status, 401);
    assert_eq!(error.to_string(), "a\0b");
    assert_eq!(
        serde_json::to_string(&error.envelope)?,
        r#"{"error":{"message":"a\u0000b","type":"invalid_request_error","code":"auth_failed"}}"#
    );
    let error = error.with_type("  server_error  ").with_param(None);
    assert_eq!(
        serde_json::to_string(&error.envelope)?,
        r#"{"error":{"message":"a\u0000b","type":"server_error","code":"auth_failed","param":null}}"#
    );
    assert_eq!(
        GatewayError::new(400, "message", None)
            .with_type(" ")
            .with_param(Some("field"))
            .envelope,
        OpenAiErrorEnvelope {
            error: OpenAiError {
                message: "message".into(),
                error_type: "invalid_request_error".into(),
                code: None,
                param: Some(Some("field".into()))
            }
        }
    );
    let unavailable = GatewayError::unavailable("unavailable", "fixed_code");
    assert_eq!(unavailable.status, 503);
    assert_eq!(
        serde_json::to_string(&unavailable.envelope)?,
        r#"{"error":{"message":"unavailable","type":"server_error","code":"fixed_code"}}"#
    );
    Ok(())
}
#[test]
fn source_json_error_and_lone_units_keep_original_policy() -> Result<(), Box<dyn std::error::Error>>
{
    let error = SourceJson::parse("{")
        .err()
        .ok_or("expected malformed JSON refusal")?;
    assert_eq!(error.status, 503);
    assert_eq!(
        error.envelope.error.code.as_deref(),
        Some("source_json_invalid")
    );
    let text = JsString16::from_units(vec![0xd800, 0, 0xdfff]);
    assert_eq!(text.to_source_json()?.as_str(), r#""\ud800\u0000\udfff""#);
    assert_eq!(text.to_source_json()?.deserialize::<JsString16>()?, text);
    assert!(text.try_utf8().is_err());
    Ok(())
}
