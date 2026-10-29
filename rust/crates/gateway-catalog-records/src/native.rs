//! Rich exact-client-version record codecs over finite DATA; no transport or store authority.
use crate::provider::{NativeMetadata, display_name, provider_hint, provider_ids};
use gateway_catalog_input::catalog::*;
use gateway_contracts::model::ModelId;
use gateway_model_metadata::metadata::*;
use gateway_prompt_policy::declaration;
use gateway_source::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use serde::Serialize;
#[derive(Serialize)]
struct NativeLevel {
    effort: JsString16,
    description: JsString16,
}
#[derive(Serialize)]
struct NativeReasoning {
    supported_reasoning_levels: Vec<NativeLevel>,
    #[serde(skip_serializing_if = "Option::is_none")]
    default_reasoning_level: Option<JsString16>,
}
#[derive(Serialize)]
struct Truncation {
    mode: &'static str,
    limit: i64,
}
#[derive(Serialize)]
struct Forwarding {
    version: &'static str,
    mode: &'static str,
    per_message_limit: usize,
}
#[derive(Serialize)]
struct NativeCommon {
    shell_type: &'static str,
    visibility: &'static str,
    supported_in_api: bool,
    priority: i64,
    availability_nux: (),
    upgrade: (),
    base_instructions: &'static str,
    support_verbosity: bool,
    default_verbosity: (),
    apply_patch_tool_type: (),
    web_search_tool_type: &'static str,
    truncation_policy: Truncation,
    #[serde(skip_serializing_if = "Option::is_none")]
    forwarding_policy: Option<Forwarding>,
    supports_parallel_tool_calls: bool,
    experimental_supported_tools: [SourceJson; 0],
}
impl NativeCommon {
    fn observed(priority: i64, forwarding: bool) -> Self {
        let policy = declaration::FORWARDED_PAYLOAD_DECLARATION;
        Self {
            shell_type: "shell_command",
            visibility: "list",
            supported_in_api: true,
            priority,
            availability_nux: (),
            upgrade: (),
            base_instructions: "",
            support_verbosity: false,
            default_verbosity: (),
            apply_patch_tool_type: (),
            web_search_tool_type: "text",
            truncation_policy: Truncation {
                mode: "tokens",
                limit: 10000,
            },
            forwarding_policy: forwarding.then_some(Forwarding {
                version: policy.version,
                mode: policy.mode,
                per_message_limit: policy.per_message_limit,
            }),
            supports_parallel_tool_calls: false,
            experimental_supported_tools: [],
        }
    }
}
#[derive(Serialize)]
struct PaidNative {
    slug: JsString16,
    display_name: JsString16,
    #[serde(skip_serializing_if = "Option::is_none")]
    description: Option<SourceJson>,
    owned_by: SourceJson,
    supported_endpoint_types: SourceJson,
    #[serde(flatten)]
    reasoning: NativeReasoning,
    #[serde(flatten)]
    metadata: NativeMetadata,
    #[serde(flatten)]
    common: NativeCommon,
}
#[derive(Serialize)]
struct ClaudeNative {
    slug: SourceJson,
    display_name: SourceJson,
    owned_by: &'static str,
    supported_endpoint_types: [&'static str; 2],
    supported_reasoning_levels: Vec<NativeLevel>,
    #[serde(skip_serializing_if = "Option::is_none")]
    default_reasoning_level: Option<&'static str>,
    #[serde(skip_serializing_if = "Option::is_none")]
    context_window: Option<i64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    max_context_window: Option<i64>,
    #[serde(skip_serializing_if = "Option::is_none")]
    max_output_tokens: Option<i64>,
    #[serde(flatten)]
    common: NativeCommon,
}
#[derive(Serialize)]
struct ConfiguredNative {
    slug: JsString16,
    display_name: JsString16,
    description: &'static str,
    owned_by: String,
    supported_endpoint_types: Vec<&'static str>,
    supported_reasoning_levels: Vec<NativeLevel>,
    default_reasoning_level: Option<JsString16>,
    #[serde(flatten)]
    metadata: NativeMetadata,
    #[serde(flatten)]
    common: NativeCommon,
}
#[derive(Serialize)]
struct OpenRouterNative {
    slug: JsString16,
    display_name: JsString16,
    description: &'static str,
    owned_by: JsString16,
    supported_endpoint_types: [&'static str; 2],
    #[serde(flatten)]
    reasoning: NativeReasoning,
    #[serde(flatten)]
    metadata: NativeMetadata,
    #[serde(flatten)]
    common: NativeCommon,
}
#[derive(Serialize)]
struct NativeEnvelope {
    models: Vec<SourceJson>,
}
fn description(effort: &JsString16) -> JsString16 {
    if gateway_schema_values::source_view::matches(effort, "none") {
        "No reasoning".into()
    } else {
        gateway_schema_values::source_view::concat(&[&"Reasoning effort: ".into(), effort])
    }
}
fn reasoning_fields(resolved: &ResolvedMetadata, external: bool) -> NativeReasoning {
    let levels = resolved
        .supported_reasoning_levels
        .as_ref()
        .map(|levels| {
            if external {
                ascending_external_levels(levels)
            } else {
                levels.clone()
            }
        })
        .filter(|l| !l.is_empty())
        .unwrap_or_else(|| vec!["none".into()]);
    let default = resolved.default_reasoning_effort.clone().or_else(|| {
        resolved
            .supported_reasoning_levels
            .is_none()
            .then(|| "none".into())
    });
    NativeReasoning {
        supported_reasoning_levels: levels
            .into_iter()
            .map(|effort| NativeLevel {
                description: description(&effort),
                effort,
            })
            .collect(),
        default_reasoning_level: default,
    }
}
pub fn paid_native_record(
    model: &SourceJson,
    input: &CatalogInput,
) -> Result<SourceJson, GatewayError> {
    let id = ModelId::from_source(
        source::member(model, "id")
            .as_ref()
            .and_then(source::string)
            .unwrap_or_default(),
    );
    let codex = input.codex().and_then(|s| s.find(&id));
    let resolved = input.metadata(&id, codex, None, codex.is_some());
    SourceJson::from_serializable(&PaidNative {
        slug: id.as_js_string().clone(),
        display_name: id.as_js_string().clone(),
        description: source::member(model, "description"),
        owned_by: match source::member(model, "owned_by") {
            Some(value) => value,
            None => SourceJson::from_serializable(&())?,
        },
        supported_endpoint_types: match source::member(model, "supported_endpoint_types") {
            Some(value) => value,
            None => SourceJson::from_serializable(&())?,
        },
        reasoning: reasoning_fields(&resolved, codex.is_none()),
        metadata: NativeMetadata::observed(&resolved, false),
        common: NativeCommon::observed(1000, false),
    })
}
pub fn claude_native_record(model: &SourceJson) -> Result<SourceJson, GatewayError> {
    let id = match source::member(model, "id") {
        Some(value) => value,
        None => SourceJson::from_serializable(&())?,
    };
    let levels = claude_levels(model);
    let window = gateway_schema_values::source_view::positive(
        source::member(model, "context_window_tokens").as_ref(),
    );
    SourceJson::from_serializable(&ClaudeNative {
        slug: id.clone(),
        display_name: source::member(model, "display_name").unwrap_or(id),
        owned_by: "anthropic",
        supported_endpoint_types: ["openai-response", "openai"],
        supported_reasoning_levels: levels
            .into_iter()
            .map(|effort| NativeLevel {
                description: if gateway_schema_values::source_view::matches(&effort, "none") {
                    "No reasoning".into()
                } else {
                    effort.clone()
                },
                effort,
            })
            .collect(),
        default_reasoning_level: source::member(model, "supports_disabled_thinking")
            .is_some_and(|v| v.as_str() == "true")
            .then_some("none"),
        context_window: window,
        max_context_window: window,
        max_output_tokens: gateway_schema_values::source_view::positive(
            source::member(model, "max_output_tokens").as_ref(),
        ),
        common: NativeCommon::observed(1000, false),
    })
}
pub fn configured_native_records(
    input: &CatalogInput,
    provider: &str,
) -> Result<Vec<SourceJson>, GatewayError> {
    if !input.enabled(provider)
        || (provider == "ubiquity" && !input.synthetic_configured())
        || (provider != "ubiquity" && !input.configured(provider))
    {
        return Ok(Vec::new());
    }
    let mut rows = Vec::new();
    for id in provider_ids(provider) {
        let id = ModelId::from_scalar(id);
        let hint = provider_hint(provider, &id);
        let resolved = if provider == "cerebras" || provider == "ubiquity" {
            resolve_metadata(&MetadataSources {
                provider: Some(hint.clone()),
                ..MetadataSources::default()
            })
        } else {
            input.metadata(&id, None, Some(hint.clone()), false)
        };
        let levels = hint
            .supported_reasoning_levels
            .as_deref()
            .unwrap_or(&[])
            .iter()
            .filter_map(gateway_model_metadata::advertised_level::AdvertisedLevel::string)
            .map(|effort| {
                let label = if provider == "deepseek" {
                    JsString16::from_utf8(
                        if gateway_schema_values::source_view::matches(&effort, "none") {
                            "Disable thinking mode"
                        } else if gateway_schema_values::source_view::matches(&effort, "low") {
                            "Thinking effort: low"
                        } else if gateway_schema_values::source_view::matches(&effort, "high") {
                            "Thinking effort: high"
                        } else {
                            "Thinking effort: maximum"
                        },
                    )
                } else {
                    description(&effort)
                };
                NativeLevel {
                    effort,
                    description: label,
                }
            })
            .collect();
        rows.push(SourceJson::from_serializable(&ConfiguredNative {slug:id.as_js_string().clone(),display_name:display_name(provider,&id),description:match provider {"deepseek"=>"DeepSeek official API (deepseek-flash) served by this gateway.","lithos"=>"LithosAI API (Chat Completions) served by this gateway.","cerebras"=>"Cerebras API (Chat Completions) served by this gateway.",_=>"Latest DeepSeek Flash waterfall: official DeepSeek, OpenRouter, LithosAI, Surplus, and OpenLux. Each hop uses the provider's latest alias and falls back only on infrastructure or serving failures."},owned_by:provider.into(),supported_endpoint_types:if provider=="ubiquity" {vec!["openai-response"]} else {vec!["openai-response","openai-chat"]},supported_reasoning_levels:levels,default_reasoning_level:hint.default_reasoning_effort.clone(),metadata:NativeMetadata::observed(&resolved,false),common:NativeCommon::observed(if provider=="ubiquity" {0}else{1},provider!="ubiquity")})?);
    }
    Ok(rows)
}
fn openrouter_native(input: &CatalogInput) -> Result<Vec<SourceJson>, GatewayError> {
    if !input.enabled("openrouter") || !input.configured("openrouter") {
        return Ok(Vec::new());
    }
    let mut rows = Vec::new();
    for model in input
        .rows("openrouter")
        .iter()
        .filter(|row| gateway_decision_protocol::catalog::text_model(row))
    {
        let Some(raw) = source::member(model, "id")
            .as_ref()
            .and_then(source::string)
        else {
            continue;
        };
        let id = ModelId::from_source(raw);
        let resolved = input.metadata(&id, None, None, false);
        rows.push(SourceJson::from_serializable(&OpenRouterNative {slug:id.as_js_string().clone(),display_name:id.as_js_string().clone(),description:"OpenRouter upstream (Chat Completions and Responses) served by this gateway.",owned_by:gateway_decision_protocol::catalog::owner(&id),supported_endpoint_types:["openai-response","openai-chat"],reasoning:reasoning_fields(&resolved,true),metadata:NativeMetadata::observed(&resolved,false),common:NativeCommon::observed(1000,false)})?);
    }
    Ok(rows)
}
fn append_native(rows: &mut Vec<SourceJson>, entries: impl IntoIterator<Item = SourceJson>) {
    for record in entries {
        let id = gateway_schema_values::source_view::identifier(&record).unwrap_or_default();
        if !rows.iter().any(|r| {
            gateway_schema_values::source_view::identifier(r).is_some_and(|s| {
                gateway_schema_values::source_view::js_trim_source(&s)
                    == gateway_schema_values::source_view::js_trim_source(&id)
            })
        }) {
            rows.push(record);
        }
    }
}
fn native_id(row: &SourceJson) -> JsString16 {
    gateway_schema_values::source_view::normalized(
        gateway_source::source_serialization::member(row, "slug").as_ref(),
    )
    .or_else(|| {
        gateway_source::source_serialization::member(row, "id")
            .as_ref()
            .and_then(gateway_source::source_serialization::string)
            .map(|id| gateway_schema_values::source_view::js_trim_source(&id))
    })
    .unwrap_or_default()
}
fn unique_paid(input: &CatalogInput) -> Vec<SourceJson> {
    let mut rows = Vec::new();
    for provider in ["openlux", "surplus"] {
        for record in input.available_rows(provider) {
            if endpoint_supports(record, "openai-response")
                && !rows.iter().any(|r| {
                    gateway_source::source_serialization::member(r, "id")
                        .as_ref()
                        .and_then(gateway_source::source_serialization::string)
                        == gateway_source::source_serialization::member(record, "id")
                            .as_ref()
                            .and_then(gateway_source::source_serialization::string)
                })
            {
                rows.push(record.clone());
            }
        }
    }
    rows
}
fn merge_configured_native(
    rows: &mut Vec<SourceJson>,
    input: &CatalogInput,
    provider: &str,
) -> Result<(), GatewayError> {
    let configured = configured_native_records(input, provider)?;
    if provider == "deepseek" {
        for row in rows.iter_mut() {
            if let Some(replacement) = configured
                .iter()
                .find(|entry| native_id(entry) == native_id(row))
            {
                *row = replacement.clone();
            }
        }
    }
    append_native(rows, configured);
    Ok(())
}
pub fn native_catalog(
    stored: Option<&SourceJson>,
    input: &CatalogInput,
) -> Result<SourceJson, GatewayError> {
    let mut rows = if input.enabled("codex") {
        stored
            .and_then(|v| source::member(v, "models"))
            .as_ref()
            .and_then(source::array)
            .unwrap_or_default()
    } else {
        Vec::new()
    };
    for row in unique_paid(input) {
        append_native(&mut rows, [paid_native_record(&row, input)?]);
    }
    for row in input.available_rows("claude_code") {
        append_native(&mut rows, [claude_native_record(row)?]);
    }
    for provider in ["deepseek", "ubiquity", "lithos", "cerebras"] {
        merge_configured_native(&mut rows, input, provider)?;
    }
    append_native(&mut rows, openrouter_native(input)?);
    let mut fields = match stored.filter(|v| v.as_str().starts_with('{')) {
        Some(value) => source::entries(value).ok_or_else(|| {
            GatewayError::unavailable(
                "Invalid source JSON serialization.",
                "source_json_serialization_invalid",
            )
        })?,
        None => Vec::new(),
    };
    let key = JsString16::from_utf8("models");
    let value = source::json_array(&filter_whitelist(rows, input.whitelist.as_ref(), true))?;
    if let Some((_, prior)) = fields.iter_mut().find(|(name, _)| name == &key) {
        *prior = value;
    } else {
        fields.push((key, value));
    }
    source::object_keys(fields)
}
pub fn native_catalog_fallback(input: &CatalogInput) -> Result<Option<SourceJson>, GatewayError> {
    let paid = unique_paid(input);
    let mut rows = Vec::new();
    for row in input.available_rows("claude_code") {
        append_native(&mut rows, [claude_native_record(row)?]);
    }
    for row in paid {
        append_native(&mut rows, [paid_native_record(&row, input)?]);
    }
    for provider in ["deepseek", "ubiquity", "lithos", "cerebras"] {
        merge_configured_native(&mut rows, input, provider)?;
    }
    append_native(&mut rows, openrouter_native(input)?);
    if rows.is_empty() {
        Ok(None)
    } else {
        Ok(Some(SourceJson::from_serializable(&NativeEnvelope {
            models: filter_whitelist(rows, input.whitelist.as_ref(), true),
        })?))
    }
}
