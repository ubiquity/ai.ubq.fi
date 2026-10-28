//! src/request-policy.ts:98-163,412-451,565-613; tiers are deliberately open UTF16 strings.
use crate::effort_wire::EffortWireMap;
use gateway_source::{
    js_string::JsString16, source_json::SourceJson, source_serialization as source,
};
use serde::{Deserialize, Serialize};
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct ModelReasoning {
    pub levels: Vec<JsString16>,
    pub default_level: Option<JsString16>,
    /// Retained raw JSON or private normalized UTF16 wire pairs; ordinary DATA.
    pub wire_efforts: EffortWireMap,
}
impl Default for ModelReasoning {
    fn default() -> Self {
        Self {
            levels: Vec::new(),
            default_level: None,
            wire_efforts: EffortWireMap::normalized(Vec::new()),
        }
    }
}
/// Callers own the normalization phase; construction/equality of ModelId never invokes trim.
pub fn advertised_effort(value: &SourceJson) -> Option<JsString16> {
    let raw = if source::entries(value).is_some() {
        gateway_source::source_serialization::member(value, "effort")?
    } else {
        value.clone()
    };
    if raw.as_str() == "null" {
        Some("none".into())
    } else {
        gateway_schema_values::source_view::normalized(Some(&raw))
    }
}
impl ModelReasoning {
    pub fn from_record(record: Option<&SourceJson>) -> Self {
        let mut levels = Vec::new();
        if let Some(entries) = record
            .and_then(|r| {
                gateway_source::source_serialization::member(r, "supported_reasoning_levels")
            })
            .as_ref()
            .and_then(gateway_source::source_serialization::array)
        {
            for effort in entries.iter().filter_map(advertised_effort) {
                if !levels.contains(&effort) {
                    levels.push(effort);
                }
            }
        }
        let default_level = record
            .and_then(|r| {
                gateway_source::source_serialization::member(r, "default_reasoning_level")
            })
            .as_ref()
            .and_then(|v| {
                if v.as_str() == "null" {
                    Some("none".into())
                } else {
                    gateway_schema_values::source_view::normalized(Some(v))
                }
            });
        if let Some(default) = &default_level
            && !levels.contains(default)
        {
            levels.push(default.clone());
        }
        let mut wire_efforts = Vec::<(JsString16, JsString16)>::new();
        if let Some(wires) = record
            .and_then(|r| {
                gateway_source::source_serialization::member(r, "reasoning_effort_wire_map")
            })
            .as_ref()
            .and_then(source::entries)
        {
            for (key, value) in wires {
                let key = gateway_schema_values::source_view::js_trim_source(&key);
                if key.as_units().is_empty() {
                    continue;
                }
                if let Some(value) = gateway_schema_values::source_view::normalized(Some(&value)) {
                    if let Some((_, old)) = wire_efforts.iter_mut().find(|(old, _)| old == &key) {
                        *old = value;
                    } else {
                        wire_efforts.push((key, value));
                    }
                }
            }
        }
        Self {
            levels,
            default_level,
            wire_efforts: EffortWireMap::normalized(wire_efforts),
        }
    }
    pub fn wire_effort(&self, effort: &JsString16) -> Option<JsString16> {
        self.wire_efforts.wire_effort(effort)
    }
}
/// Explicit old scalar adapters; source runtime callers use source_request directly.
pub fn effort_for_codex(effort: &JsString16, model: &ModelReasoning) -> JsString16 {
    if gateway_schema_values::source_view::matches(effort, "none") {
        "none".into()
    } else if gateway_schema_values::source_view::matches(effort, "ultra") {
        "max".into()
    } else {
        model.wire_effort(effort).unwrap_or_else(|| effort.clone())
    }
}
#[derive(Clone, Debug, PartialEq)]
pub struct ResolvedReasoning {
    pub label: JsString16,
    pub value: SourceJson,
}
pub fn resolve_reasoning(
    value: Option<&SourceJson>,
    model: &ModelReasoning,
    default_effort: &JsString16,
) -> Result<ResolvedReasoning, gateway_local_refusal::source_error::SourceRequestError> {
    let parsed = gateway_request_normalization::syntax::parse_reasoning(value)?;
    let label = parsed
        .as_ref()
        .and_then(|r| gateway_source::source_serialization::member(r, "effort"))
        .as_ref()
        .and_then(|v| gateway_schema_values::source_view::normalized(Some(v)))
        .unwrap_or_else(|| default_effort.clone());
    let mut value = match parsed {
        Some(value) => value,
        None => source::object(vec![(
            "effort",
            source::quoted(&effort_for_codex(&label, model))?,
        )])?,
    };
    if let Some(effort) = gateway_source::source_serialization::member(&value, "effort")
        .as_ref()
        .and_then(gateway_source::source_serialization::string)
    {
        value = reasoning_with_effort(value, &effort_for_codex(&effort, model))?;
    }
    Ok(ResolvedReasoning { label, value })
}

fn reasoning_with_effort(
    value: SourceJson,
    effort: &JsString16,
) -> Result<SourceJson, gateway_source::error::GatewayError> {
    let mut fields = source::entries(&value).ok_or_else(|| {
        gateway_source::error::GatewayError::unavailable(
            "Invalid source JSON serialization.",
            "source_json_serialization_invalid",
        )
    })?;
    let key = JsString16::from_utf8("effort");
    let next = source::quoted(effort)?;
    if let Some((_, old)) = fields.iter_mut().find(|(name, _)| name == &key) {
        *old = next;
    } else {
        fields.push((key, next));
    }
    source::object_keys(fields)
}
