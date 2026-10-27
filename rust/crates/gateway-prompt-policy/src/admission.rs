//! Aggregate admission measures a complete provider projection without removing history.
use crate::tokens::estimate_tokens;
use gateway_source::{error::GatewayError, source_json::SourceJson, source_serialization as json};
use serde::Serialize;

pub const VERSION: &str = "chat-prompt-admission/v3";
pub const MINIMUM_OUTPUT_RESERVE: u64 = 16_384;
const MAX_SAFE_INTEGER: f64 = 9_007_199_254_740_991.0;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PromptProfile {
    Deepseek,
    Lithos,
    Cerebras,
}
impl PromptProfile {
    pub const fn context_window_tokens(self) -> u64 {
        match self {
            Self::Deepseek => 1_000_000,
            Self::Lithos => 1_048_576,
            Self::Cerebras => 131_072,
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PromptAdmission {
    pub policy: &'static str,
    pub estimated_prompt_tokens: u64,
    pub serialized_body_bytes: u64,
    pub structural_overhead_tokens: u64,
    pub requested_output_tokens: Option<u64>,
    pub output_reserve_tokens: u64,
    pub context_window_tokens: u64,
    pub prompt_budget_tokens: u64,
    pub admitted: bool,
}

fn array_len(body: &SourceJson, name: &str) -> u64 {
    json::member(body, name)
        .and_then(|value| json::array(&value))
        .map_or(0, |values| values.len() as u64)
}

/// The caller supplies the actual provider projection, including Cerebras's field removal.
pub fn estimate(
    body: &SourceJson,
    profile: PromptProfile,
) -> Result<PromptAdmission, GatewayError> {
    let serialized = json::canonical(body)?;
    let structural_overhead_tokens =
        256 + 16 * array_len(body, "messages") + 32 * array_len(body, "tools");
    let units = serialized.as_str().encode_utf16().collect::<Vec<_>>();
    let estimated_prompt_tokens = estimate_tokens(&units) as u64 + structural_overhead_tokens;
    let requested_output_tokens = json::member(body, "max_tokens")
        .and_then(|value| json::number(&value))
        .filter(|value| {
            value.is_finite() && *value > 0.0 && *value <= MAX_SAFE_INTEGER && value.fract() == 0.0
        })
        .map(|value| value as u64);
    let output_reserve_tokens = MINIMUM_OUTPUT_RESERVE.max(requested_output_tokens.unwrap_or(0));
    let context_window_tokens = profile.context_window_tokens();
    let prompt_budget_tokens = context_window_tokens.saturating_sub(output_reserve_tokens);
    Ok(PromptAdmission {
        policy: VERSION,
        estimated_prompt_tokens,
        serialized_body_bytes: serialized.as_str().len() as u64,
        structural_overhead_tokens,
        requested_output_tokens,
        output_reserve_tokens,
        context_window_tokens,
        prompt_budget_tokens,
        admitted: estimated_prompt_tokens <= prompt_budget_tokens,
    })
}

/// A failed provider measurement projection conservatively measures the original body.
pub fn estimate_projected(
    body: &SourceJson,
    profile: PromptProfile,
    project: impl FnOnce(&SourceJson) -> Result<SourceJson, GatewayError>,
) -> Result<PromptAdmission, GatewayError> {
    match project(body) {
        Ok(projected) => estimate(&projected, profile),
        Err(_) => estimate(body, profile),
    }
}

pub fn failure_message(admission: &PromptAdmission, provider_label: &str) -> String {
    format!(
        "the complete projected {provider_label} prompt is an estimated {} tokens (the calibrated estimator over {} serialized UTF-8 bytes plus {} structural tokens), above the {}-token budget from a {}-token window minus a {}-token output reserve under {}. Reduce the prompt; no payload was dispatched.",
        admission.estimated_prompt_tokens,
        admission.serialized_body_bytes,
        admission.structural_overhead_tokens,
        admission.prompt_budget_tokens,
        admission.context_window_tokens,
        admission.output_reserve_tokens,
        admission.policy,
    )
}
