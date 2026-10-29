//! Rich exact-client-version Codex catalog. Never substitute the official model-list envelope.
use crate::{model_control as c, provider_catalog::*};
use gateway_catalog_input::catalog::*;
use gateway_model_metadata::metadata::*;
use gateway_source::{js_string::JsString16, source_json::SourceJson};
use {gateway_contracts::model::ModelId, gateway_prompt_policy::declaration};
fn description(effort: &JsString16) -> JsString16 {
    if gateway_schema_values::source_view::matches(effort, "none") {
        "No reasoning".into()
    } else {
        gateway_schema_values::source_view::concat(&[&"Reasoning effort: ".into(), effort])
    }
}
fn common_fields(row: &mut SourceJson, priority: i64, forwarding: bool) {
    for (key, value) in [
        ("shell_type", c::json("shell_command")),
        ("visibility", c::json("list")),
        ("supported_in_api", c::json(&true)),
        ("priority", c::json(&priority)),
        ("availability_nux", c::json(&())),
        ("upgrade", c::json(&())),
        ("base_instructions", c::json("")),
        ("support_verbosity", c::json(&false)),
        ("default_verbosity", c::json(&())),
        ("apply_patch_tool_type", c::json(&())),
        ("web_search_tool_type", c::json("text")),
        (
            "truncation_policy",
            c::object(vec![
                ("mode", c::json("tokens")),
                ("limit", c::json(&10000)),
            ]),
        ),
    ] {
        c::set(row, key, value);
    }
    if let (true, policy) = (forwarding, declaration::FORWARDED_PAYLOAD_DECLARATION) {
        c::set(
            row,
            "forwarding_policy",
            c::object(vec![
                ("version", c::json(policy.version)),
                ("mode", c::json(policy.mode)),
                ("per_message_limit", c::json(&policy.per_message_limit)),
            ]),
        );
    }
    c::set(row, "supports_parallel_tool_calls", c::json(&false));
    c::set(
        row,
        "experimental_supported_tools",
        c::json(&Vec::<SourceJson>::new()),
    );
}
fn reasoning_fields(row: &mut SourceJson, resolved: &ResolvedMetadata, external: bool) {
    let levels = resolved.supported_reasoning_levels.as_ref().map(|levels| {
        if external {
            ascending_external_levels(levels)
        } else {
            levels.clone()
        }
    });
    let levels = levels
        .filter(|l| !l.is_empty())
        .unwrap_or_else(|| vec!["none".into()]);
    c::set(
        row,
        "supported_reasoning_levels",
        c::json(
            &levels
                .iter()
                .map(|e| {
                    c::object(vec![
                        ("effort", c::json(e)),
                        ("description", c::json(&description(e))),
                    ])
                })
                .collect::<Vec<_>>(),
        ),
    );
    if let Some(default) = &resolved.default_reasoning_effort {
        c::set(row, "default_reasoning_level", c::json(default));
    } else if resolved.supported_reasoning_levels.is_none() {
        c::set(row, "default_reasoning_level", c::json("none"));
    }
}
pub fn paid_native_record(model: &SourceJson, input: &CatalogInput) -> SourceJson {
    let id = ModelId::from_source(
        gateway_source::source_serialization::member(model, "id")
            .as_ref()
            .and_then(gateway_source::source_serialization::string)
            .unwrap_or_default(),
    );
    let codex = input.codex().and_then(|s| s.find(&id));
    let resolved = input.metadata(&id, codex, None, codex.is_some());
    let mut row = c::object(vec![("slug", c::json(&id)), ("display_name", c::json(&id))]);
    if let Some(description) = gateway_source::source_serialization::member(model, "description") {
        c::set(&mut row, "description", description);
    }
    for key in ["owned_by", "supported_endpoint_types"] {
        c::set(
            &mut row,
            key,
            gateway_source::source_serialization::member(model, key)
                .unwrap_or_else(|| c::json(&())),
        );
    }
    reasoning_fields(&mut row, &resolved, codex.is_none());
    apply_resolved_fields(&mut row, &resolved, true, false);
    common_fields(&mut row, 1000, false);
    row
}
pub fn claude_native_record(model: &SourceJson) -> SourceJson {
    let id =
        gateway_source::source_serialization::member(model, "id").unwrap_or_else(|| c::json(&()));
    let levels = claude_levels(model);
    let mut row = c::object(vec![
        ("slug", id.clone()),
        (
            "display_name",
            gateway_source::source_serialization::member(model, "display_name").unwrap_or(id),
        ),
        ("owned_by", c::json("anthropic")),
        (
            "supported_endpoint_types",
            c::json(&["openai-response", "openai"]),
        ),
        (
            "supported_reasoning_levels",
            c::json(
                &levels
                    .iter()
                    .map(|e| {
                        c::object(vec![
                            ("effort", c::json(e)),
                            (
                                "description",
                                c::json(
                                    &if gateway_schema_values::source_view::matches(e, "none") {
                                        JsString16::from_utf8("No reasoning")
                                    } else {
                                        e.clone()
                                    },
                                ),
                            ),
                        ])
                    })
                    .collect::<Vec<_>>(),
            ),
        ),
    ]);
    if gateway_source::source_serialization::member(model, "supports_disabled_thinking")
        .is_some_and(|v| v.as_str() == "true")
    {
        c::set(&mut row, "default_reasoning_level", c::json("none"));
    }
    if let Some(window) = gateway_schema_values::source_view::positive(
        gateway_source::source_serialization::member(model, "context_window_tokens").as_ref(),
    ) {
        c::set(&mut row, "context_window", c::json(&window));
        c::set(&mut row, "max_context_window", c::json(&window));
    }
    if let Some(max) = gateway_schema_values::source_view::positive(
        gateway_source::source_serialization::member(model, "max_output_tokens").as_ref(),
    ) {
        c::set(&mut row, "max_output_tokens", c::json(&max));
    }
    common_fields(&mut row, 1000, false);
    row
}
pub fn configured_native_records(input: &CatalogInput, provider: &str) -> Vec<SourceJson> {
    if !input.enabled(provider)
        || (provider == "ubiquity" && !input.synthetic_configured())
        || (provider != "ubiquity" && !input.configured(provider))
    {
        return Vec::new();
    }
    provider_ids(provider).iter().map(|id|{
        let id=ModelId::from_scalar(id);let mut row=c::object(vec![("slug",c::json(&id)),("display_name",c::json(&display_name(provider,&id)))]);
        c::set(&mut row,"description",c::json(match provider{"deepseek"=>"DeepSeek official API (deepseek-flash) served by this gateway.","lithos"=>"LithosAI API (Chat Completions) served by this gateway.","cerebras"=>"Cerebras API (Chat Completions) served by this gateway.",_=>"Latest DeepSeek Flash waterfall: official DeepSeek, OpenRouter, LithosAI, Surplus, and OpenLux. Each hop uses the provider's latest alias and falls back only on infrastructure or serving failures."}));
        c::set(&mut row,"owned_by",c::json(provider));c::set(&mut row,"supported_endpoint_types",c::json(&if provider=="ubiquity"{vec!["openai-response"]}else{vec!["openai-response","openai-chat"]}));
        let hint=provider_hint(provider,&id);let resolved=if provider=="cerebras"||provider=="ubiquity"{resolve_metadata(&MetadataSources{provider:Some(hint.clone()),..MetadataSources::default()})}else{input.metadata(&id,None,Some(hint.clone()),false)};
        let levels=hint.supported_reasoning_levels.as_deref().unwrap_or(&[]);let levels=levels.iter().filter_map(gateway_model_metadata::advertised_level::AdvertisedLevel::string).map(|e|{
            let label=if provider=="deepseek"{JsString16::from_utf8(if gateway_schema_values::source_view::matches(&e,"none"){"Disable thinking mode"}else if gateway_schema_values::source_view::matches(&e,"low"){"Thinking effort: low"}else if gateway_schema_values::source_view::matches(&e,"high"){"Thinking effort: high"}else{"Thinking effort: maximum"})}else{description(&e)};
            c::object(vec![("effort",c::json(&e)),("description",c::json(&label))])
        }).collect::<Vec<_>>();
        c::set(&mut row,"supported_reasoning_levels",c::json(&levels));c::set(&mut row,"default_reasoning_level",c::json(&hint.default_reasoning_effort));apply_resolved_fields(&mut row,&resolved,true,false);common_fields(&mut row,if provider=="ubiquity"{0}else{1},provider!="ubiquity");row
    }).collect()
}
fn openrouter_native(input: &CatalogInput) -> Vec<SourceJson> {
    if !input.enabled("openrouter") || !input.configured("openrouter") {
        return Vec::new();
    }
    input.rows("openrouter").iter().filter(|row| gateway_decision_protocol::catalog::text_model(row)).filter_map(|model|{
        let id=ModelId::from_source(gateway_source::source_serialization::member(model,"id").as_ref().and_then(gateway_source::source_serialization::string)?);let resolved=input.metadata(&id,None,None,false);
        let mut row=c::object(vec![("slug",c::json(&id)),("display_name",c::json(&id)),("description",c::json("OpenRouter upstream (Chat Completions and Responses) served by this gateway.")),("owned_by",c::json(&gateway_decision_protocol::catalog::owner(&id))),("supported_endpoint_types",c::json(&["openai-response","openai-chat"]))]);
        reasoning_fields(&mut row,&resolved,true);apply_resolved_fields(&mut row,&resolved,true,false);common_fields(&mut row,1000,false);Some(row)
    }).collect()
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
fn merge_configured_native(rows: &mut Vec<SourceJson>, input: &CatalogInput, provider: &str) {
    let configured = configured_native_records(input, provider);
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
pub fn native_catalog(stored: Option<&SourceJson>, input: &CatalogInput) -> SourceJson {
    let mut envelope = stored
        .filter(|v| c::is_object(v))
        .cloned()
        .unwrap_or_else(|| c::object(vec![("models", c::json(&Vec::<SourceJson>::new()))]));
    let mut rows = if input.enabled("codex") {
        stored
            .and_then(|v| gateway_source::source_serialization::member(v, "models"))
            .as_ref()
            .and_then(gateway_source::source_serialization::array)
            .unwrap_or_default()
    } else {
        Vec::new()
    };
    append_native(
        &mut rows,
        unique_paid(input)
            .iter()
            .map(|r| paid_native_record(r, input)),
    );
    append_native(
        &mut rows,
        input
            .available_rows("claude_code")
            .iter()
            .map(claude_native_record),
    );
    for provider in ["deepseek", "ubiquity", "lithos", "cerebras"] {
        merge_configured_native(&mut rows, input, provider);
    }
    append_native(&mut rows, openrouter_native(input));
    c::set(
        &mut envelope,
        "models",
        c::json(&filter_whitelist(rows, input.whitelist.as_ref(), true)),
    );
    envelope
}
pub fn native_catalog_fallback(input: &CatalogInput) -> Option<SourceJson> {
    let paid = unique_paid(input);
    let mut rows = Vec::new();
    append_native(
        &mut rows,
        input
            .available_rows("claude_code")
            .iter()
            .map(claude_native_record),
    );
    append_native(&mut rows, paid.iter().map(|r| paid_native_record(r, input)));
    for provider in ["deepseek", "ubiquity", "lithos", "cerebras"] {
        merge_configured_native(&mut rows, input, provider);
    }
    append_native(&mut rows, openrouter_native(input));
    if rows.is_empty() {
        None
    } else {
        Some(c::object(vec![(
            "models",
            c::json(&filter_whitelist(rows, input.whitelist.as_ref(), true)),
        )]))
    }
}
