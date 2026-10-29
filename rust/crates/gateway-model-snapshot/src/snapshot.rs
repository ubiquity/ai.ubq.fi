//! Full uploaded snapshot normalization and evidence-preserving refresh projection.
use gateway_contracts::model::ModelId;
use gateway_model_metadata::reasoning::advertised_effort;
use gateway_source::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use serde::{Deserialize, Serialize};
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct ModelSnapshot {
    pub models: Vec<SourceJson>,
    pub source: JsString16,
    pub updated_at_ms: i64,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub client_version: Option<String>,
}
#[derive(Clone, Debug, Default)]
pub struct SnapshotOverrides {
    pub source: Option<JsString16>,
    pub client_version: Option<String>,
    pub updated_at_ms: Option<i64>,
}
pub fn parse_client_version(raw: &str) -> Option<[u64; 3]> {
    let parts: Vec<_> = raw
        .trim_matches(gateway_schema_values::scalar::js_whitespace)
        .split('.')
        .collect();
    if parts.len() != 3 {
        return None;
    }
    let mut result = [0; 3];
    for (i, part) in parts.iter().enumerate() {
        if part.is_empty() || !part.bytes().all(|b| b.is_ascii_digit()) {
            return None;
        }
        let n = part.parse::<u64>().ok()?;
        if n > 9_007_199_254_740_991 {
            return None;
        }
        result[i] = n;
    }
    Some(result)
}
pub fn compare_client_versions(left: &str, right: &str) -> Option<std::cmp::Ordering> {
    Some(parse_client_version(left)?.cmp(&parse_client_version(right)?))
}
pub fn normalize_snapshot(
    value: &SourceJson,
    overrides: &SnapshotOverrides,
    now_ms: i64,
) -> Result<Option<ModelSnapshot>, GatewayError> {
    let raw = gateway_source::source_serialization::array(value)
        .or_else(|| {
            gateway_source::source_serialization::member(value, "models")
                .as_ref()
                .and_then(gateway_source::source_serialization::array)
        })
        .or_else(|| {
            gateway_source::source_serialization::member(value, "data")
                .as_ref()
                .and_then(gateway_source::source_serialization::array)
        });
    let Some(raw) = raw else {
        return Ok(None);
    };
    let mut models = Vec::new();
    let mut seen = Vec::new();
    for item in &raw {
        if source::entries(item).is_none() && source::array(item).is_none() {
            continue;
        }
        let hidden = gateway_source::source_serialization::member(item, "visibility")
            .as_ref()
            .and_then(gateway_source::source_serialization::string)
            .is_some_and(|v| {
                gateway_schema_values::source_view::js_trim_source(&v)
                    .as_units()
                    .iter()
                    .copied()
                    .map(|u| if (65..=90).contains(&u) { u + 32 } else { u })
                    .eq("hide".encode_utf16())
            })
            && gateway_source::source_serialization::member(item, "supported_in_api")
                .is_none_or(|v| v.as_str() != "true");
        if hidden {
            continue;
        }
        let Some(slug) = gateway_schema_values::source_view::identifier(item)
            .filter(|s| !s.as_units().is_empty())
        else {
            continue;
        };
        if seen.contains(&slug) {
            continue;
        }
        seen.push(slug.clone());
        models.push(normalized_model(item, &slug)?);
    }
    if models.is_empty() {
        return Ok(None);
    }
    let source = overrides
        .source
        .as_ref()
        .filter(|s| !s.as_units().is_empty())
        .cloned()
        .or_else(|| {
            gateway_source::source_serialization::member(value, "source")
                .as_ref()
                .and_then(gateway_source::source_serialization::string)
        })
        .unwrap_or_else(|| "codex_cli".into());
    let client_version = overrides
        .client_version
        .as_ref()
        .filter(|s| !s.is_empty())
        .cloned()
        .or_else(|| {
            source::member(value, "client_version")
                .as_ref()
                .and_then(source::string)
                .and_then(|s| s.try_utf8().ok())
                .or_else(|| {
                    source::member(value, "clientVersion")
                        .as_ref()
                        .and_then(source::string)
                        .and_then(|s| s.try_utf8().ok())
                })
        });
    let updated_at_ms = overrides
        .updated_at_ms
        .or_else(|| {
            gateway_source::source_serialization::member(value, "updated_at_ms")
                .as_ref()
                .and_then(gateway_source::source_serialization::number)
                .filter(|n| n.is_finite())
                .map(|n| n.trunc() as i64)
        })
        .unwrap_or(now_ms);
    Ok(Some(ModelSnapshot {
        models,
        source,
        updated_at_ms,
        client_version,
    }))
}
impl ModelSnapshot {
    pub fn default_model(&self) -> Option<ModelId> {
        self.models.iter().find_map(|m| {
            gateway_schema_values::source_view::identifier(m)
                .map(|s| gateway_schema_values::source_view::js_trim_source(&s))
                .filter(|s| !s.as_units().is_empty())
                .map(ModelId::from_source)
        })
    }
    pub fn find(&self, id: &ModelId) -> Option<&SourceJson> {
        let target = gateway_schema_values::source_view::js_trim_source(id.as_js_string());
        self.models.iter().find(|m| {
            gateway_schema_values::source_view::identifier(m)
                .is_some_and(|s| gateway_schema_values::source_view::js_trim_source(&s) == target)
        })
    }
    pub fn unique(&self, id: &ModelId) -> Option<&SourceJson> {
        let target = gateway_schema_values::source_view::js_trim_source(id.as_js_string());
        let matches = self
            .models
            .iter()
            .filter(|m| {
                source::entries(m).is_some()
                    && gateway_schema_values::source_view::identifier(m).is_some_and(|s| {
                        gateway_schema_values::source_view::js_trim_source(&s) == target
                    })
            })
            .collect::<Vec<_>>();
        (matches.len() == 1).then(|| matches[0])
    }
    pub fn preserve_default(&self, candidate: Option<&ModelId>) -> Option<ModelId> {
        candidate
            .map(|m| {
                ModelId::from_source(gateway_schema_values::source_view::js_trim_source(
                    m.as_js_string(),
                ))
            })
            .filter(|m| !m.as_js_string().as_units().is_empty() && self.find(m).is_some())
    }
    pub fn merge_prompt_cache(
        &self,
        previous: Option<&ModelSnapshot>,
    ) -> Result<Self, GatewayError> {
        let mut next = self.clone();
        for model in &mut next.models {
            let id = gateway_schema_values::source_view::model(model);
            let prior = previous.and_then(|s| id.as_ref().and_then(|id| s.find(id)));
            let prior =
                prior.and_then(|p| gateway_source::source_serialization::member(p, "prompt_cache"));
            let current = gateway_source::source_serialization::member(model, "prompt_cache");
            if let Some(cache) = gateway_prompt_cache::metadata::merge_capabilities_source(
                prior.as_ref(),
                current.as_ref(),
            ) {
                *model = model_with_cache(model, cache)?;
            }
        }
        Ok(next)
    }
    pub fn with_prompt_cache_scope(
        &self,
        slug: &ModelId,
        provider_id: &str,
        scope: &SourceJson,
    ) -> Result<Option<Self>, GatewayError> {
        let Some(model) = self.unique(slug) else {
            return Ok(None);
        };
        let Some(scope) = gateway_prompt_cache::metadata::normalize_scope_source(scope) else {
            return Ok(None);
        };
        let cache = gateway_prompt_cache::metadata::normalize_capabilities_source(
            gateway_source::source_serialization::member(model, "prompt_cache").as_ref(),
        );
        let Some(cache) = cache else {
            return Ok(None);
        };
        let providers = gateway_source::source_serialization::member(&cache, "providers")
            .as_ref()
            .and_then(gateway_source::source_serialization::array);
        let Some(mut providers) = providers else {
            return Ok(None);
        };
        let provider = providers.iter_mut().find(|p| {
            gateway_source::source_serialization::member(p, "id")
                .as_ref()
                .and_then(gateway_source::source_serialization::string)
                .is_some_and(|id| gateway_schema_values::source_view::matches(&id, provider_id))
        });
        let Some(provider) = provider else {
            return Ok(None);
        };
        *provider = provider_with_scope(provider, scope)?;
        let cache = cache_with_providers(&cache, SourceJson::from_serializable(&providers)?)?;
        let mut next = self.clone();
        let target = gateway_schema_values::source_view::js_trim_source(slug.as_js_string());
        for record in &mut next.models {
            if gateway_schema_values::source_view::identifier(record)
                .is_some_and(|s| gateway_schema_values::source_view::js_trim_source(&s) == target)
            {
                *record = model_with_cache(record, cache.clone())?;
            }
        }
        Ok(Some(next))
    }
    pub fn scope_experiment_eligible(&self, slug: &ModelId) -> bool {
        let cache = self.unique(slug).and_then(|m| {
            gateway_prompt_cache::metadata::normalize_capabilities_source(
                gateway_source::source_serialization::member(m, "prompt_cache").as_ref(),
            )
        });
        let controls = cache
            .as_ref()
            .and_then(|c| gateway_source::source_serialization::member(c, "providers"))
            .as_ref()
            .and_then(gateway_source::source_serialization::array)
            .and_then(|ps| {
                ps.into_iter().find(|p| {
                    gateway_source::source_serialization::member(p, "id")
                        .as_ref()
                        .and_then(gateway_source::source_serialization::string)
                        .is_some_and(|id| {
                            gateway_schema_values::source_view::matches(
                                &id,
                                gateway_prompt_cache::profile::CODEX_PROVIDER,
                            )
                        })
                })
            })
            .and_then(|p| gateway_source::source_serialization::member(&p, "controls"));
        controls.is_some_and(|controls| {
            gateway_source::source_serialization::member(&controls, "key")
                .is_some_and(|v| v.as_str() == "true")
                && gateway_source::source_serialization::member(&controls, "implicit")
                    .is_none_or(|v| v.as_str() != "false")
                && gateway_source::source_serialization::member(&controls, "expected_usage_fields")
                    .as_ref()
                    .and_then(gateway_source::source_serialization::array)
                    .is_some_and(|fields| {
                        ["cached_tokens", "cache_write_tokens"].iter().all(|key| {
                            fields.iter().any(|v| {
                                gateway_source::source_serialization::string(v).is_some_and(|s| {
                                    gateway_schema_values::source_view::matches(&s, key)
                                })
                            })
                        })
                    })
        })
    }
}
/// Pool order and first row are authoritative; unknown top-level metadata stays verbatim.
pub type AccountCatalogEvidence = Vec<(String, Vec<ModelId>)>;
pub type AccountCatalogUnion = (SourceJson, AccountCatalogEvidence);
pub fn union_account_catalogs(
    catalogs: &[(String, SourceJson)],
) -> Result<Option<AccountCatalogUnion>, GatewayError> {
    let mut envelope = None;
    let mut models = Vec::new();
    let mut seen = Vec::new();
    let mut accounts = Vec::new();
    for (account, catalog) in catalogs {
        let Some(rows) = gateway_source::source_serialization::member(catalog, "models")
            .as_ref()
            .and_then(gateway_source::source_serialization::array)
        else {
            continue;
        };
        if source::entries(catalog).is_none() {
            continue;
        }
        if envelope.is_none() {
            envelope = Some(catalog.clone());
        }
        let mut slugs = Vec::new();
        for row in rows {
            let Some(slug) = gateway_schema_values::source_view::identifier(&row)
                .map(|s| gateway_schema_values::source_view::js_trim_source(&s))
                .filter(|s| !s.as_units().is_empty())
                .map(ModelId::from_source)
            else {
                continue;
            };
            gateway_schema_values::source_view::push_unique(&mut slugs, slug.clone());
            if !seen.contains(&slug) {
                models.push(row);
                seen.push(slug);
            }
        }
        accounts.push((account.clone(), slugs));
    }
    let Some(envelope) = envelope else {
        return Ok(None);
    };
    let envelope = union_with_models(&envelope, SourceJson::from_serializable(&models)?)?;
    Ok(Some((envelope, accounts)))
}

#[derive(Serialize)]
struct NormalizedModel {
    slug: SourceJson,
    #[serde(skip_serializing_if = "Option::is_none")]
    display_name: Option<SourceJson>,
    #[serde(skip_serializing_if = "Option::is_none")]
    description: Option<SourceJson>,
    #[serde(skip_serializing_if = "Option::is_none")]
    visibility: Option<SourceJson>,
    #[serde(skip_serializing_if = "Option::is_none")]
    supported_in_api: Option<SourceJson>,
    #[serde(skip_serializing_if = "Option::is_none")]
    context_window: Option<SourceJson>,
    #[serde(skip_serializing_if = "Option::is_none")]
    max_context_window: Option<SourceJson>,
    #[serde(skip_serializing_if = "Option::is_none")]
    auto_compact_token_limit: Option<SourceJson>,
    #[serde(skip_serializing_if = "Option::is_none")]
    effective_context_window_percent: Option<SourceJson>,
    #[serde(skip_serializing_if = "Option::is_none")]
    default_reasoning_level: Option<SourceJson>,
    #[serde(skip_serializing_if = "Option::is_none")]
    supported_reasoning_levels: Option<SourceJson>,
    #[serde(skip_serializing_if = "Option::is_none")]
    reasoning_effort_wire_map: Option<SourceJson>,
    #[serde(skip_serializing_if = "Option::is_none")]
    prompt_cache: Option<SourceJson>,
}
fn normalized_model(item: &SourceJson, slug: &JsString16) -> Result<SourceJson, GatewayError> {
    let mut model = NormalizedModel {
        slug: SourceJson::from_serializable(slug)?,
        display_name: None,
        description: None,
        visibility: None,
        supported_in_api: None,
        context_window: None,
        max_context_window: None,
        auto_compact_token_limit: None,
        effective_context_window_percent: None,
        default_reasoning_level: None,
        supported_reasoning_levels: None,
        reasoning_effort_wire_map: None,
        prompt_cache: None,
    };
    if let Some(name) = ["display_name", "displayName", "name"]
        .iter()
        .find_map(|key| source::member(item, key).as_ref().and_then(source::string))
        .filter(|s| !s.as_units().is_empty())
    {
        model.display_name = Some(SourceJson::from_serializable(&name)?);
    }
    for (key, target) in [
        ("description", &mut model.description),
        ("visibility", &mut model.visibility),
    ] {
        if let Some(value) = source::member(item, key)
            .as_ref()
            .and_then(source::string)
            .filter(|s| !s.as_units().is_empty())
        {
            *target = Some(SourceJson::from_serializable(&value)?);
        }
    }
    model.supported_in_api =
        source::member(item, "supported_in_api").filter(|v| matches!(v.as_str(), "true" | "false"));
    normalized_context_fields(item, &mut model)?;
    normalized_reasoning_fields(item, &mut model)?;
    model.prompt_cache = gateway_prompt_cache::metadata::normalize_capabilities_source(
        source::member(item, "prompt_cache").as_ref(),
    );
    SourceJson::from_serializable(&model)
}
fn normalized_context_fields(
    item: &SourceJson,
    model: &mut NormalizedModel,
) -> Result<(), GatewayError> {
    for (key, target) in [
        ("context_window", &mut model.context_window),
        ("max_context_window", &mut model.max_context_window),
        (
            "auto_compact_token_limit",
            &mut model.auto_compact_token_limit,
        ),
        (
            "effective_context_window_percent",
            &mut model.effective_context_window_percent,
        ),
    ] {
        if let Some(value) = source::member(item, key) {
            if value.as_str() == "null" {
                *target = Some(value);
            } else if let Some(n) = source::number(&value).filter(|n| n.is_finite() && *n >= 0.0) {
                let n = n.trunc();
                *target = Some(if n <= gateway_schema_values::scalar::MAX_SAFE_INTEGER {
                    SourceJson::from_serializable(&(n as i64))?
                } else {
                    SourceJson::from_serializable(&n)?
                });
            }
        }
    }
    Ok(())
}
fn normalized_reasoning_fields(
    item: &SourceJson,
    model: &mut NormalizedModel,
) -> Result<(), GatewayError> {
    if let Some(value) = source::member(item, "default_reasoning_level") {
        if value.as_str() == "null" {
            model.default_reasoning_level = Some(SourceJson::from_serializable("none")?);
        } else if let Some(level) = gateway_schema_values::source_view::normalized(Some(&value)) {
            model.default_reasoning_level = Some(SourceJson::from_serializable(&level)?);
        }
    }
    if let Some(raw_levels) = source::member(item, "supported_reasoning_levels")
        .as_ref()
        .and_then(source::array)
    {
        let levels = raw_levels
            .iter()
            .filter_map(advertised_effort)
            .collect::<Vec<_>>();
        if !levels.is_empty() {
            model.supported_reasoning_levels = Some(SourceJson::from_serializable(&levels)?);
        }
        let mut wire = Vec::new();
        for level in raw_levels {
            if let Some(effort) = advertised_effort(&level) {
                let upstream = if gateway_schema_values::source_view::matches(&effort, "ultra") {
                    Some("max".into())
                } else {
                    gateway_schema_values::source_view::normalized(
                        source::member(&level, "wire_effort").as_ref(),
                    )
                };
                if let Some(upstream) = upstream.filter(|s| s != &effort) {
                    let value = SourceJson::from_serializable(&upstream)?;
                    if let Some((_, old)) = wire.iter_mut().find(|(key, _)| key == &effort) {
                        *old = value;
                    } else {
                        wire.push((effort, value));
                    }
                }
            }
        }
        if !wire.is_empty() {
            model.reasoning_effort_wire_map = Some(source::object_keys(wire)?);
        }
    }
    Ok(())
}
fn model_with_cache(record: &SourceJson, value: SourceJson) -> Result<SourceJson, GatewayError> {
    let mut fields = source::entries(record).ok_or_else(|| {
        GatewayError::unavailable(
            "Invalid source JSON serialization.",
            "source_json_serialization_invalid",
        )
    })?;
    let key = JsString16::from_utf8("prompt_cache");
    if let Some((_, prior)) = fields.iter_mut().find(|(name, _)| name == &key) {
        *prior = value;
    } else {
        fields.push((key, value));
    }
    source::object_keys(fields)
}
fn provider_with_scope(record: &SourceJson, value: SourceJson) -> Result<SourceJson, GatewayError> {
    let mut fields = source::entries(record).ok_or_else(|| {
        GatewayError::unavailable(
            "Invalid source JSON serialization.",
            "source_json_serialization_invalid",
        )
    })?;
    let key = JsString16::from_utf8("scope");
    if let Some((_, prior)) = fields.iter_mut().find(|(name, _)| name == &key) {
        *prior = value;
    } else {
        fields.push((key, value));
    }
    source::object_keys(fields)
}
fn cache_with_providers(
    record: &SourceJson,
    value: SourceJson,
) -> Result<SourceJson, GatewayError> {
    let mut fields = source::entries(record).ok_or_else(|| {
        GatewayError::unavailable(
            "Invalid source JSON serialization.",
            "source_json_serialization_invalid",
        )
    })?;
    let key = JsString16::from_utf8("providers");
    if let Some((_, prior)) = fields.iter_mut().find(|(name, _)| name == &key) {
        *prior = value;
    } else {
        fields.push((key, value));
    }
    source::object_keys(fields)
}
fn union_with_models(record: &SourceJson, value: SourceJson) -> Result<SourceJson, GatewayError> {
    let mut fields = source::entries(record).ok_or_else(|| {
        GatewayError::unavailable(
            "Invalid source JSON serialization.",
            "source_json_serialization_invalid",
        )
    })?;
    let key = JsString16::from_utf8("models");
    if let Some((_, prior)) = fields.iter_mut().find(|(name, _)| name == &key) {
        *prior = value;
    } else {
        fields.push((key, value));
    }
    source::object_keys(fields)
}
