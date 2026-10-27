use gateway_provider_source::source as s;
use crate::types::ClaudeCredentialKind;
use gateway_core::source_json::SourceJson;
use gateway_core::source_serialization as json;

pub fn claude_transport(body: &mut SourceJson, kind: ClaudeCredentialKind) -> Result<(), gateway_core::error::GatewayError> {
    s::remove(body, "prompt_cache_key")?;
    if kind == ClaudeCredentialKind::Oauth {
        let mut system = vec![s::scalar_json(&serde_json::json!({"type":"text","text":crate::finalize::CLAUDE_ATTRIBUTION}))?];
        system.extend(s::present(body, "system").and_then(|observed| json::array(&observed)).unwrap_or_default());
        s::set(body, "system", s::array(&system)?)?;
    }
    Ok(())
}
