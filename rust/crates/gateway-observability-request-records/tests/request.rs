use gateway_observability_request_records::{
    request::RequestLogRecord, request::merge_request_fields_source,
};
use gateway_source::{
    js_string::JsString16, source_json::SourceJson, source_serialization as source,
};
type TestResult = Result<(), Box<dyn std::error::Error>>;
fn normalize(value: &SourceJson) -> Result<RequestLogRecord, Box<dyn std::error::Error>> {
    RequestLogRecord::normalize(value, "key", &JsString16::from_utf8("request"), 7.0)?
        .ok_or_else(|| "Expected request-log DATA".into())
}
#[test]
fn legacy_request_record_omits_new_field_and_preserves_existing_guards() -> TestResult {
    let input = SourceJson::parse(
        r#"{"id":"request","status_code":777,"method":"post","model":"m\ud800","stream":true}"#,
    )?;
    let record = normalize(&input)?;
    assert!(record.native_http_failure.is_none());
    assert_eq!(record.status_code, 599.0);
    assert_eq!(record.method, JsString16::from_utf8("POST"));
    assert_eq!(
        record.model.as_ref().map(JsString16::as_units),
        Some([109, 0xd800].as_slice())
    );
    assert!(source::member(&record.source_json()?, "native_http_failure").is_none());
    Ok(())
}
#[test]
fn passive_original_failure_roundtrips_utf16_and_existing_safe_bounds() -> TestResult {
    let header = JsString16::from_units([vec![120; 511], vec![0xd800]].concat());
    let input = SourceJson::parse(
        r#"{"id":"request","native_http_failure":{"status":503,"headers":{"retry-after":"17"},"headers_truncated":true,"error":{"code":"upstream","message":"a\u0000\ud800","\udc00":"opaque"},"body_complete":true}}"#,
    )?;
    let mut original = source::member(&input, "native_http_failure")
        .ok_or("Missing original failure")?
        .object_entries()?;
    for (name, value) in &mut original {
        if name.as_units() == JsString16::from_utf8("headers").as_units() {
            *value = source::object_keys(vec![(
                JsString16::from_utf8("retry-after"),
                SourceJson::from_serializable(&header)?,
            )])?;
        }
    }
    let original = source::object_keys(original)?;
    let input = source::object_keys(vec![
        ("id".into(), SourceJson::from_serializable("request")?),
        ("native_http_failure".into(), original.clone()),
    ])?;
    let record = normalize(&input)?;
    let stored = source::member(&record.source_json()?, "native_http_failure")
        .ok_or("Original failure dropped")?;
    assert_eq!(stored.as_str(), original.as_str());
    let headers = source::member(&stored, "headers").ok_or("Missing headers")?;
    assert_eq!(headers.object_entries()?.len(), 1);
    let actual: JsString16 = source::member(&headers, "retry-after")
        .ok_or("Missing bounded header")?
        .deserialize()?;
    assert_eq!(actual.as_units().len(), 512);
    assert_eq!(actual, header);
    Ok(())
}
#[test]
fn partial_merge_retains_original_failure_and_incomplete_null_error() -> TestResult {
    let original = SourceJson::parse(
        r#"{"id":"request","native_http_failure":{"status":400,"headers":{},"headers_truncated":false,"error":null,"body_complete":false}}"#,
    )?;
    let record = normalize(&original)?;
    let patch =
        SourceJson::parse(r#"{"provider":"openrouter","status_code":200,"completed_at_ms":12}"#)?;
    let merged = merge_request_fields_source(
        Some(&record),
        &patch,
        "key",
        &JsString16::from_utf8("request"),
        7.0,
    )?;
    assert_eq!(merged.provider, "openrouter");
    assert_eq!(merged.status_code, 200.0);
    assert_eq!(
        merged.native_http_failure.as_ref().map(SourceJson::as_str),
        record.native_http_failure.as_ref().map(SourceJson::as_str)
    );
    Ok(())
}
