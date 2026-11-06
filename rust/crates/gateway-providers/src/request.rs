//! Exact provider-bound request construction. The caller supplies account/discovery capabilities.
use crate::types::*;
use gateway_core::{
    error::GatewayError, model::ModelId, provider::UpstreamProvider, source_json::SourceJson,
};
use serde_json::Value;
pub const LITHOS_MODELS: [&str; 8] = [
    "deepseek-ai/DeepSeek-V4.1-Flash",
    "deepseek-ai/DeepSeek-V4.1-Flash-fast",
    "deepseek-ai/DeepSeek-V4.1-Flash-ultra",
    "deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat",
    "moonshotai/Kimi-K3",
    "moonshotai/Kimi-K3-fast",
    "moonshotai/Kimi-K3-ultra",
    "moonshotai/Kimi-K3-ultra-chat",
];
pub fn canonical_model(
    provider: UpstreamProvider,
    model: &str,
    allowed: &[String],
) -> Option<String> {
    let lower = model.trim().to_lowercase();
    match provider {
        UpstreamProvider::Deepseek => match lower.as_str() {
            "deepseek-flash" | "deepseek-v4-flash" => Some("deepseek-flash".into()),
            "deepseek-v4-pro" => Some(lower),
            _ => None,
        },
        UpstreamProvider::Lithos => LITHOS_MODELS
            .iter()
            .find(|s| s.eq_ignore_ascii_case(&lower))
            .map(|s| (*s).into()),
        UpstreamProvider::Cerebras => ["gpt-oss-120b", "qwen-3.8-27b"]
            .contains(&lower.as_str())
            .then_some(lower),
        UpstreamProvider::Openrouter => {
            allowed.iter().find(|s| s.as_str() == model.trim()).cloned()
        }
        _ => (!model.trim().is_empty()).then(|| model.trim().to_owned()),
    }
}
/// Source model identity is retained; only the finite direct-provider matcher observes scalar aliases.
pub fn canonical_source_model(
    provider: UpstreamProvider,
    model: &ModelId,
    allowed: &[ModelId],
) -> Option<ModelId> {
    let model = ModelId::from_source(crate::source::trim(model.as_js_string()));
    match provider {
        UpstreamProvider::Deepseek | UpstreamProvider::Lithos | UpstreamProvider::Cerebras => model
            .as_js_string()
            .try_utf8()
            .ok()
            .and_then(|model| canonical_model(provider, &model, &[]))
            .map(|model| ModelId::from_scalar(&model)),
        UpstreamProvider::Openrouter => allowed
            .iter()
            .find(|candidate| **candidate == model)
            .cloned(),
        _ => (!model.as_js_string().as_units().is_empty()).then_some(model),
    }
}
pub(crate) fn bearer_headers(secret: &str, accept: &str) -> Headers {
    Headers::from([
        ("Authorization".into(), format!("Bearer {}", secret.trim())),
        ("Content-Type".into(), "application/json".into()),
        ("Accept".into(), accept.into()),
    ])
}
pub(crate) fn codex_synthetic_id(id: &str) -> bool {
    const KINDS: [&str; 4] = ["rs", "msg", "fc", "ctc"];
    let response =
        |s: &str| !s.is_empty() && s.len() <= 40 && s.chars().all(|c| c.is_ascii_alphanumeric());
    let digits = |s: &str| !s.is_empty() && s.chars().all(|c| c.is_ascii_digit());
    let indexes = |s: &str| s.split('_').count() <= 2 && s.split('_').all(digits);
    // `resp_<response>_<kind>_<indexes>` or `<kind>_resp_<response>_<indexes>`.
    let Some(rest) = id.strip_prefix("resp_") else {
        let tail = id.split_once('_').filter(|(kind, _)| KINDS.contains(kind));
        let tail = tail.and_then(|(_, rest)| rest.strip_prefix("resp_")?.split_once('_'));
        return tail.is_some_and(|(id, rest)| response(id) && indexes(rest));
    };
    let parts = rest.split_once('_').filter(|(id, _)| response(id));
    let parts = parts.and_then(|(_, rest)| rest.split_once('_'));
    parts.is_some_and(|(kind, rest)| KINDS.contains(&kind) && indexes(rest))
}
pub fn codex_replay_input(input: &Value) -> SourceJson {
    crate::source_controls::replay(&crate::source::generated(input))
}
pub fn build_codex_body(raw: &Value) -> Result<SourceJson, GatewayError> {
    crate::source_controls::codex(&crate::source::generated(raw))
        .map_err(crate::source::scalar_error)
}
pub fn chat_request_to_responses(raw: &Value) -> Result<SourceJson, GatewayError> {
    crate::source_controls::ordinary(
        &crate::source::generated(raw),
        ClientSurface::Chat,
        &ModelId::from_scalar(raw["model"].as_str().unwrap_or("")),
        None,
    )
    .map_err(crate::source::scalar_error)
}
pub fn prepare_request(
    provider: UpstreamProvider,
    surface: ClientSurface,
    raw: &Value,
    options: &PrepareOptions,
) -> Result<PreparedRequest, GatewayError> {
    if surface == ClientSurface::SystemOne {
        return crate::extras::prepare_systemone(raw, options);
    }
    if surface == ClientSurface::Embeddings {
        return crate::extras::prepare_embeddings(raw, options);
    }
    let model = raw["model"]
        .as_str()
        .ok_or_else(|| invalid("model must be a string", "model"))?;
    crate::source_request::prepare_source_request(
        provider,
        surface,
        &crate::source::generated(raw),
        &ModelId::from_scalar(model),
        options,
    )
    .map_err(crate::source::scalar_error)
}
pub fn project_cerebras_schema(value: &Value) -> SourceJson {
    crate::source_controls::cerebras_schema(&crate::source::generated(value))
}
pub(crate) fn codex_user_agent(version: &str) -> String {
    let version = version.trim();
    let parts = version
        .split('.')
        .map(str::parse::<f64>)
        .collect::<Result<Vec<_>, _>>();
    let chosen = if version.is_empty() {
        "0.100.0"
    } else if let Ok(parts) = parts {
        if (2..=3).contains(&parts.len())
            && parts.iter().all(|v| v.is_finite() && *v >= 0.0)
            && [
                parts[0] as u64,
                parts[1] as u64,
                parts.get(2).copied().unwrap_or(0.0) as u64,
            ] < [0, 100, 0]
        {
            "0.100.0"
        } else {
            version
        }
    } else {
        version
    };
    format!("codex_cli_rs/{chosen} (ai.ubq.fi)")
}
