use gateway_provider_input::request::bearer_headers;
use gateway_provider_source::source as s;
use gateway_provider_chat::source_anthropic;
use gateway_provider_input::source_controls as controls;
use gateway_provider_tool_grammar::source_tools::ToolMap;
use gateway_provider_protocol::types::AuthMaterial;
use crate::types::ClaudeBodyOptions;
use gateway_provider_protocol::types::ClientSurface;
use gateway_provider_protocol::types::DecodePlan;
use crate::types::OrdinaryPhaseOptions;
use gateway_provider_protocol::types::PrepareOptions;
use crate::types::PreparedClaudeBody;
use crate::types::PreparedClaudeOrdinary;
use gateway_provider_protocol::types::PreparedRequest;
use gateway_provider_protocol::types::ResponseDecode;
use gateway_core::model::ModelId;
use gateway_core::provider::UpstreamProvider;
use gateway_core::source_json::SourceJson;
use gateway_core::source_serialization as json;
use s::invalid;
use gateway_provider_input::source_request::source_prelude;

impl PreparedClaudeOrdinary {
    /// Finish the one ordinary source phase before any selected-account capability lookup.
    pub fn prepare(
        surface: ClientSurface,
        original: &SourceJson,
        selected_model: &ModelId,
        options: &OrdinaryPhaseOptions,
    ) -> s::Result<Self> {
        let (_, stream, requested, model) = source_prelude(
            UpstreamProvider::ClaudeCode,
            surface,
            original,
            selected_model,
            &options.allowed_models,
        )?;
        prepare_claude_ordinary_resolved(surface, original, requested, model, stream, options)
    }
}


fn prepare_claude_ordinary_resolved(
    surface: ClientSurface,
    raw: &SourceJson,
    requested: ModelId,
    model: ModelId,
    mut client_stream: bool,
    options: &OrdinaryPhaseOptions,
) -> s::Result<PreparedClaudeOrdinary> {
    let normalized =
        controls::ordinary(raw, surface, &model, options.ordinary_preparation.as_ref())?;
    if surface == ClientSurface::Chat {
        client_stream = s::present(&normalized, "stream").and_then(|observed| s::boolean(&observed)).unwrap_or(false);
    }
    // Syntax/default/mapping already ran in the shared coordinator. This is only
    // the exact data outcome used for account-bound lookup, never a proof.
    let effective_effort = json::member(&normalized, "reasoning")
        .and_then(|reasoning| json::member(&reasoning, "effort"))
        .as_ref()
        .and_then(json::string);
    Ok(PreparedClaudeOrdinary {
        normalized,
        surface,
        requested,
        model,
        client_stream,
        effective_effort,
    })
}


impl PreparedClaudeBody {
    /// Convenience API: the same single ordinary phase followed by native finalization.
    pub fn prepare(
        surface: ClientSurface,
        raw: &SourceJson,
        selected_model: &ModelId,
        options: &ClaudeBodyOptions,
    ) -> s::Result<Self> {
        let phase = PreparedClaudeOrdinary::prepare(
            surface,
            raw,
            selected_model,
            &OrdinaryPhaseOptions {
                allowed_models: options.allowed_models.clone(),
                ordinary_preparation: options.ordinary_preparation.clone(),
            },
        )?;
        Self::from_ordinary(phase, raw, options)
    }
    /// Consume the existing outcome once after real same-account capability selection.
    /// The caller retains the immutable original binding/echo; no normalization runs here.
    pub fn from_ordinary(
        phase: PreparedClaudeOrdinary,
        original: &SourceJson,
        options: &ClaudeBodyOptions,
    ) -> s::Result<Self> {
        let mut body = crate::source_anthropic::native_claude(&phase.normalized, options)?.ok_or_else(|| invalid(
            "Claude request cannot be represented losslessly with the current credential/workspace capability proof",
            "reasoning",
        ))?;
        controls::claude_transport(&mut body, options.kind);
        let stream = s::present(&body, "stream").and_then(|observed| s::boolean(&observed)) == Some(true);
        let body = s::canonical(&body)?;
        Ok(Self {
            body,
            kind: options.kind,
            stream,
            warnings: Vec::new(),
            decode: DecodePlan {
                provider: UpstreamProvider::ClaudeCode,
                surface: phase.surface,
                decode: ResponseDecode::AnthropicToResponses,
                model: phase.requested,
                upstream_model: phase.model,
                response_id: options.response_id.clone(),
                created_at: options.created_at,
                stream: phase.client_stream,
                request: original.clone(),
                tool_names: ToolMap::new(),
                reasoning_summary: options.reasoning_summary.clone(),
            },
        })
    }
}


fn prepare_claude_resolved(
    surface: ClientSurface,
    raw: &SourceJson,
    requested: ModelId,
    model: ModelId,
    client_stream: bool,
    options: &ClaudeBodyOptions,
) -> s::Result<PreparedClaudeBody> {
    let phase = prepare_claude_ordinary_resolved(
        surface,
        raw,
        requested,
        model,
        client_stream,
        &OrdinaryPhaseOptions {
            allowed_models: options.allowed_models.clone(),
            ordinary_preparation: options.ordinary_preparation.clone(),
        },
    )?;
    PreparedClaudeBody::from_ordinary(phase, raw, options)
}


fn finalize_scalar_claude(body: PreparedClaudeBody, auth: &AuthMaterial) -> PreparedRequest {
    let mut headers = bearer_headers(
        &auth.secret,
        if body.stream {
            "text/event-stream"
        } else {
            "application/json"
        },
    );
    headers.insert("anthropic-version".into(), "2023-06-01".into());
    if auth.api_key {
        headers.remove("Authorization");
        headers.insert("x-api-key".into(), auth.secret.clone());
    } else {
        headers.insert("anthropic-beta".into(), "oauth-2025-04-20".into());
    }
    if let Some(workspace) = &auth.workspace_id {
        headers.insert("anthropic-workspace-id".into(), workspace.clone());
    }
    PreparedRequest {
        provider: UpstreamProvider::ClaudeCode,
        url: body.url().into(),
        method: body.method().into(),
        headers,
        body: body.body,
        decode: body.decode,
        warnings: body.warnings,
    }
}


pub fn prepare_scalar_claude(surface: ClientSurface, original: &SourceJson, requested: ModelId, selected: ModelId, client_stream: bool, options: &PrepareOptions) -> s::Result<PreparedRequest> {
    let body_options = ClaudeBodyOptions::from_scalar(options);
    let body = prepare_claude_resolved(surface, original, requested, selected, client_stream, &body_options)?;
    Ok(finalize_scalar_claude(body, &options.auth))
}
