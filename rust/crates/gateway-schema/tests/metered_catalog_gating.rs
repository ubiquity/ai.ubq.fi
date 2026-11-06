//! Source 54cd4cc, 990ea7c, 60a0b1e, 2e30700 (src/catalog/index.ts `meteredCatalogResponse`):
//! the storeless versioned catalog appends DeepSeek, the DeepSeek waterfall, Lithos, Cerebras and
//! OpenRouter rows only for providers the selection enables, and OpenRouter rows also need the
//! routing credential (`readOpenRouterApiKey()`), which the Rust host projects as `configured`.
use gateway_core::{error::GatewayError, source_json::SourceJson};
use gateway_schema::{
    aliases::LITHOS_IDS,
    catalog::{CatalogInput, ProviderCatalog},
    native_catalog::native_catalog_fallback,
    selection::ProviderSelection,
};
use serde_json::{Value, json};

type TestResult = Result<(), Box<dyn std::error::Error>>;

fn provider(models: &[Value], configured: bool) -> Result<ProviderCatalog, GatewayError> {
    Ok(ProviderCatalog {
        models: models
            .iter()
            .map(SourceJson::from_serializable)
            .collect::<Result<_, _>>()?,
        configured,
        available: configured,
        ..Default::default()
    })
}
fn input(selection: Option<&[&str]>, router: bool) -> Result<CatalogInput, GatewayError> {
    let mut input = CatalogInput {
        selection: selection.map(|ids| ProviderSelection {
            provider_ids: ids.iter().map(|id| (*id).to_owned()).collect(),
            updated_at_ms: 1,
        }),
        ..Default::default()
    };
    for id in ["deepseek", "lithos", "cerebras"] {
        input.providers.insert(id.into(), provider(&[], true)?);
    }
    let rows = [json!({"id":"vendor/router-model","architecture":{"modality":"text->text"}})];
    input
        .providers
        .insert("openrouter".into(), provider(&rows, router)?);
    Ok(input)
}
fn slugs(selection: Option<&[&str]>, router: bool) -> Result<Vec<String>, GatewayError> {
    let Some(body) = native_catalog_fallback(&input(selection, router)?) else {
        return Ok(vec![]);
    };
    let body: Value = body.deserialize()?;
    let rows = body["models"].as_array().cloned().unwrap_or_default();
    Ok(rows
        .iter()
        .filter_map(|row| row["slug"].as_str().map(str::to_owned))
        .collect())
}

#[test]
fn storeless_catalog_appends_only_selected_configured_providers() -> TestResult {
    let all = slugs(None, true)?;
    for expected in [
        "deepseek-flash",
        "ubiquity/deepseek-flash",
        "deepseek-ai/DeepSeek-V4.1-Flash",
        "gpt-oss-120b",
        "vendor/router-model",
    ] {
        assert!(
            all.iter().any(|slug| slug == expected),
            "{expected}: {all:?}"
        );
    }
    assert_eq!(slugs(Some(&["lithos"]), true)?, LITHOS_IDS);
    assert_eq!(slugs(Some(&["openrouter"]), true)?, ["vendor/router-model"]);
    assert_eq!(slugs(Some(&["codex"]), true)?, Vec::<String>::new());
    Ok(())
}

#[test]
fn selected_openrouter_without_a_routing_credential_lists_no_rows() -> TestResult {
    assert_eq!(slugs(Some(&["openrouter"]), false)?, Vec::<String>::new());
    let all = slugs(None, false)?;
    assert!(
        !all.iter().any(|slug| slug == "vendor/router-model"),
        "{all:?}"
    );
    Ok(())
}
