//! Provider-declared catalog projections; credentials/configuration are injected observations.
use gateway_catalog_input::catalog::*;
use gateway_contracts::model::ModelId;
use gateway_decision_protocol::catalog as decisions;
use gateway_model_metadata::metadata::*;
use gateway_model_selection::aliases::*;
use gateway_source::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use serde::Serialize;
pub const CEREBRAS_IDS: &[&str] = &["gpt-oss-120b", "qwen-3.8-27b"];
pub fn provider_hint(provider: &str, id: &ModelId) -> MetadataHint {
    match provider {
        "deepseek" => MetadataHint::provider(1_000_000, &["none", "low", "high", "max"], "high"),
        "cerebras" if id.matches_scalar("qwen-3.8-27b") => {
            MetadataHint::provider(131_072, &["none", "low", "medium", "high"], "high")
        }
        "cerebras" => MetadataHint::provider(131_072, &["low", "medium", "high"], "medium"),
        "lithos" => MetadataHint {
            effective_context_window_percent: Some(100),
            ..MetadataHint::provider(
                1_048_576,
                &["none", "minimal", "low", "medium", "high", "xhigh", "max"],
                "medium",
            )
        },
        "ubiquity" => MetadataHint {
            auto_compact_token_limit_tokens: Some(891_289),
            effective_context_window_percent: Some(95),
            ..MetadataHint::provider(1_048_576, &["low", "high", "max"], "high")
        },
        _ => MetadataHint::default(),
    }
}
pub fn display_name(provider: &str, id: &ModelId) -> JsString16 {
    let literal = match provider {
        "deepseek" if id.matches_scalar("deepseek-flash") => Some("DeepSeek Flash"),
        "deepseek" if id.matches_scalar("deepseek-v4-flash") => Some("DeepSeek Flash (legacy id)"),
        "deepseek" if id.matches_scalar("deepseek-v4-pro") => Some("DeepSeek Pro"),
        "cerebras" if id.matches_scalar("gpt-oss-120b") => Some("GPT-OSS 120B"),
        "cerebras" if id.matches_scalar("qwen-3.8-27b") => Some("Qwen 3.8 27B"),
        "ubiquity" => Some("Ubiquity DeepSeek Flash"),
        _ => None,
    };
    if let Some(value) = literal {
        return value.into();
    }
    if provider == "lithos" {
        let units = id.as_js_string().as_units();
        let deepseek: Vec<u16> = "DeepSeek".encode_utf16().collect();
        let base = if units.windows(deepseek.len()).any(|part| part == deepseek) {
            "LithosAI DeepSeek V4.1 Flash"
        } else {
            "LithosAI Kimi K3"
        };
        let ends = |value: &str| units.ends_with(&value.encode_utf16().collect::<Vec<_>>());
        let suffix = if ends("-ultra-chat") {
            " (ultra, chat)"
        } else if ends("-ultra") {
            " (ultra)"
        } else if ends("-fast") {
            " (fast)"
        } else {
            ""
        };
        return format!("{base}{suffix}").as_str().into();
    }
    id.as_js_string().clone()
}
pub fn provider_ids(provider: &str) -> &'static [&'static str] {
    match provider {
        "deepseek" => DEEPSEEK_IDS,
        "lithos" => LITHOS_IDS,
        "cerebras" => CEREBRAS_IDS,
        "ubiquity" => &[SYNTHETIC_MODEL],
        _ => &[],
    }
}

#[derive(Serialize)]
struct ModelRow {
    id: JsString16,
    object: &'static str,
    created: i64,
    owned_by: JsString16,
}
#[derive(Serialize)]
struct OrdinaryMetadata {
    #[serde(skip_serializing_if = "Option::is_none")]
    context_window_tokens: Option<Option<i64>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    max_context_window_tokens: Option<Option<i64>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    auto_compact_token_limit_tokens: Option<Option<i64>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    effective_context_window_percent: Option<i64>,
}
impl OrdinaryMetadata {
    fn observed(resolved: &ResolvedMetadata, nulls: bool) -> Self {
        Self {
            context_window_tokens: resolved
                .context_window_tokens
                .map(Some)
                .or_else(|| nulls.then_some(None)),
            max_context_window_tokens: resolved
                .max_context_window_tokens
                .map(Some)
                .or_else(|| nulls.then_some(None)),
            auto_compact_token_limit_tokens: resolved
                .auto_compact_token_limit_tokens
                .map(Some)
                .or_else(|| nulls.then_some(None)),
            effective_context_window_percent: resolved.effective_context_window_percent,
        }
    }
}
#[derive(Serialize)]
pub(super) struct NativeMetadata {
    #[serde(skip_serializing_if = "Option::is_none")]
    context_window: Option<Option<i64>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    max_context_window: Option<Option<i64>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    auto_compact_token_limit: Option<Option<i64>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    effective_context_window_percent: Option<i64>,
}
impl NativeMetadata {
    pub(super) fn observed(resolved: &ResolvedMetadata, nulls: bool) -> Self {
        let fields = OrdinaryMetadata::observed(resolved, nulls);
        Self {
            context_window: fields.context_window_tokens,
            max_context_window: fields.max_context_window_tokens,
            auto_compact_token_limit: fields.auto_compact_token_limit_tokens,
            effective_context_window_percent: fields.effective_context_window_percent,
        }
    }
}
#[derive(Serialize)]
struct ConfiguredCapability {
    id: JsString16,
    object: &'static str,
    owned_by: String,
    display_name: JsString16,
    upstream_provider: String,
    supported_endpoints: SourceJson,
    supported_reasoning_levels: SourceJson,
    default_reasoning_effort: Option<JsString16>,
    reasoning_effort_wire_map: SourceJson,
    #[serde(flatten)]
    metadata: OrdinaryMetadata,
    context_source: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    reasoning_source: Option<&'static str>,
}
#[derive(Serialize)]
struct OpenRouterCapability {
    id: JsString16,
    object: &'static str,
    owned_by: SourceJson,
    display_name: JsString16,
    upstream_provider: &'static str,
    supported_endpoints: SourceJson,
    supported_reasoning_levels: Vec<JsString16>,
    reasoning_effort_wire_map: SourceJson,
    #[serde(skip_serializing_if = "Option::is_none")]
    default_reasoning_effort: Option<JsString16>,
    #[serde(flatten)]
    metadata: OrdinaryMetadata,
    context_source: String,
    reasoning_source: String,
}
#[derive(Serialize)]
struct CodexCapability {
    id: JsString16,
    object: &'static str,
    owned_by: JsString16,
    display_name: SourceJson,
    upstream_provider: &'static str,
    supported_endpoints: [&'static str; 2],
    supported_reasoning_levels: Vec<JsString16>,
    default_reasoning_effort: Option<JsString16>,
    reasoning_effort_wire_map: SourceJson,
    #[serde(flatten)]
    metadata: OrdinaryMetadata,
    context_source: String,
    reasoning_source: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    prompt_cache: Option<SourceJson>,
}
pub fn configured_model_rows(
    input: &CatalogInput,
    provider: &str,
) -> Result<Vec<SourceJson>, GatewayError> {
    if !input.enabled(provider)
        || (provider == "ubiquity" && !input.synthetic_configured())
        || (provider != "ubiquity" && !input.configured(provider))
    {
        return Ok(Vec::new());
    }
    provider_ids(provider)
        .iter()
        .map(|id| {
            SourceJson::from_serializable(&ModelRow {
                id: (*id).into(),
                object: "model",
                created: if provider == "ubiquity" {
                    1_791_244_800
                } else {
                    0
                },
                owned_by: provider.into(),
            })
        })
        .collect()
}
pub fn apply_resolved_fields(
    row: &mut SourceJson,
    resolved: &ResolvedMetadata,
    native: bool,
    include_nulls: bool,
) -> Result<(), GatewayError> {
    let fields = OrdinaryMetadata::observed(resolved, include_nulls);
    let updates = if native {
        SourceJson::from_serializable(&NativeMetadata {
            context_window: fields.context_window_tokens,
            max_context_window: fields.max_context_window_tokens,
            auto_compact_token_limit: fields.auto_compact_token_limit_tokens,
            effective_context_window_percent: fields.effective_context_window_percent,
        })?
    } else {
        SourceJson::from_serializable(&fields)?
    };
    // Only this operation's four declared metadata slots may update the raw row.
    let invalid = || {
        GatewayError::unavailable(
            "Invalid source JSON serialization.",
            "source_json_serialization_invalid",
        )
    };
    let updates = source::entries(&updates).ok_or_else(invalid)?;
    if updates.is_empty() {
        return Ok(());
    }
    let mut entries = source::entries(row).ok_or_else(invalid)?;
    for (key, value) in updates {
        if let Some((_, prior)) = entries.iter_mut().find(|(name, _)| name == &key) {
            *prior = value;
        } else {
            entries.push((key, value));
        }
    }
    *row = source::object_keys(entries)?;
    Ok(())
}
pub fn configured_capabilities(
    input: &CatalogInput,
    provider: &str,
) -> Result<Vec<SourceJson>, GatewayError> {
    let mut rows = Vec::new();
    for model in configured_model_rows(input, provider)? {
        let Some(id) = source::member(&model, "id")
            .as_ref()
            .and_then(source::string)
            .map(ModelId::from_source)
        else {
            continue;
        };
        let hint = provider_hint(provider, &id);
        let mut resolved = input.metadata(&id, None, Some(hint.clone()), false);
        if provider == "ubiquity" {
            resolved = resolve_metadata(&MetadataSources {
                provider: Some(hint.clone()),
                ..MetadataSources::default()
            });
        }
        let endpoints = if provider == "cerebras" {
            vec!["/v1/chat/completions"]
        } else if provider == "ubiquity" {
            vec!["/v1/responses"]
        } else {
            vec!["/v1/chat/completions", "/v1/responses"]
        };
        let wire = if provider == "deepseek" {
            source::object(vec![("ultra", SourceJson::from_serializable("max")?)])?
        } else {
            source::object(Vec::new())?
        };
        rows.push(SourceJson::from_serializable(&ConfiguredCapability {
            id: id.as_js_string().clone(),
            object: "uos.model_capabilities",
            owned_by: provider.into(),
            display_name: display_name(provider, &id),
            upstream_provider: provider.into(),
            supported_endpoints: SourceJson::from_serializable(&endpoints)?,
            supported_reasoning_levels: SourceJson::from_serializable(
                hint.supported_reasoning_levels.as_deref().unwrap_or(&[]),
            )?,
            default_reasoning_effort: hint.default_reasoning_effort.clone(),
            reasoning_effort_wire_map: wire,
            metadata: OrdinaryMetadata::observed(&resolved, true),
            context_source: if provider == "ubiquity" {
                "openrouter".into()
            } else {
                resolved.context_source
            },
            reasoning_source: (provider == "ubiquity").then_some("provider_discovery"),
        })?);
    }
    Ok(rows)
}
pub fn openrouter_models(input: &CatalogInput) -> Result<Vec<SourceJson>, GatewayError> {
    if !input.enabled("openrouter") || !input.configured("openrouter") {
        return Ok(Vec::new());
    }
    let mut rows = Vec::new();
    for record in decisions::served_models(input.rows("openrouter")) {
        let Some(id) = source::member(record, "id")
            .as_ref()
            .and_then(source::string)
            .map(ModelId::from_source)
        else {
            continue;
        };
        rows.push(SourceJson::from_serializable(&ModelRow {
            id: id.as_js_string().clone(),
            object: "model",
            created: 0,
            owned_by: decisions::owner(&id),
        })?);
    }
    append_unique(
        &mut rows,
        [SourceJson::from_serializable(&ModelRow {
            id: "typesafe/jev-latest".into(),
            object: "model",
            created: 0,
            owned_by: "typesafe".into(),
        })?],
        "id",
    );
    Ok(rows)
}
pub fn openrouter_capabilities(input: &CatalogInput) -> Result<Vec<SourceJson>, GatewayError> {
    let mut rows = Vec::new();
    for model in openrouter_models(input)? {
        let Some(id) = source::member(&model, "id")
            .as_ref()
            .and_then(source::string)
            .map(ModelId::from_source)
        else {
            continue;
        };
        let jev = id.matches_scalar("typesafe/jev-latest");
        let resolved = input.metadata(&id, None, None, false);
        let discovered = input.rows("openrouter").iter().find(|r| {
            source::member(r, "id")
                .as_ref()
                .and_then(source::string)
                .as_ref()
                == Some(id.as_js_string())
        });
        let levels = if jev {
            vec!["none".into()]
        } else {
            resolved
                .supported_reasoning_levels
                .clone()
                .unwrap_or_else(|| vec!["none".into()])
        };
        let endpoints = if jev && discovered.is_none() {
            vec!["/v1/decisions", "/v1/systemone"]
        } else {
            discovered.map_or_else(Vec::new, decisions::endpoints)
        };
        let default = if jev {
            Some("none".into())
        } else {
            resolved.default_reasoning_effort.clone().or_else(|| {
                resolved
                    .supported_reasoning_levels
                    .is_none()
                    .then(|| "none".into())
            })
        };
        let metadata = if jev {
            OrdinaryMetadata {
                context_window_tokens: Some(None),
                max_context_window_tokens: Some(None),
                auto_compact_token_limit_tokens: Some(None),
                effective_context_window_percent: None,
            }
        } else {
            OrdinaryMetadata::observed(&resolved, true)
        };
        rows.push(SourceJson::from_serializable(&OpenRouterCapability {
            id: id.as_js_string().clone(),
            object: "uos.model_capabilities",
            owned_by: match source::member(&model, "owned_by") {
                Some(value) => value,
                None => SourceJson::from_serializable(&())?,
            },
            display_name: if jev {
                "Jev (Typesafe System One decisions)".into()
            } else {
                id.as_js_string().clone()
            },
            upstream_provider: "openrouter",
            supported_endpoints: SourceJson::from_serializable(&endpoints)?,
            supported_reasoning_levels: levels,
            reasoning_effort_wire_map: source::object(Vec::new())?,
            default_reasoning_effort: default,
            metadata,
            context_source: if jev {
                "unknown".into()
            } else {
                resolved.context_source
            },
            reasoning_source: if jev {
                "unknown".into()
            } else {
                resolved.reasoning_source
            },
        })?);
    }
    Ok(rows)
}
pub fn normalized_codex_capability(
    input: &CatalogInput,
    record: &SourceJson,
) -> Result<Option<SourceJson>, GatewayError> {
    let Some(raw) = gateway_schema_values::source_view::identifier(record) else {
        return Ok(None);
    };
    let id = ModelId::from_source(gateway_schema_values::source_view::js_trim_source(&raw));
    if id.as_js_string().as_units().is_empty() {
        return Ok(None);
    }
    let reasoning = gateway_model_metadata::reasoning::ModelReasoning::from_record(Some(record));
    let resolved = input.metadata(&id, Some(record), None, true);
    let owner = source::member(record, "owned_by")
        .as_ref()
        .and_then(source::string)
        .unwrap_or_else(|| "openai".into());
    Ok(Some(SourceJson::from_serializable(&CodexCapability {
        id: id.as_js_string().clone(),
        object: "uos.model_capabilities",
        owned_by: owner,
        display_name: match source::member(record, "display_name") {
            Some(value) => value,
            None => SourceJson::from_serializable(&())?,
        },
        upstream_provider: "codex_chatgpt",
        supported_endpoints: ["/v1/chat/completions", "/v1/responses"],
        supported_reasoning_levels: reasoning.levels.clone(),
        default_reasoning_effort: reasoning.default_level.clone(),
        reasoning_effort_wire_map: reasoning.wire_efforts.to_source_json()?,
        metadata: OrdinaryMetadata::observed(&resolved, true),
        context_source: resolved.context_source,
        reasoning_source: resolved.reasoning_source,
        prompt_cache: gateway_prompt_cache::metadata::normalize_capabilities_source(
            source::member(record, "prompt_cache").as_ref(),
        ),
    })?))
}
