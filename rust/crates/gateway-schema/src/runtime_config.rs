//! Small runtime catalog projection; source model/effort controls never pass through Value.
use gateway_contracts::model::ModelId;
use gateway_local_refusal::source_error::SourceRequestError;
use gateway_model_metadata::reasoning::ModelReasoning;
use gateway_model_snapshot::snapshot::ModelSnapshot;
use gateway_prompt_cache::metadata::compact_capabilities_source;
use gateway_source::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use serde::Serialize;

fn error(message: impl Into<String>) -> GatewayError {
    GatewayError::new(503, message, Some("runtime_config_invalid"))
}
enum RuntimeConfigFailure {
    Invalid(SourceRequestError),
    Encoding(GatewayError),
}
impl From<GatewayError> for RuntimeConfigFailure {
    fn from(error: GatewayError) -> Self {
        Self::Encoding(error)
    }
}
impl RuntimeConfigFailure {
    fn request(self) -> SourceRequestError {
        match self {
            Self::Invalid(error) => error,
            Self::Encoding(error) => SourceRequestError::from_gateway(error),
        }
    }
}
fn invalid(message: impl Into<String>) -> RuntimeConfigFailure {
    RuntimeConfigFailure::Invalid(SourceRequestError::from_gateway(error(message)))
}
pub fn default_model(value: &SourceJson) -> Option<ModelId> {
    gateway_schema_values::source_view::normalized(source::member(value, "default_model").as_ref())
        .map(ModelId::from_source)
}
pub fn default_reasoning_effort(value: &SourceJson) -> Option<JsString16> {
    gateway_schema_values::source_view::normalized(
        source::member(value, "default_reasoning_effort").as_ref(),
    )
}
#[derive(Serialize)]
struct CompactModel {
    slug: SourceJson,
    #[serde(skip_serializing_if = "Option::is_none")]
    default_reasoning_level: Option<SourceJson>,
    supported_reasoning_levels: SourceJson,
    #[serde(skip_serializing_if = "Option::is_none")]
    reasoning_effort_wire_map: Option<SourceJson>,
    #[serde(skip_serializing_if = "Option::is_none")]
    prompt_cache: Option<SourceJson>,
}
#[derive(Serialize)]
struct CompactCatalog<'a> {
    models: Vec<SourceJson>,
    source: JsString16,
    updated_at_ms: i64,
    #[serde(skip_serializing_if = "Option::is_none")]
    client_version: Option<&'a str>,
}
struct CompactProjection {
    document: SourceJson,
    slugs: Vec<JsString16>,
}
fn compact_projection(snapshot: &ModelSnapshot) -> Result<CompactProjection, RuntimeConfigFailure> {
    let mut models = Vec::new();
    let mut seen = Vec::new();
    for record in &snapshot.models {
        let Some(slug) = gateway_schema_values::source_view::identifier(record)
            .map(|s| gateway_schema_values::source_view::js_trim_source(&s))
            .filter(|s| !s.as_units().is_empty())
        else {
            continue;
        };
        if seen.contains(&slug) {
            continue;
        }
        seen.push(slug.clone());
        let reasoning = ModelReasoning::from_record(Some(record));
        let slug = SourceJson::from_serializable(&slug)?;
        let default = reasoning
            .default_level
            .as_ref()
            .map(SourceJson::from_serializable)
            .transpose()?;
        let levels = SourceJson::from_serializable(&reasoning.levels)?;
        let mut wires = reasoning.wire_efforts.to_source_json()?;
        if reasoning
            .levels
            .iter()
            .any(|s| gateway_schema_values::source_view::matches(s, "ultra"))
        {
            wires = wire_with_ultra(&wires)?;
        }
        let wires = source::entries(&wires)
            .is_some_and(|w| !w.is_empty())
            .then_some(wires);
        let cache = compact_capabilities_source(source::member(record, "prompt_cache").as_ref());
        models.push(SourceJson::from_serializable(&CompactModel {
            slug,
            default_reasoning_level: default,
            supported_reasoning_levels: levels,
            reasoning_effort_wire_map: wires,
            prompt_cache: cache,
        })?);
    }
    if models.is_empty() {
        return Err(invalid(
            "runtime config requires a non-empty Codex model catalog",
        ));
    }
    let source_label = gateway_schema_values::source_view::js_trim_source(&snapshot.source);
    if source_label.as_units().is_empty() || snapshot.updated_at_ms <= 0 {
        return Err(invalid(
            "runtime config requires valid Codex catalog metadata",
        ));
    }
    let version = snapshot
        .client_version
        .as_deref()
        .map(gateway_schema_values::scalar::js_trim)
        .filter(|s| !s.is_empty());
    Ok(CompactProjection {
        document: SourceJson::from_serializable(&CompactCatalog {
            models,
            source: source_label,
            updated_at_ms: snapshot.updated_at_ms,
            client_version: version,
        })?,
        slugs: seen,
    })
}
fn wire_with_ultra(wires: &SourceJson) -> Result<SourceJson, GatewayError> {
    let mut fields = source::entries(wires).ok_or_else(|| {
        GatewayError::unavailable(
            "Invalid source JSON serialization.",
            "source_json_serialization_invalid",
        )
    })?;
    let key = JsString16::from_utf8("ultra");
    let next = SourceJson::from_serializable("max")?;
    if let Some((_, old)) = fields.iter_mut().find(|(name, _)| name == &key) {
        *old = next;
    } else {
        fields.push((key, next));
    }
    source::object_keys(fields)
}
pub fn compact_snapshot(snapshot: &ModelSnapshot) -> Result<SourceJson, GatewayError> {
    compact_projection(snapshot)
        .map(|projection| projection.document)
        .map_err(|failure| match failure {
            RuntimeConfigFailure::Invalid(error) => error.into_scalar(),
            RuntimeConfigFailure::Encoding(error) => error,
        })
}
#[derive(Serialize)]
struct RuntimeConfigDocument {
    version: i32,
    default_model: JsString16,
    default_reasoning_effort: JsString16,
    codex_models: SourceJson,
    updated_at_ms: i64,
}
fn checked_runtime_config(
    snapshot: &ModelSnapshot,
    default_model: Option<&ModelId>,
    default_effort: Option<&JsString16>,
    now_ms: i64,
) -> Result<SourceJson, RuntimeConfigFailure> {
    let compact = compact_projection(snapshot)?;
    let requested = default_model
        .map(|m| gateway_schema_values::source_view::js_trim_source(m.as_js_string()))
        .filter(|s| !s.as_units().is_empty());
    let model = requested
        .or_else(|| compact.slugs.first().cloned())
        .ok_or_else(|| invalid("runtime config requires a default model"))?;
    if !compact.slugs.iter().any(|slug| slug == &model) {
        let prefix =
            JsString16::from_utf8("runtime config default model is absent from the catalog: ");
        return Err(RuntimeConfigFailure::Invalid(SourceRequestError::new(
            503,
            gateway_schema_values::source_view::concat(&[&prefix, &model]),
            "invalid_request_error",
            Some("runtime_config_invalid"),
            None,
        )));
    }
    let effort = default_effort
        .map(gateway_schema_values::source_view::js_trim_source)
        .filter(|s| !s.as_units().is_empty())
        .unwrap_or_else(|| "medium".into());
    let value = SourceJson::from_serializable(&RuntimeConfigDocument {
        version: 2,
        default_model: model,
        default_reasoning_effort: effort,
        codex_models: compact.document,
        updated_at_ms: now_ms,
    })?;
    let value = source::canonical(&value)?;
    let bytes = value.as_str().len();
    if bytes > 4096 {
        return Err(invalid(format!(
            "runtime config is too large ({bytes} bytes; maximum 4096 bytes)"
        )));
    }
    Ok(value)
}
pub fn build_runtime_config(
    snapshot: &ModelSnapshot,
    default_model: Option<&ModelId>,
    default_effort: Option<&JsString16>,
    now_ms: i64,
) -> Result<SourceJson, SourceRequestError> {
    checked_runtime_config(snapshot, default_model, default_effort, now_ms)
        .map_err(RuntimeConfigFailure::request)
}
pub fn build_optional_runtime_config(
    snapshot: &ModelSnapshot,
    default_model: Option<&ModelId>,
    default_effort: Option<&JsString16>,
    now_ms: i64,
) -> Result<Option<SourceJson>, GatewayError> {
    match checked_runtime_config(snapshot, default_model, default_effort, now_ms) {
        Ok(value) => Ok(Some(value)),
        Err(RuntimeConfigFailure::Invalid(_)) => Ok(None),
        Err(RuntimeConfigFailure::Encoding(error)) => Err(error),
    }
}
struct RuntimeArguments {
    snapshot: ModelSnapshot,
    model: ModelId,
    effort: JsString16,
    time: i64,
}
fn runtime_arguments(value: &SourceJson) -> Option<RuntimeArguments> {
    if source::member(value, "version")
        .as_ref()
        .and_then(source::number)
        != Some(2.0)
    {
        return None;
    }
    let snapshot = source::member(value, "codex_models")?
        .deserialize::<ModelSnapshot>()
        .ok()?;
    let model = default_model(value)?;
    let effort = default_reasoning_effort(value)?;
    let time = source::member(value, "updated_at_ms")
        .as_ref()
        .and_then(source::number)?
        .trunc() as i64;
    if time <= 0 {
        return None;
    }
    Some(RuntimeArguments {
        snapshot,
        model,
        effort,
        time,
    })
}
pub fn normalize_runtime_config(value: &SourceJson) -> Result<Option<SourceJson>, GatewayError> {
    let Some(args) = runtime_arguments(value) else {
        return Ok(None);
    };
    build_optional_runtime_config(
        &args.snapshot,
        Some(&args.model),
        Some(&args.effort),
        args.time,
    )
}
