//! Full uploaded snapshot normalization and evidence-preserving refresh projection.
use crate::{model_control as c, reasoning::advertised_effort};
use gateway_contracts::model::ModelId;
use gateway_source::{js_string::JsString16, source_json::SourceJson};
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
) -> Option<ModelSnapshot> {
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
        })?;
    let mut models = Vec::new();
    let mut seen = Vec::new();
    for item in &raw {
        if !c::is_record(item) {
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
        let mut model = c::object(vec![("slug", c::json(&slug))]);
        if let Some(name) = ["display_name", "displayName", "name"]
            .iter()
            .find_map(|key| {
                gateway_source::source_serialization::member(item, key)
                    .as_ref()
                    .and_then(gateway_source::source_serialization::string)
            })
            .filter(|s| !s.as_units().is_empty())
        {
            c::set(&mut model, "display_name", c::json(&name));
        }
        for key in ["description", "visibility"] {
            if let Some(value) = gateway_source::source_serialization::member(item, key)
                .as_ref()
                .and_then(gateway_source::source_serialization::string)
                .filter(|s| !s.as_units().is_empty())
            {
                c::set(&mut model, key, c::json(&value));
            }
        }
        if let Some(value) = gateway_source::source_serialization::member(item, "supported_in_api")
            .filter(|v| matches!(v.as_str(), "true" | "false"))
        {
            c::set(&mut model, "supported_in_api", value);
        }
        for key in [
            "context_window",
            "max_context_window",
            "auto_compact_token_limit",
            "effective_context_window_percent",
        ] {
            if let Some(value) = gateway_source::source_serialization::member(item, key) {
                if value.as_str() == "null" {
                    c::set(&mut model, key, value);
                } else if let Some(n) = gateway_source::source_serialization::number(&value)
                    .filter(|n| n.is_finite() && *n >= 0.0)
                {
                    let n = n.trunc();
                    c::set(
                        &mut model,
                        key,
                        if n <= gateway_schema_values::scalar::MAX_SAFE_INTEGER {
                            c::json(&(n as i64))
                        } else {
                            c::json(&n)
                        },
                    );
                }
            }
        }
        if let Some(v) =
            gateway_source::source_serialization::member(item, "default_reasoning_level")
        {
            if v.as_str() == "null" {
                c::set(&mut model, "default_reasoning_level", c::json("none"));
            } else if let Some(level) = gateway_schema_values::source_view::normalized(Some(&v)) {
                c::set(&mut model, "default_reasoning_level", c::json(&level));
            }
        }
        if let Some(raw_levels) =
            gateway_source::source_serialization::member(item, "supported_reasoning_levels")
                .as_ref()
                .and_then(gateway_source::source_serialization::array)
        {
            let levels = raw_levels
                .iter()
                .filter_map(advertised_effort)
                .collect::<Vec<_>>();
            if !levels.is_empty() {
                c::set(&mut model, "supported_reasoning_levels", c::json(&levels));
            }
            let mut wire = Vec::new();
            for level in raw_levels {
                if let Some(effort) = advertised_effort(&level) {
                    let upstream = if gateway_schema_values::source_view::matches(&effort, "ultra")
                    {
                        Some("max".into())
                    } else {
                        gateway_schema_values::source_view::normalized(
                            gateway_source::source_serialization::member(&level, "wire_effort")
                                .as_ref(),
                        )
                    };
                    if let Some(upstream) = upstream.filter(|s| s != &effort) {
                        let value = c::json(&upstream);
                        if let Some((_, old)) = wire.iter_mut().find(|(key, _)| key == &effort) {
                            *old = value;
                        } else {
                            wire.push((effort, value));
                        }
                    }
                }
            }
            if !wire.is_empty() {
                c::set(
                    &mut model,
                    "reasoning_effort_wire_map",
                    gateway_source::source_serialization::object_keys(wire)
                        .expect("source wire map"),
                );
            }
        }
        if let Some(cache) = gateway_prompt_cache::metadata::normalize_capabilities_source(
            gateway_source::source_serialization::member(item, "prompt_cache").as_ref(),
        ) {
            c::set(&mut model, "prompt_cache", cache);
        }
        models.push(model);
    }
    if models.is_empty() {
        return None;
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
            c::scalar(
                gateway_source::source_serialization::member(value, "client_version").as_ref(),
            )
            .or_else(|| {
                c::scalar(
                    gateway_source::source_serialization::member(value, "clientVersion").as_ref(),
                )
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
    Some(ModelSnapshot {
        models,
        source,
        updated_at_ms,
        client_version,
    })
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
                c::is_object(m)
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
    pub fn merge_prompt_cache(&self, previous: Option<&ModelSnapshot>) -> Self {
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
                c::set(model, "prompt_cache", cache);
            }
        }
        next
    }
    pub fn with_prompt_cache_scope(
        &self,
        slug: &ModelId,
        provider_id: &str,
        scope: &SourceJson,
    ) -> Option<Self> {
        let model = self.unique(slug)?;
        let scope = gateway_prompt_cache::metadata::normalize_scope_source(scope)?;
        let mut cache = gateway_prompt_cache::metadata::normalize_capabilities_source(
            gateway_source::source_serialization::member(model, "prompt_cache").as_ref(),
        )?;
        let mut providers = gateway_source::source_serialization::member(&cache, "providers")
            .as_ref()
            .and_then(gateway_source::source_serialization::array)?;
        let provider = providers.iter_mut().find(|p| {
            gateway_source::source_serialization::member(p, "id")
                .as_ref()
                .and_then(gateway_source::source_serialization::string)
                .is_some_and(|id| gateway_schema_values::source_view::matches(&id, provider_id))
        })?;
        c::set(provider, "scope", scope);
        c::set(&mut cache, "providers", c::json(&providers));
        let mut next = self.clone();
        let target = gateway_schema_values::source_view::js_trim_source(slug.as_js_string());
        for record in &mut next.models {
            if gateway_schema_values::source_view::identifier(record)
                .is_some_and(|s| gateway_schema_values::source_view::js_trim_source(&s) == target)
            {
                c::set(record, "prompt_cache", cache.clone());
            }
        }
        Some(next)
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
pub fn union_account_catalogs(catalogs: &[(String, SourceJson)]) -> Option<AccountCatalogUnion> {
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
        if !c::is_object(catalog) {
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
    let mut envelope = envelope?;
    c::set(&mut envelope, "models", c::json(&models));
    Some((envelope, accounts))
}
