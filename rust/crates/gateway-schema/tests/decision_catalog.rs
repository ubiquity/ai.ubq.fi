use gateway_model_selection::{aliases::SourceStringCollator, selection::ProviderSelection};
use gateway_schema::{
    catalog::{CatalogInput, ProviderCatalog},
    catalog_views, native_catalog,
};
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use serde_json::{Value, json};
use std::{cmp::Ordering, collections::BTreeMap};
struct Order;
impl SourceStringCollator for Order {
    fn compare(&self, left: &JsString16, right: &JsString16) -> Result<Ordering, GatewayError> {
        Ok(left.cmp(right))
    }
}
#[test]
fn every_listing_surface_preserves_decision_modality_and_native_excludes_it()
-> Result<(), Box<dyn std::error::Error>> {
    let rows = vec![
        SourceJson::from_serializable(&json!({"id":"a/text","output_modalities":["text"]}))?,
        SourceJson::from_serializable(
            &json!({"id":"a/decision","output_modalities":["decisions"]}),
        )?,
        SourceJson::from_serializable(
            &json!({"id":"a/dual","output_modalities":["text","decisions"]}),
        )?,
        SourceJson::from_serializable(&json!({"id":"a/image","output_modalities":["image"]}))?,
    ];
    let input = CatalogInput {
        providers: BTreeMap::from([(
            "openrouter".into(),
            ProviderCatalog {
                models: rows,
                configured: true,
                available: true,
                ..Default::default()
            },
        )]),
        selection: ProviderSelection::normalize(
            &json!({"provider_ids":["openrouter"],"updated_at_ms":1}),
        ),
        ..Default::default()
    };
    let ordinary = catalog_views::ordinary_models(&input).deserialize::<Value>()?;
    assert!(
        ordinary["data"]
            .as_array()
            .ok_or("missing data")?
            .iter()
            .any(|row| row["id"] == "a/decision")
    );
    assert!(
        !ordinary["data"]
            .as_array()
            .ok_or("missing data")?
            .iter()
            .any(|row| row["id"] == "a/image")
    );
    let capabilities = catalog_views::capabilities(&input).deserialize::<Value>()?;
    let decision = capabilities["data"]
        .as_array()
        .ok_or("missing capabilities")?
        .iter()
        .find(|row| row["id"] == "a/decision")
        .ok_or("missing decision")?;
    assert_eq!(decision["supported_endpoints"], json!(["/v1/decisions"]));
    let capability_rows = capabilities["data"]
        .as_array()
        .ok_or("missing capabilities")?;
    let alias = capability_rows
        .iter()
        .find(|row| row["id"] == "typesafe/jev-latest")
        .ok_or("missing alias")?;
    assert_eq!(
        alias["supported_endpoints"],
        json!(["/v1/decisions", "/v1/systemone"])
    );
    let dual = capability_rows
        .iter()
        .find(|row| row["id"] == "a/dual")
        .ok_or("missing dual")?;
    assert_eq!(
        dual["supported_endpoints"],
        json!(["/v1/responses", "/v1/chat/completions", "/v1/decisions"])
    );
    assert!(!capability_rows.iter().any(|row| row["id"] == "a/image"));
    for (catalog, field) in [
        (
            catalog_views::build_catalog_snapshot(&input, &Order)?,
            "models",
        ),
        (catalog_views::public_catalog(&input, &Order)?, "data"),
    ] {
        let catalog = catalog.deserialize::<Value>()?;
        let decision = catalog[field]
            .as_array()
            .ok_or("missing public models")?
            .iter()
            .find(|row| row["id"] == "a/decision")
            .ok_or("missing public decision")?;
        assert_eq!(
            decision["providers"][0]["supported_endpoints"],
            json!(["/v1/decisions"])
        );
        let rows = catalog[field].as_array().ok_or("missing public models")?;
        let alias = rows
            .iter()
            .find(|row| row["id"] == "typesafe/jev-latest")
            .ok_or("missing public alias")?;
        assert_eq!(
            alias["providers"][0]["supported_endpoints"],
            json!(["/v1/decisions", "/v1/systemone"])
        );
        assert!(!rows.iter().any(|row| row["id"] == "a/image"));
        if field == "models" {
            assert_eq!(catalog["sources"]["openrouter"]["count"], 4);
        }
    }
    let native = native_catalog::native_catalog_fallback(&input)
        .ok_or("missing native catalog")?
        .deserialize::<Value>()?;
    let models = native["models"].as_array().ok_or("missing native models")?;
    assert!(models.iter().any(|row| row["slug"] == "a/text"));
    assert!(models.iter().any(|row| row["slug"] == "a/dual"));
    assert!(!models.iter().any(|row| row["slug"] == "a/decision"));
    assert!(!models.iter().any(|row| row["slug"] == "a/image"));
    Ok(())
}
