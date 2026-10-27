use gateway_provider_source::source as s;
use gateway_core::model::ModelId;
use gateway_core::provider::UpstreamProvider;

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
    let model = ModelId::from_source(s::trim(model.as_js_string()));
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


pub fn codex_synthetic_id(id: &str) -> bool {
    let (response, rest) = if let Some(rest) = id.strip_prefix("resp_") {
        let Some((response, rest)) = rest.split_once('_') else {
            return false;
        };
        (response, rest)
    } else {
        let Some((kind, rest)) = id.split_once('_') else {
            return false;
        };
        if !["rs", "msg", "fc", "ctc"].contains(&kind) {
            return false;
        }
        let Some(rest) = rest.strip_prefix("resp_") else {
            return false;
        };
        let Some((response, rest)) = rest.split_once('_') else {
            return false;
        };
        if response.is_empty()
            || response.len() > 40
            || !response.chars().all(|c| c.is_ascii_alphanumeric())
        {
            return false;
        }
        return rest.split('_').count() <= 2
            && rest
                .split('_')
                .all(|s| !s.is_empty() && s.chars().all(|c| c.is_ascii_digit()));
    };
    if response.is_empty()
        || response.len() > 40
        || !response.chars().all(|c| c.is_ascii_alphanumeric())
    {
        return false;
    }
    let Some((kind, indexes)) = rest.split_once('_') else {
        return false;
    };
    ["rs", "msg", "fc", "ctc"].contains(&kind)
        && indexes.split('_').count() <= 2
        && indexes
            .split('_')
            .all(|s| !s.is_empty() && s.chars().all(|c| c.is_ascii_digit()))
}
