use gateway_contracts::model::ModelId;
use gateway_contracts::provider::UpstreamProvider;
use gateway_provider_protocol::types::ClientSurface;
use gateway_provider_protocol::types::PrepareOptions;
use gateway_provider_protocol::types::PreparedRequest;
use gateway_provider_protocol::types::invalid;
use gateway_provider_source::source as s;
use gateway_source::error::GatewayError;
use gateway_source::source_json::SourceJson;
use serde_json::Value;

pub fn prepare_request(
    provider: UpstreamProvider,
    surface: ClientSurface,
    raw: &Value,
    options: &PrepareOptions,
) -> Result<PreparedRequest, GatewayError> {
    if surface == ClientSurface::Decisions {
        return gateway_provider_services::extras::prepare_source_decisions(
            &SourceJson::from_serializable(raw)?,
            options,
        )
        .map_err(s::scalar_error);
    }
    if surface == ClientSurface::SystemOne {
        return gateway_provider_services::extras::prepare_systemone(raw, options);
    }
    if surface == ClientSurface::Embeddings {
        return gateway_provider_services::extras::prepare_embeddings(raw, options);
    }
    let model = raw["model"]
        .as_str()
        .ok_or_else(|| invalid("model must be a string", "model"))?;
    crate::source_request::prepare_source_request(
        provider,
        surface,
        &s::scalar_json(raw)?,
        &ModelId::from_scalar(model),
        options,
    )
    .map_err(s::scalar_error)
}

pub fn codex_user_agent(version: &str) -> String {
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
