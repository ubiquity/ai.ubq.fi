//! Match the existing direct-provider priority before waiting for discovery.
use gateway_contracts::{model::ModelId, provider::UpstreamProvider};
use gateway_providers::request::canonical_source_model;
use gateway_schema::{aliases, catalog::CatalogInput, util::js_whitespace};

pub fn uses_openrouter(input: &CatalogInput, model: &ModelId) -> bool {
    if aliases::is_synthetic(model) {
        return input.enabled("ubiquity") && input.configured("openrouter");
    }
    if !input.enabled("openrouter") || !input.configured("openrouter") {
        return false;
    }
    if [
        ("deepseek", UpstreamProvider::Deepseek),
        ("lithos", UpstreamProvider::Lithos),
        ("cerebras", UpstreamProvider::Cerebras),
    ]
    .iter()
    .any(|(name, provider)| {
        input.enabled(name) && canonical_source_model(*provider, model, &[]).is_some()
    }) {
        return false;
    }
    let parts = model
        .as_js_string()
        .as_units()
        .split(|unit| *unit == 47)
        .collect::<Vec<_>>();
    parts.len() == 2
        && parts.iter().all(|part| {
            !part.is_empty()
                && !part
                    .iter()
                    .any(|unit| char::from_u32(u32::from(*unit)).is_some_and(js_whitespace))
        })
}
