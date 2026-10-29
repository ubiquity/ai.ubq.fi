#[test]
fn original_catalog_default_model_keeps_source_units_without_scalar_fallback()
-> Result<(), Box<dyn std::error::Error>> {
    let runtime =
        gateway_source::source_json::SourceJson::parse(r#"{"default_model":"model\ud800"}"#)?;
    let model =
        gateway_extras_contract::model::runtime_default_model(&runtime).ok_or("missing model")?;
    assert!(model.as_js_string().as_units().contains(&0xd800));
    Ok(())
}
