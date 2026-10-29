use gateway_local_refusal::source_error::SourceRequestError;
use gateway_source::{
    js_string::JsString16, source_json::SourceJson, source_serialization as source,
};

#[test]
fn finite_generic_serialization_matches_wire_without_serializing_the_private_kind()
-> Result<(), Box<dyn std::error::Error>> {
    let error = SourceRequestError::aggregate_context_overflow(JsString16::from_units(vec![
        0xd800, 0, 0xd83d, 0xde00,
    ]));
    let encoded = serde_json::to_vec(error.body())?;
    assert_eq!(encoded, error.body().as_bytes());
    let parsed = SourceJson::parse_bytes(&encoded)?;
    assert!(source::member(&parsed, "local_aggregate_context_overflow").is_none());
    let fields = source::member(&parsed, "error").ok_or("missing error")?;
    assert_eq!(fields.object_entries()?.len(), 4);
    assert!(source::member(&fields, "local_aggregate_context_overflow").is_none());
    assert!(error.is_local_aggregate_context_overflow());
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
fn finite_generic_serializer_preserves_actual_writer_failure() {
    let error = SourceRequestError::new(400, "m", " t ", None, Some(None));
    assert!(serde_json::to_writer(RefuseWrite, error.body()).is_err());
    assert_eq!(
        error.body().as_str(),
        r#"{"error":{"message":"m","type":" t ","param":null}}"#
    );
}
