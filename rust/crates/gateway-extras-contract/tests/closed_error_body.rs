use gateway_extras_contract::{
    response_body::ExtrasResponseBody,
    types::{ExtrasResponse, KernelDisposition},
};
use gateway_local_refusal::source_error::SourceRequestError;
use gateway_source::{js_string::JsString16, source_json::SourceJson};

#[test]
fn closed_extras_response_keeps_defaults_and_original_utf16_bytes() {
    let error = SourceRequestError::new(
        400,
        JsString16::from_units(vec![0xd800, 0xdc00, 0xdfff]),
        "invalid_request_error",
        Some("source_code"),
        Some(None),
    );
    let bytes = error.body().as_bytes().to_vec();
    let response = ExtrasResponse::source_error(error);
    assert_eq!(response.status, 400);
    assert_eq!(response.body.as_bytes(), bytes);
    assert_eq!(response.headers["content-type"], "application/json");
    assert!(response.semantic_terminal.is_none());
    assert!(response.usage.is_none());
    assert_eq!(response.kernel, KernelDisposition::FromAggregate);
    assert!(response.failure_kind.is_none());
    assert!(response.upstream_errors.is_empty());
    assert!(response.health.is_empty());
    assert_eq!(response.voyage().headers["x-uos-upstream"], "voyage");
}

#[test]
fn extras_json_and_closed_wire_equality_keeps_original_source_identity()
-> Result<(), Box<dyn std::error::Error>> {
    let error = SourceRequestError::new(409, "m", " t ", None, Some(None));
    let parsed = SourceJson::parse(error.body().as_str())?;
    let closed = ExtrasResponse::source_error(error);
    let json = ExtrasResponse::source_json(409, parsed);
    assert_eq!(closed.body, json.body);
    assert_eq!(serde_json::to_vec(&closed.body)?, closed.body.as_bytes());
    let arbitrary = SourceJson::parse(r#"{"x":1,"x":2,"raw":"\ud800"}"#)?;
    let response = ExtrasResponse::source_json(200, arbitrary.clone());
    assert_eq!(response.body.as_bytes(), arbitrary.as_bytes());
    assert_eq!(serde_json::to_vec(&response.body)?, arbitrary.as_bytes());
    Ok(())
}

struct RefuseWrite;
impl std::io::Write for RefuseWrite {
    fn write(&mut self, _: &[u8]) -> std::io::Result<usize> {
        Err(std::io::Error::other("fixture writer failure"))
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}
#[test]
fn extras_generic_serializer_retains_its_actual_failure_boundary() {
    let error = SourceRequestError::scalar("m", None);
    let response = ExtrasResponse::source_error(error);
    assert!(matches!(&response.body, ExtrasResponseBody::ClosedError(_)));
    assert!(serde_json::to_writer(RefuseWrite, &response.body).is_err());
    assert_eq!(
        response.body.as_str(),
        r#"{"error":{"message":"m","type":"invalid_request_error","code":"invalid_request_error"}}"#
    );
}
