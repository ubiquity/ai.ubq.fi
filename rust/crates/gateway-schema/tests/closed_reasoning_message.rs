use gateway_schema::source_request::reasoning::parse_reasoning;
use gateway_source::{
    js_string::JsString16, source_json::SourceJson, source_serialization as source,
};

#[test]
fn reasoning_error_message_and_parent_param_stay_at_the_original_validation_point()
-> Result<(), Box<dyn std::error::Error>> {
    for raw in [r#"{"effort":7}"#, r#"{"effort":true}"#, r#"{"effort":[]}"#] {
        let input = SourceJson::parse(raw)?;
        let Err(error) = parse_reasoning(Some(&input)) else {
            return Err("invalid effort was accepted".into());
        };
        assert_eq!(error.status, 400);
        assert_eq!(
            error.body().as_str(),
            r#"{"error":{"message":"reasoning.effort must be a string","type":"invalid_request_error","code":"invalid_request_error","param":"reasoning"}}"#
        );
        assert!(!error.is_local_aggregate_context_overflow());
    }
    Ok(())
}

#[test]
fn nonempty_lone_unit_reasoning_remains_open_source_data() -> Result<(), Box<dyn std::error::Error>>
{
    let input = SourceJson::parse(r#"{"effort":"\ud800","summary":"auto"}"#)?;
    let output = match parse_reasoning(Some(&input)) {
        Ok(Some(output)) => output,
        _ => return Err("source-valid reasoning was rejected".into()),
    };
    let effort = source::member(&output, "effort").ok_or("missing effort")?;
    assert_eq!(
        source::string(&effort),
        Some(JsString16::from_units(vec![0xd800]))
    );
    assert_eq!(
        source::member(&output, "summary"),
        source::member(&input, "summary")
    );
    Ok(())
}
