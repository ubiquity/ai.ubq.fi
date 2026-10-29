//! Complete pure native-catalog window and compaction serving projection.
use gateway_contracts::model::ModelId;
use gateway_model_metadata::metadata::{MetadataHint, resolved_auto_compact};
use gateway_schema_values::source_view::{model, positive};
use gateway_source::source_serialization as source;
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use std::collections::BTreeMap;

/// Ordinary DATA selection; it grants no cache, publication, or upstream rights.
pub enum Projection<'a> {
    Publication(&'a BTreeMap<ModelId, MetadataHint>),
    VerifiedCache,
}

enum CountField {
    Context,
    Maximum,
    Compact,
}
impl CountField {
    fn name(&self) -> &'static str {
        match self {
            Self::Context => "context_window",
            Self::Maximum => "max_context_window",
            Self::Compact => "auto_compact_token_limit",
        }
    }
}
fn encoding_error() -> GatewayError {
    GatewayError::unavailable(
        "Invalid source JSON serialization.",
        "source_json_serialization_invalid",
    )
}
fn with_count(
    record: &SourceJson,
    field: CountField,
    count: i64,
) -> Result<SourceJson, GatewayError> {
    let mut fields = source::entries(record).ok_or_else(encoding_error)?;
    let key = JsString16::from_utf8(field.name());
    let next = SourceJson::from_serializable(&count)?;
    if let Some((_, value)) = fields.iter_mut().find(|(name, _)| name == &key) {
        *value = next;
    } else {
        fields.push((key, next));
    }
    source::object_keys(fields)
}

/// Call after the existing full native-catalog validator, at the original phase.
/// None preserves the exact original served bytes; encoding errors propagate.
pub fn project_native_catalog(
    parsed: &SourceJson,
    projection: Projection<'_>,
) -> Result<Option<SourceJson>, GatewayError> {
    let mut models = source::member(parsed, "models")
        .as_ref()
        .and_then(source::array)
        .ok_or_else(encoding_error)?;
    let mut changed = false;
    if let Projection::Publication(enrichment) = projection {
        // The complete existing widening operation remains one pure operation.
        for record in &mut models {
            let Some(id) = model(record) else {
                continue;
            };
            let Some(hint) = enrichment.get(&id) else {
                continue;
            };
            let endpoint = positive(source::member(record, "context_window").as_ref());
            let ceiling = positive(source::member(record, "max_context_window").as_ref());
            let widest = [
                endpoint,
                ceiling,
                hint.context_window_tokens,
                hint.max_context_window_tokens,
            ]
            .into_iter()
            .flatten()
            .filter(|n| *n > 0)
            .max();
            let Some(widest) = widest else {
                continue;
            };
            if endpoint.is_none_or(|n| widest > n) {
                *record = with_count(record, CountField::Context, widest)?;
                changed = true;
            }
            if ceiling.is_none_or(|n| widest > n) {
                *record = with_count(record, CountField::Maximum, widest)?;
                changed = true;
            }
        }
    }
    // Also cover valid rows without enrichment and verified pre-fix caches.
    for record in &mut models {
        let Some(window) = positive(source::member(record, "context_window").as_ref()) else {
            continue;
        };
        let prior = source::member(record, "auto_compact_token_limit");
        let configured = positive(prior.as_ref());
        let limit =
            resolved_auto_compact(window, configured).min(resolved_auto_compact(window, None));
        if prior.as_ref().and_then(source::number) == Some(limit as f64) {
            continue;
        }
        *record = with_count(record, CountField::Compact, limit)?;
        changed = true;
    }
    if !changed {
        return Ok(None);
    }
    let mut fields = source::entries(parsed).ok_or_else(encoding_error)?;
    let key = JsString16::from_utf8("models");
    let value = source::json_array(&models)?;
    let (_, target) = fields
        .iter_mut()
        .find(|(name, _)| name == &key)
        .ok_or_else(encoding_error)?;
    *target = value;
    let projected = source::object_keys(fields)?;
    source::canonical(&projected).map(Some)
}
