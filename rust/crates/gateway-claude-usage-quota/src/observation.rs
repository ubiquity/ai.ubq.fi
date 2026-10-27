//! Accepted-observation data and its source producer codec; this type carries no current-account proof.
use crate::QuotaObservation;
use gateway_core::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use serde::Serialize;
#[derive(Clone, Debug, Serialize)]
pub struct AcceptedObservation {
    pub v: u32,
    pub observation_id: String,
    pub account_id: String,
    pub account_cohort_id: String,
    pub credential_kind: String,
    pub workspace_id: Option<JsString16>,
    pub source_observed_at_ms: Option<i64>,
    pub sampled_at_ms: i64,
    pub response_status: Option<u16>,
    pub state: String,
    pub observation: QuotaObservation,
}
impl AcceptedObservation {
    /// Source JSON number spelling also supports m19's integer timestamp DTO.
    pub fn source_value(&self) -> Result<SourceJson, GatewayError> {
        let mut fields = source::entries(&SourceJson::from_serializable(self)?)
            .ok_or_else(invalid_observation)?;
        let observation = fields
            .iter_mut()
            .find(|(name, _)| name == &JsString16::from_utf8("observation"))
            .ok_or_else(invalid_observation)?;
        let mut observation_fields =
            source::entries(&observation.1).ok_or_else(invalid_observation)?;
        let standard = observation_fields
            .iter_mut()
            .find(|(name, _)| name == &JsString16::from_utf8("standard"))
            .ok_or_else(invalid_observation)?;
        let mut remaining = source::entries(&standard.1).ok_or_else(invalid_observation)?;
        let mut ordered = Vec::new();
        // Actual observeClaudeCodeQuota producer order, not arbitrary consumer input order.
        for family in crate::STANDARD_FAMILIES {
            if let Some(index) = remaining
                .iter()
                .position(|(name, _)| name == &JsString16::from_utf8(family))
            {
                ordered.push(remaining.remove(index));
            }
        }
        ordered.extend(remaining);
        standard.1 = source::object_keys(ordered)?;
        observation.1 = source::object_keys(observation_fields)?;
        source::object_keys(fields).and_then(|value| source::canonical(&value))
    }
}

fn invalid_observation() -> GatewayError {
    GatewayError::unavailable(
        "Claude observation is invalid",
        "claude_observation_invalid",
    )
}

#[cfg(test)]
mod producer_order_tests {
    use super::*;
    #[test]
    fn accepted_standard_families_follow_actual_source_producer_order()
    -> Result<(), Box<dyn std::error::Error>> {
        let mut quota = QuotaObservation::empty(1_800_000_000_000);
        for (index, family) in crate::STANDARD_FAMILIES.into_iter().enumerate() {
            quota.standard.insert(
                family.into(),
                crate::StandardWindow {
                    limit: Some((index + 1) as f64),
                    remaining: None,
                    reset_at_ms: None,
                    observed_at_ms: quota.observed_at_ms,
                },
            );
        }
        let event = AcceptedObservation {
            v: 1,
            observation_id: "fixture-observation".into(),
            account_id: "fixture-account".into(),
            account_cohort_id: "fixture-cohort".into(),
            credential_kind: "api_key".into(),
            workspace_id: Some(JsString16::from_units(vec![119, 0xd800])),
            source_observed_at_ms: Some(1_800_000_000_000),
            sampled_at_ms: 1_800_000_000_000,
            response_status: Some(200),
            state: "available".into(),
            observation: quota,
        };
        let document = event.source_value()?;
        let observation = source::member(&document, "observation").ok_or("observation absent")?;
        let standard = source::member(&observation, "standard").ok_or("standard absent")?;
        let fields = source::entries(&standard).ok_or("standard is not an object")?;
        assert_eq!(
            fields
                .iter()
                .map(|(name, _)| name.try_utf8())
                .collect::<Result<Vec<_>, _>>()?,
            crate::STANDARD_FAMILIES
        );
        for (index, (_, value)) in fields.iter().enumerate() {
            assert_eq!(
                source::number(&source::member(value, "limit").ok_or("limit absent")?),
                Some((index + 1) as f64)
            );
        }
        assert_eq!(
            source::string(&source::member(&document, "workspace_id").ok_or("workspace absent")?),
            event.workspace_id
        );
        // Serialization must not mutate the in-memory map or any consumer's source JSON.
        assert_eq!(
            event
                .observation
                .standard
                .keys()
                .map(String::as_str)
                .collect::<Vec<_>>(),
            vec!["input-tokens", "output-tokens", "requests"]
        );
        Ok(())
    }
}
