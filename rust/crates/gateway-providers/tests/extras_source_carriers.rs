use gateway_contracts::model::ModelId;
use gateway_provider_protocol::types::PrepareOptions;
use gateway_source::{
    js_string::JsString16, source_json::SourceJson, source_serialization as source,
};
fn parse(raw: &str) -> SourceJson {
    SourceJson::parse(raw).unwrap()
}
fn text(raw: &SourceJson, key: &str) -> Result<JsString16, String> {
    let field = source::member(raw, key).ok_or("missing expected source field")?;
    gateway_provider_source::source::string(&field)
        .ok_or_else(|| format!("expected text field {key}"))
}
#[test]
fn image_source_prompt_user_model_default_and_unknown_key_error_remain_original()
-> Result<(), String> {
    let raw =
        parse(r#"{"prompt":"p\ud800","user":"u\udc00","model":" m\ud801 ","size":"s\udc02"}"#);
    let parsed = gateway_provider_images::extras::parse_source_image(&raw, false)
        .map_err(|error| format!("{error:?}"))?;
    let default = ModelId::from_source(JsString16::from_units(vec![68, 0xd802]));
    let plan =
        gateway_provider_images::extras::image::prepare_parsed_image(&parsed, &default, "original")
            .map_err(|error| format!("{error:?}"))?;
    assert_eq!(
        text(&plan.responses, "model")?,
        default.as_js_string().clone()
    );
    assert_eq!(text(&plan.responses, "input")?, text(&raw, "prompt")?);
    assert_eq!(text(&plan.responses, "user")?, text(&raw, "user")?);
    let tool = gateway_source::source_serialization::array(
        &source::member(&plan.responses, "tools").ok_or("missing image tools")?,
    )
    .ok_or("missing image tools array")?
    .remove(0);
    assert_eq!(
        text(&tool, "model")?,
        JsString16::from_units(vec![109, 0xd801])
    );
    let bad = parse(r#"{"prompt":"","\ud800":true,"n":0}"#);
    let error = gateway_provider_images::extras::parse_source_image(&bad, false)
        .err()
        .ok_or("expected source rejection")?;
    let fields = source::member(error.body(), "error").ok_or("missing source error envelope")?;
    assert_eq!(
        text(&fields, "param")?,
        JsString16::from_units(vec![0xd800])
    );
    assert!(text(&fields, "message")?.as_units().contains(&0xd800));
    Ok(())
}
#[test]
fn embedding_and_systemone_use_the_same_source_engine_without_scalar_payloads() -> Result<(), String>
{
    let raw = parse(r#"{"model":"voyage-4-large","input":["x\ud800"],"user":"u\udc00"}"#);
    let profile = gateway_provider_services::extras::parse_source_embeddings(&raw, false)
        .map_err(|error| format!("{error:?}"))?;
    assert_eq!(profile.inputs[0].as_units(), &[120, 0xd800]);
    let prepared = gateway_provider_services::extras::prepare_source_embeddings(
        &raw,
        &PrepareOptions::default(),
    )
    .map_err(|error| format!("{error:?}"))?;
    assert_eq!(prepared.decode.request, raw);
    assert!(prepared.body.as_str().contains("\\ud800"));
    let system = parse(r#"{"state":{"\ud800":"v\udc00"},"questions":{"q\ud801":{"type":"noul"}}}"#);
    let prepared = gateway_provider_services::extras::prepare_source_systemone(
        &system,
        &PrepareOptions::default(),
    )
    .map_err(|error| format!("{error:?}"))?;
    assert_eq!(prepared.decode.request, system);
    assert!(prepared.body.as_str().contains("\\ud800"));
    assert!(prepared.body.as_str().contains("\\ud801"));
    Ok(())
}
#[test]
fn source_upstream_response_text_is_retained_in_existing_json_shape() -> Result<(), String> {
    let payload = parse(
        r#"{"output":[{"type":"image_generation_call","result":"AA==","revised_prompt":"r\ud800"}]}"#,
    );
    let images = gateway_provider_images::extras::extract_source_images(&payload)
        .map_err(|error| format!("{error:?}"))?;
    assert_eq!(
        text(&images[0], "revised_prompt")?,
        JsString16::from_units(vec![114, 0xd800])
    );
    let system = parse(r#"{"answers":{"q":"a\udc00"}}"#);
    assert_eq!(
        gateway_provider_services::extras::decode_source_systemone(&system)
            .map_err(|error| format!("{error:?}"))?,
        system
    );

    Ok(())
}
