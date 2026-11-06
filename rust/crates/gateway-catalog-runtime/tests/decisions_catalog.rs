//! Catalog attribution for decision models (tests/decisions.test.ts, cfe8458).
use gateway_catalog_runtime::{host::CatalogProvider, providers::parse_page};
use gateway_core::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use gateway_schema::{
    aliases::SourceStringCollator,
    catalog::{CatalogInput, ProviderCatalog},
    catalog_views::{build_catalog_snapshot, capabilities, ordinary_models},
    native_catalog::native_catalog_fallback,
    provider_catalog::openrouter_endpoints,
};
use serde_json::{Value, json};
use std::cmp::Ordering;

type TestResult = Result<(), Box<dyn std::error::Error>>;

struct Ordinal;
impl SourceStringCollator for Ordinal {
    fn compare(&self, left: &JsString16, right: &JsString16) -> Result<Ordering, GatewayError> {
        Ok(left.cmp(right))
    }
}

fn catalogue() -> Value {
    json!({"data":[
        {"id":"vendor/text","context_length":1000,"top_provider":{"context_length":1000},
         "architecture":{"input_modalities":["text"],"output_modalities":["text"]},
         "reasoning":{"supported_efforts":["high"],"default_effort":"high"}},
        {"id":"baseten/elephant-1:free","context_length":16384,
         "architecture":{"input_modalities":["text"],"output_modalities":[" decisions ","decisions",""]},
         "reasoning":null},
        {"id":"~typesafe/jev-latest","context_length":262144,
         "architecture":{"input_modalities":["text","image"],"output_modalities":["decisions"]},
         "reasoning":null}
    ]})
}
fn rows() -> Result<Vec<SourceJson>, GatewayError> {
    let payload = SourceJson::from_serializable(&catalogue())?;
    Ok(parse_page(CatalogProvider::Openrouter, &payload)?.0)
}
fn input() -> Result<CatalogInput, GatewayError> {
    let mut input = CatalogInput::default();
    let catalog = ProviderCatalog {
        models: rows()?,
        updated_at_ms: Some(1),
        configured: true,
        available: true,
        stale: false,
    };
    input.providers.insert("openrouter".into(), catalog);
    Ok(input)
}
fn value(source: &SourceJson) -> Result<Value, GatewayError> {
    source.deserialize::<Value>()
}
fn find<'a>(rows: &'a Value, id: &str) -> Option<&'a Value> {
    rows.as_array()?.iter().find(|row| row["id"] == id)
}

#[test]
fn discovery_requests_text_and_decisions_and_records_declared_modalities() -> TestResult {
    let url = CatalogProvider::Openrouter.models_url().unwrap_or_default();
    assert!(url.ends_with("/api/v1/models?output_modalities=text%2Cdecisions"));
    let rows = rows()?;
    let parsed: Vec<Value> = rows.iter().map(value).collect::<Result<_, _>>()?;
    assert_eq!(parsed[0]["output_modalities"], json!(["text"]));
    assert_eq!(parsed[1]["output_modalities"], json!(["decisions"]));
    let endpoints: Vec<Vec<&str>> = rows.iter().map(openrouter_endpoints).collect();
    assert_eq!(endpoints[0], ["/v1/responses", "/v1/chat/completions"]);
    assert_eq!(endpoints[1], ["/v1/decisions"]);
    let legacy = SourceJson::from_serializable(&json!({"id":"legacy/text"}))?;
    assert_eq!(
        openrouter_endpoints(&legacy),
        ["/v1/responses", "/v1/chat/completions"]
    );
    let both = SourceJson::from_serializable(
        &json!({"id":"x/y","output_modalities":["decisions","text"]}),
    )?;
    assert_eq!(
        openrouter_endpoints(&both),
        ["/v1/responses", "/v1/chat/completions", "/v1/decisions"]
    );
    let image = SourceJson::from_serializable(&json!({"id":"x/y","output_modalities":["image"]}))?;
    assert!(openrouter_endpoints(&image).is_empty());
    Ok(())
}

#[test]
fn decision_models_are_exposed_on_the_decision_route_and_hidden_from_codex_text() -> TestResult {
    let input = input()?;
    let models = value(&ordinary_models(&input))?;
    for id in [
        "baseten/elephant-1:free",
        "~typesafe/jev-latest",
        "typesafe/jev-latest",
    ] {
        assert!(find(&models["data"], id).is_some(), "{id}");
    }

    let capabilities = value(&capabilities(&input))?;
    let endpoints =
        |id: &str| find(&capabilities["data"], id).map(|row| row["supported_endpoints"].clone());
    assert_eq!(
        endpoints("baseten/elephant-1:free"),
        Some(json!(["/v1/decisions"]))
    );
    assert_eq!(
        endpoints("vendor/text"),
        Some(json!(["/v1/responses", "/v1/chat/completions"]))
    );
    assert_eq!(
        endpoints("typesafe/jev-latest"),
        Some(json!(["/v1/decisions", "/v1/systemone"]))
    );

    let snapshot = value(&build_catalog_snapshot(&input, &Ordinal)?)?;
    let entry = find(&snapshot["models"], "baseten/elephant-1:free")
        .map(|row| row["providers"][0]["supported_endpoints"].clone());
    assert_eq!(entry, Some(json!(["/v1/decisions"])));
    let alias = find(&snapshot["models"], "typesafe/jev-latest")
        .map(|row| row["providers"][0]["supported_endpoints"].clone());
    assert_eq!(alias, Some(json!(["/v1/decisions", "/v1/systemone"])));
    assert_eq!(snapshot["sources"]["openrouter"]["count"], 4);

    let native = native_catalog_fallback(&input)
        .map(|catalog| value(&catalog))
        .transpose()?;
    let slugs: Vec<Value> = native
        .as_ref()
        .and_then(|catalog| catalog["models"].as_array())
        .map(|models| models.iter().map(|row| row["slug"].clone()).collect())
        .unwrap_or_default();
    // The gateway-owned synthetic row stays; only the OpenRouter text row joins it.
    assert_eq!(
        slugs,
        [json!("ubiquity/deepseek-flash"), json!("vendor/text")]
    );
    Ok(())
}
