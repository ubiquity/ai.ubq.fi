//! `chat-prompt-admission/v3`: the complete projected Chat body against one provider window minus
//! an output reserve. v3 never removes history; an overflow fails closed for every truncation mode.
use gateway_compaction::token_estimate::estimate_tokens;
use gateway_core::{
    error::GatewayError, js_string::JsString16, model::ModelId, source_json::SourceJson,
    source_serialization as json,
};
use gateway_schema::source_request::error::SourceRequestError;
use serde::Serialize;

pub const PROMPT_ADMISSION_VERSION: &str = "chat-prompt-admission/v3";
pub const MINIMUM_OUTPUT_RESERVE_TOKENS: usize = 16_384;
pub const REQUEST_STRUCTURAL_OVERHEAD_TOKENS: usize = 256;
pub const MESSAGE_STRUCTURAL_OVERHEAD_TOKENS: usize = 16;
pub const TOOL_STRUCTURAL_OVERHEAD_TOKENS: usize = 32;
const OVERFLOW_PREFIX: &str = "the complete projected ";
const OVERFLOW_SUFFIX: &str = " Reduce the prompt; no payload was dispatched.";
/// Each Chat-only provider profile keeps its own measured context window.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AdmissionProfile {
    Deepseek,
    Lithos,
    Cerebras,
}
impl AdmissionProfile {
    pub fn label(self) -> &'static str {
        match self {
            Self::Deepseek => "DeepSeek",
            Self::Lithos => "LithosAI",
            Self::Cerebras => "Cerebras",
        }
    }
    /// The provider hint owns the window. Cerebras publishes no `/v1/models` context length,
    /// so its `gpt-oss-120b` route hint owns the measured paid window for every id.
    pub fn window_tokens(self) -> Option<usize> {
        let (provider, model) = match self {
            Self::Deepseek => ("deepseek", "deepseek-flash"),
            Self::Lithos => ("lithos", "deepseek-ai/DeepSeek-V4.1-Flash"),
            Self::Cerebras => ("cerebras", "gpt-oss-120b"),
        };
        gateway_schema::provider_catalog::provider_hint(provider, &ModelId::from_scalar(model))
            .context_window_tokens
            .filter(|window| *window > 0)
            .and_then(|window| usize::try_from(window).ok())
    }
}
/// The aggregate budget and observed cost for one complete projected Chat body.
#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PromptAdmission {
    pub policy: &'static str,
    pub estimated_prompt_tokens: usize,
    pub serialized_body_bytes: usize,
    pub structural_overhead_tokens: usize,
    pub requested_output_tokens: Option<usize>,
    pub output_reserve_tokens: usize,
    pub context_window_tokens: usize,
    pub prompt_budget_tokens: usize,
    pub admitted: bool,
}
fn length(body: &SourceJson, name: &str) -> usize {
    json::member(body, name)
        .and_then(|value| json::array(&value))
        .map_or(0, |values| values.len())
}
/// JS `Number.isSafeInteger(value) && value > 0` over the serialized `max_tokens` member.
fn requested_output(body: &SourceJson) -> Option<usize> {
    json::member(body, "max_tokens")
        .and_then(|value| json::number(&value))
        .filter(|value| *value > 0.0 && *value <= 9_007_199_254_740_991.0 && value.fract() == 0.0)
        .map(|value| value as usize)
}
/// Estimates one complete projected Chat prompt against one provider window.
pub fn estimate_prompt_admission(
    body: &SourceJson,
    context_window_tokens: usize,
) -> Result<PromptAdmission, GatewayError> {
    let serialized = json::canonical(body)?;
    let serialized = serialized.as_str();
    let structural_overhead_tokens = REQUEST_STRUCTURAL_OVERHEAD_TOKENS
        + length(body, "messages") * MESSAGE_STRUCTURAL_OVERHEAD_TOKENS
        + length(body, "tools") * TOOL_STRUCTURAL_OVERHEAD_TOKENS;
    let estimated_prompt_tokens =
        estimate_tokens(&JsString16::from_utf8(serialized)) + structural_overhead_tokens;
    let requested_output_tokens = requested_output(body);
    let output_reserve_tokens =
        MINIMUM_OUTPUT_RESERVE_TOKENS.max(requested_output_tokens.unwrap_or(0));
    let prompt_budget_tokens = context_window_tokens.saturating_sub(output_reserve_tokens);
    Ok(PromptAdmission {
        policy: PROMPT_ADMISSION_VERSION,
        estimated_prompt_tokens,
        serialized_body_bytes: serialized.len(),
        structural_overhead_tokens,
        requested_output_tokens,
        output_reserve_tokens,
        context_window_tokens,
        prompt_budget_tokens,
        admitted: estimated_prompt_tokens <= prompt_budget_tokens,
    })
}
/// The client-facing explanation for an aggregate admission refusal.
pub fn failure_message(admission: &PromptAdmission, provider_label: &str) -> String {
    format!(
        "{OVERFLOW_PREFIX}{provider_label} prompt is an estimated {} tokens (the calibrated estimator over {} serialized UTF-8 bytes plus {} structural tokens), above the {}-token budget from a {}-token window minus a {}-token output reserve under {}.{OVERFLOW_SUFFIX}",
        admission.estimated_prompt_tokens,
        admission.serialized_body_bytes,
        admission.structural_overhead_tokens,
        admission.prompt_budget_tokens,
        admission.context_window_tokens,
        admission.output_reserve_tokens,
        admission.policy,
    )
}
/// Source `aggregateContextOverflow`: HTTP 400 `context_length_exceeded` on `input`. The
/// diagnostic carries the gateway-built message so the trusted internal distinction below
/// survives the routing boundary; no request field or metadata can produce it.
pub fn aggregate_context_overflow(
    admission: &PromptAdmission,
    provider_label: &str,
) -> SourceRequestError {
    SourceRequestError::from_gateway(
        GatewayError::new(
            400,
            failure_message(admission, provider_label),
            Some("context_length_exceeded"),
        )
        .with_param(Some("input")),
    )
}
/// A selected provider profile without a verified window refuses aggregate admission.
pub fn missing_window(provider_label: &str) -> SourceRequestError {
    SourceRequestError::new(
        400,
        JsString16::from_utf8(&format!(
            "{provider_label} has no verified prompt window; aggregate admission refused the request."
        )),
        "invalid_request_error",
        Some("context_length_exceeded"),
        Some(Some(JsString16::from_utf8("model"))),
    )
}
/// True only for the gateway-built aggregate overflow diagnostic, never a per-field refusal.
pub fn is_aggregate_context_overflow(error: &GatewayError) -> bool {
    let fields = &error.envelope.error;
    error.status == 400
        && fields.code.as_deref() == Some("context_length_exceeded")
        && fields.param == Some(Some("input".into()))
        && fields.message.starts_with(OVERFLOW_PREFIX)
        && fields.message.ends_with(&format!(
            " under {PROMPT_ADMISSION_VERSION}.{OVERFLOW_SUFFIX}"
        ))
}
