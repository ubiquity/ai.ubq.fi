//! Catalog assembly consumes observations; network, authentication and refresh are adapter-owned.
use gateway_contracts::model::ModelId;
use gateway_local_refusal::source_error::SourceRequestError;
use gateway_model_metadata::{
    metadata::{MetadataHint, MetadataSources, ResolvedMetadata, resolve_metadata},
    reasoning::ModelReasoning,
};
use gateway_model_selection::{
    aliases::FLASH_ORDER,
    selection::{ModelWhitelist, ProviderSelection},
};
use gateway_model_snapshot::snapshot::ModelSnapshot;
use gateway_source::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct ProviderCatalog {
    pub models: Vec<SourceJson>,
    pub updated_at_ms: Option<i64>,
    pub configured: bool,
    pub available: bool,
    pub stale: bool,
}
#[derive(Clone, Debug, Default)]
pub struct CatalogInput {
    pub snapshot: Option<ModelSnapshot>,
    pub full_snapshot: Option<ModelSnapshot>,
    pub providers: BTreeMap<String, ProviderCatalog>,
    pub enrichment: BTreeMap<ModelId, MetadataHint>,
    pub selection: Option<ProviderSelection>,
    pub whitelist: Option<ModelWhitelist>,
}
impl CatalogInput {
    pub fn enabled(&self, provider: &str) -> bool {
        ProviderSelection::enabled(self.selection.as_ref(), provider)
    }
    pub fn configured(&self, provider: &str) -> bool {
        self.providers.get(provider).is_some_and(|p| p.configured)
    }
    pub fn rows(&self, provider: &str) -> &[SourceJson] {
        self.providers
            .get(provider)
            .map(|p| p.models.as_slice())
            .unwrap_or(&[])
    }
    pub fn available_rows(&self, provider: &str) -> &[SourceJson] {
        if self.enabled(provider) {
            self.rows(provider)
        } else {
            &[]
        }
    }
    pub fn codex(&self) -> Option<&ModelSnapshot> {
        self.full_snapshot.as_ref().or(self.snapshot.as_ref())
    }
    pub fn metadata(
        &self,
        id: &ModelId,
        codex: Option<&SourceJson>,
        provider: Option<MetadataHint>,
        subscription: bool,
    ) -> ResolvedMetadata {
        resolve_metadata(&MetadataSources {
            codex: codex.map(MetadataHint::codex),
            subscription: subscription.then(MetadataHint::subscription),
            provider,
            openrouter: self.enrichment.get(id).cloned(),
        })
    }
    pub fn synthetic_configured(&self) -> bool {
        FLASH_ORDER.iter().any(|p| self.configured(p))
    }
    pub fn synthetic_enabled(&self) -> bool {
        self.enabled("ubiquity") && self.synthetic_configured()
    }
}
pub fn endpoint_paths(model: &SourceJson) -> Vec<String> {
    let mut paths = Vec::new();
    if endpoint_supports(model, "openai-response") {
        paths.push("/v1/responses".into());
    }
    if endpoint_supports(model, "openai") {
        paths.push("/v1/chat/completions".into());
    }
    paths
}
pub fn endpoint_supports(model: &SourceJson, endpoint: &str) -> bool {
    gateway_source::source_serialization::member(model, "supported_endpoint_types")
        .as_ref()
        .and_then(gateway_source::source_serialization::array)
        .is_some_and(|types| {
            types.iter().any(|v| {
                gateway_source::source_serialization::string(v)
                    .is_some_and(|s| gateway_schema_values::source_view::matches(&s, endpoint))
            })
        })
}
#[derive(Serialize)]
#[serde(untagged)]
enum ModelCreated {
    Integer(i64),
    Number(f64),
}
#[derive(Serialize)]
struct ModelListRow {
    id: JsString16,
    object: &'static str,
    created: ModelCreated,
    owned_by: JsString16,
}
#[derive(Serialize)]
struct ModelList {
    object: &'static str,
    data: Vec<SourceJson>,
}
#[derive(Serialize)]
struct RequestRecord {
    slug: SourceJson,
    #[serde(skip_serializing_if = "Option::is_none")]
    supported_reasoning_levels: Option<Vec<JsString16>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    default_reasoning_level: Option<JsString16>,
}
pub fn model_list_row(
    record: &SourceJson,
    fallback_created: i64,
) -> Result<Option<SourceJson>, GatewayError> {
    let Some(id) = ["id", "slug", "model", "name"].iter().find_map(|key| {
        source::member(record, key)
            .as_ref()
            .and_then(source::string)
    }) else {
        return Ok(None);
    };
    if id.as_units().is_empty() {
        return Ok(None);
    }
    let created = source::member(record, "created")
        .as_ref()
        .and_then(source::number)
        .map(f64::trunc)
        .filter(|n| n.is_finite() && *n >= 0.0)
        .map(|n| {
            if n <= gateway_schema_values::scalar::MAX_SAFE_INTEGER {
                ModelCreated::Integer(n as i64)
            } else {
                ModelCreated::Number(n)
            }
        })
        .unwrap_or(ModelCreated::Integer(fallback_created));
    let owner = source::member(record, "owned_by")
        .as_ref()
        .and_then(source::string)
        .unwrap_or_else(|| "openai".into());
    Ok(Some(SourceJson::from_serializable(&ModelListRow {
        id,
        object: "model",
        created,
        owned_by: owner,
    })?))
}
pub fn normalize_model_list(payload: &SourceJson) -> Result<Option<SourceJson>, GatewayError> {
    if !payload.as_str().starts_with('{') && !payload.as_str().starts_with('[') {
        return Ok(None);
    }
    let fallback = source::member(payload, "updated_at_ms")
        .as_ref()
        .and_then(source::number)
        .filter(|n| n.is_finite())
        .map(|n| (n / 1000.0).trunc().max(0.0) as i64)
        .unwrap_or(0);
    let Some(rows) = source::member(payload, "data")
        .as_ref()
        .and_then(source::array)
        .or_else(|| {
            source::member(payload, "models")
                .as_ref()
                .and_then(source::array)
        })
    else {
        return Ok(None);
    };
    let mut normalized = Vec::new();
    for row in rows {
        if let Some(row) = model_list_row(&row, fallback)? {
            normalized.push(row);
        }
    }
    Ok(Some(SourceJson::from_serializable(&ModelList {
        object: "list",
        data: normalized,
    })?))
}
fn field_equal(a: Option<SourceJson>, b: Option<SourceJson>) -> bool {
    match (a, b) {
        (None, None) => true,
        (Some(a), Some(b)) => source::canonical(&a).ok() == source::canonical(&b).ok(),
        _ => false,
    }
}
pub fn append_unique(
    rows: &mut Vec<SourceJson>,
    new_rows: impl IntoIterator<Item = SourceJson>,
    key: &str,
) {
    for row in new_rows {
        if !rows.iter().any(|r| {
            field_equal(
                gateway_source::source_serialization::member(r, key),
                gateway_source::source_serialization::member(&row, key),
            )
        }) {
            rows.push(row);
        }
    }
}
pub fn filter_whitelist(
    rows: Vec<SourceJson>,
    whitelist: Option<&ModelWhitelist>,
    native: bool,
) -> Vec<SourceJson> {
    rows.into_iter()
        .filter(|row| {
            let id = if native {
                gateway_schema_values::source_view::identifier(row)
            } else {
                gateway_source::source_serialization::member(row, "id")
                    .as_ref()
                    .and_then(gateway_source::source_serialization::string)
            };
            id.is_some_and(|id| {
                let id = if native {
                    gateway_schema_values::source_view::js_trim_source(&id)
                } else {
                    id
                };
                ModelWhitelist::permits(whitelist, &ModelId::from_source(id))
            })
        })
        .collect()
}
pub fn claude_levels(model: &SourceJson) -> Vec<JsString16> {
    let mut levels = Vec::new();
    if gateway_source::source_serialization::member(model, "supports_disabled_thinking")
        .is_some_and(|v| v.as_str() == "true")
    {
        levels.push("none".into());
    }
    if let Some(raw) =
        gateway_source::source_serialization::member(model, "supported_reasoning_levels")
            .as_ref()
            .and_then(gateway_source::source_serialization::array)
    {
        levels.extend(
            raw.iter()
                .filter_map(gateway_source::source_serialization::string),
        );
    }
    levels
}
#[derive(Clone, Debug, PartialEq)]
pub struct RequestModelMetadata {
    pub snapshot: Option<ModelSnapshot>,
    pub record: Option<SourceJson>,
    pub reasoning: ModelReasoning,
    pub supported_endpoints: Option<Vec<String>>,
}
impl RequestModelMetadata {
    pub fn prompt_cache(&self) -> Option<SourceJson> {
        self.record
            .as_ref()
            .and_then(|record| gateway_source::source_serialization::member(record, "prompt_cache"))
    }
}
fn model_matches(row: &SourceJson, model: &ModelId) -> bool {
    source::member(row, "id")
        .as_ref()
        .and_then(source::string)
        .is_some_and(|id| &id == model.as_js_string())
}
pub fn request_model_metadata(
    input: &CatalogInput,
    model: &ModelId,
    responses: bool,
    now_ms: i64,
) -> Result<RequestModelMetadata, GatewayError> {
    if model.matches_scalar("glm-5.2") {
        let record = SourceJson::from_serializable(&RequestRecord {
            slug: SourceJson::from_serializable("glm-5.2")?,
            supported_reasoning_levels: None,
            default_reasoning_level: None,
        })?;
        return Ok(RequestModelMetadata {
            snapshot: None,
            reasoning: ModelReasoning::from_record(Some(&record)),
            record: Some(record),
            supported_endpoints: Some(vec!["openai".into(), "openai-response".into()]),
        });
    }
    let snapshot = input.snapshot.clone();
    if let Some(record) = snapshot.as_ref().and_then(|s| s.find(model)).cloned() {
        return Ok(RequestModelMetadata {
            snapshot,
            reasoning: ModelReasoning::from_record(Some(&record)),
            record: Some(record),
            supported_endpoints: None,
        });
    }
    if input.enabled("claude_code")
        && let Some(row) = input
            .rows("claude_code")
            .iter()
            .find(|row| model_matches(row, model))
    {
        let slug = source::member(row, "id").ok_or_else(|| {
            GatewayError::unavailable(
                "Matched model id is unavailable",
                "source_json_serialization_invalid",
            )
        })?;
        let default = source::member(row, "supports_disabled_thinking")
            .is_some_and(|v| v.as_str() == "true")
            .then(|| "none".into());
        let record = SourceJson::from_serializable(&RequestRecord {
            slug,
            supported_reasoning_levels: Some(claude_levels(row)),
            default_reasoning_level: default,
        })?;
        return Ok(RequestModelMetadata {
            snapshot,
            reasoning: ModelReasoning::from_record(Some(&record)),
            record: Some(record),
            supported_endpoints: Some(vec!["openai".into(), "openai-response".into()]),
        });
    }
    paid_request_metadata(input, model, responses, now_ms, snapshot)
}
fn paid_request_metadata(
    input: &CatalogInput,
    model: &ModelId,
    responses: bool,
    now_ms: i64,
    snapshot: Option<ModelSnapshot>,
) -> Result<RequestModelMetadata, GatewayError> {
    let endpoint = if responses {
        "openai-response"
    } else {
        "openai"
    };
    let candidates = ["openlux", "surplus"]
        .iter()
        .filter_map(|provider| {
            input
                .rows(provider)
                .iter()
                .find(|row| model_matches(row, model))
                .map(|r| (*provider, r))
        })
        .collect::<Vec<_>>();
    if let Some((provider, paid)) = candidates
        .iter()
        .find(|(_, m)| endpoint_supports(m, endpoint))
        .or_else(|| candidates.first())
    {
        let record = SourceJson::from_serializable(&RequestRecord {
            slug: model.to_source_json()?,
            supported_reasoning_levels: Some(vec!["none".into()]),
            default_reasoning_level: Some("none".into()),
        })?;
        let snapshot = snapshot.or_else(|| {
            Some(ModelSnapshot {
                models: Vec::new(),
                source: if *provider == "openlux" {
                    "metered"
                } else {
                    "surplus"
                }
                .into(),
                updated_at_ms: input
                    .providers
                    .get(*provider)
                    .and_then(|p| p.updated_at_ms)
                    .unwrap_or(now_ms),
                client_version: None,
            })
        });
        // Only finite endpoint membership is consumed here; original source rows remain authoritative.
        let endpoints =
            gateway_source::source_serialization::member(paid, "supported_endpoint_types")
                .as_ref()
                .and_then(gateway_source::source_serialization::array)
                .map(|items| {
                    items
                        .iter()
                        .filter_map(|item| {
                            gateway_source::source_serialization::string(item)
                                .and_then(|s| s.try_utf8().ok())
                        })
                        .collect()
                });
        return Ok(RequestModelMetadata {
            snapshot,
            reasoning: ModelReasoning::from_record(Some(&record)),
            record: Some(record),
            supported_endpoints: endpoints,
        });
    }
    Ok(RequestModelMetadata {
        snapshot,
        record: None,
        reasoning: ModelReasoning::default(),
        supported_endpoints: None,
    })
}
pub fn validate_model_available(
    model: &ModelId,
    responses: bool,
    metadata: &RequestModelMetadata,
) -> Result<(), SourceRequestError> {
    let route = if responses {
        "responses"
    } else {
        "chat.completions"
    };
    let endpoint = if responses {
        "openai-response"
    } else {
        "openai"
    };
    let suffix = if metadata
        .supported_endpoints
        .as_ref()
        .is_some_and(|e| !e.iter().any(|e| e == endpoint))
    {
        Some(format!(
            "' does not support {route}. Use /v1/models for supported models."
        ))
    } else if metadata
        .snapshot
        .as_ref()
        .is_some_and(|s| !s.models.is_empty())
        && metadata.record.is_none()
        && !gateway_schema_values::source_view::matches(
            &gateway_schema_values::source_view::js_trim_source(model.as_js_string()),
            "gpt-reserve",
        )
    {
        Some("' does not exist or is not available through this gateway. Use /v1/models for supported models.".into())
    } else {
        None
    };
    if let Some(suffix) = suffix {
        let prefix = JsString16::from_utf8("The model '");
        let suffix = JsString16::from_utf8(&suffix);
        Err(SourceRequestError::new(
            404,
            gateway_schema_values::source_view::concat(&[&prefix, model.as_js_string(), &suffix]),
            "invalid_request_error",
            Some("model_not_found"),
            Some(Some("model".into())),
        ))
    } else {
        Ok(())
    }
}
