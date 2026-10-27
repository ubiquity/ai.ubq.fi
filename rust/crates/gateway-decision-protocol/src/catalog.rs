use gateway_core::{
    js_string::JsString16, model::ModelId, source_json::SourceJson, source_serialization as source,
};

pub const MODELS_URL: &str =
    "https://openrouter.ai/api/v1/models?output_modalities=text%2Cdecisions";
pub const DECISIONS_URL: &str = "https://openrouter.ai/api/alpha/decisions";

pub fn whitespace(c: char) -> bool {
    matches!(c, '\u{0009}'..='\u{000d}' | ' ' | '\u{00a0}' | '\u{1680}'
        | '\u{2000}'..='\u{200a}' | '\u{2028}' | '\u{2029}' | '\u{202f}'
        | '\u{205f}' | '\u{3000}' | '\u{feff}')
}
pub fn trim(value: &JsString16) -> JsString16 {
    let units = value.as_units();
    let white = |u: &&u16| char::from_u32(u32::from(**u)).is_some_and(whitespace);
    let start = units.iter().take_while(white).count();
    let end = units.len() - units[start..].iter().rev().take_while(white).count();
    JsString16::from_units(units[start..end].to_vec())
}
pub fn equals(value: &JsString16, text: &str) -> bool {
    value.as_units().iter().copied().eq(text.encode_utf16())
}
/// Normalize only an actual upstream architecture statement. Empty statements
/// preserve the source's historical text default.
pub fn output_modalities(raw: &SourceJson) -> Option<Vec<JsString16>> {
    let architecture = source::member(raw, "architecture")?;
    let values = source::array(&source::member(&architecture, "output_modalities")?)?;
    let mut result = Vec::new();
    for value in values {
        if let Some(value) = source::string(&value) {
            let value = trim(&value);
            if !value.as_units().is_empty() && !result.contains(&value) {
                result.push(value);
            }
        }
    }
    (!result.is_empty()).then_some(result)
}
pub fn supports(model: &SourceJson, modality: &str) -> bool {
    let Some(value) = source::member(model, "output_modalities") else {
        return modality == "text";
    };
    source::array(&value).is_some_and(|values| {
        values
            .iter()
            .filter_map(source::string)
            .any(|value| equals(&value, modality))
    })
}
pub fn text_model(model: &SourceJson) -> bool {
    supports(model, "text")
}
pub fn decision_model(model: &SourceJson) -> bool {
    supports(model, "decisions")
}
pub fn served_models(models: &[SourceJson]) -> impl Iterator<Item = &SourceJson> {
    models
        .iter()
        .filter(|model| text_model(model) || decision_model(model))
}
pub fn endpoints(model: &SourceJson) -> Vec<&'static str> {
    let mut endpoints = Vec::new();
    if text_model(model) {
        endpoints.extend(["/v1/responses", "/v1/chat/completions"]);
    }
    if decision_model(model) {
        endpoints.push("/v1/decisions");
    }
    endpoints
}
pub fn resolve_decision(model: &ModelId, allowed: &[ModelId]) -> Option<ModelId> {
    let model = ModelId::from_source(trim(model.as_js_string()));
    if allowed.contains(&model) {
        return Some(model);
    }
    if model.matches_scalar("typesafe/jev-latest") {
        return allowed
            .iter()
            .find(|row| row.matches_scalar("~typesafe/jev-latest"))
            .cloned();
    }
    None
}

pub fn owner(id: &ModelId) -> JsString16 {
    let units = id.as_js_string().as_units();
    let prefix = units.split(|unit| *unit == 47).next().unwrap_or_default();
    if prefix.is_empty() {
        "openrouter".into()
    } else {
        JsString16::from_units(prefix.to_vec())
    }
}
