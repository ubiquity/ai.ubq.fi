//! One source-phase-aware dispatcher. Host routing and accounting remain outside this crate.
use crate::{
    request::{bearer_headers, canonical_source_model, codex_user_agent},
    source as s, source_anthropic, source_chat, source_controls as controls,
    source_tools::ToolMap,
    types::{
        AuthMaterial, ChatProfile, ClaudeBodyOptions, ClientSurface, DecodePlan, Headers,
        OrdinaryPhaseOptions, PrepareOptions, PreparedClaudeBody, PreparedClaudeOrdinary,
        PreparedRequest, ResponseDecode,
    },
};
use gateway_core::{
    error::GatewayError, js_string::JsString16, model::ModelId, provider::UpstreamProvider,
    source_json::SourceJson, source_serialization as json,
};
use gateway_schema::{
    request::RequestSurface,
    source_request::{
        SourceModelRequest, SourceRequestDocument, error::SourceRequestError,
        ordinary::OrdinarySourceRequest,
    },
};
fn lift(error: GatewayError) -> SourceRequestError {
    SourceRequestError::from_gateway(error)
}
fn invalid(message: &str, field: &str) -> SourceRequestError {
    s::invalid(JsString16::from_utf8(message), field)
}
fn unavailable(message: &str, code: &str) -> SourceRequestError {
    lift(GatewayError::unavailable(message, code))
}
/// The selected provider's wire projection, filled by one per-provider arm.
struct Wire {
    body: SourceJson,
    names: ToolMap,
    warnings: Vec<String>,
    decode: ResponseDecode,
    headers: Headers,
    url: String,
    stream: bool,
}
pub fn prepare_source_request(
    provider: UpstreamProvider,
    surface: ClientSurface,
    raw: &SourceJson,
    selected_model: &ModelId,
    options: &PrepareOptions,
) -> s::Result<PreparedRequest> {
    let (view, client_stream, requested, model) = source_prelude(
        provider,
        surface,
        raw,
        selected_model,
        &options.allowed_models,
    )?;
    if options.auth.secret.trim().is_empty() {
        return Err(unavailable(
            "The requested provider is not configured.",
            &format!(
                "{}_api_key_missing",
                crate::policy::direct_provider_label(provider)
            ),
        ));
    }
    if provider == UpstreamProvider::ClaudeCode {
        let body_options = ClaudeBodyOptions::from_scalar(options);
        let body =
            prepare_claude_resolved(surface, raw, requested, model, client_stream, &body_options)?;
        return Ok(finalize_scalar_claude(body, &options.auth));
    }
    let direct = matches!(
        provider,
        UpstreamProvider::Deepseek | UpstreamProvider::Lithos | UpstreamProvider::Cerebras
    );
    let mut wire = Wire {
        body: raw.clone(),
        names: ToolMap::new(),
        warnings: vec![],
        decode: if surface == ClientSurface::Responses {
            ResponseDecode::Responses
        } else {
            ResponseDecode::Chat
        },
        headers: bearer_headers(&options.auth.secret, "application/json"),
        url: String::new(),
        stream: client_stream,
    };
    match provider {
        UpstreamProvider::Deepseek | UpstreamProvider::Lithos | UpstreamProvider::Cerebras => {
            direct_wire(&mut wire, provider, surface, raw, &model, &view)?;
        }
        UpstreamProvider::Openrouter => {
            wire.url = format!(
                "https://openrouter.ai/api/v1/{}",
                if surface == ClientSurface::Responses {
                    "responses"
                } else {
                    "chat/completions"
                }
            );
            s::set(&mut wire.body, "model", s::quoted(model.as_js_string()));
            controls::openrouter(&mut wire.body, &model);
            // Source Chat handler: the gateway needs the upstream usage frame to meter the call.
            if surface == ClientSurface::Chat && !client_stream {
                s::remove(&mut wire.body, "stream_options");
            } else if surface == ClientSurface::Chat
                && s::present(&wire.body, "stream_options").is_none()
            {
                let include = serde_json::json!({"include_usage":true});
                s::set(&mut wire.body, "stream_options", s::generated(&include));
            }
        }
        UpstreamProvider::ChatgptCodex => codex_wire(&mut wire, surface, raw, &model, options)?,
        UpstreamProvider::Surplus | UpstreamProvider::Metered => {
            paid_wire(&mut wire, provider, surface, raw, &model, options)?;
        }
        _ => {
            return Err(unavailable(
                "This provider has no Chat/Responses transport",
                "provider_route_unavailable",
            ));
        }
    }
    // Codex's whitelist already excludes this, so detect the source field after its projection.
    if surface == ClientSurface::Responses
        && provider == UpstreamProvider::ChatgptCodex
        && s::present(raw, "previous_response_id").is_some()
    {
        s::remove(&mut wire.body, "previous_response_id");
        wire.warnings.push("previous_response_id_ignored".into());
    }
    let plan = DecodePlan {
        provider,
        surface,
        decode: wire.decode,
        model: if surface == ClientSurface::Chat && direct {
            model.clone()
        } else {
            requested
        },
        upstream_model: model,
        response_id: options.response_id.clone(),
        created_at: options.created_at,
        stream: wire.stream,
        request: raw.clone(),
        tool_names: wire.names,
        reasoning_summary: options.reasoning_summary.clone(),
    };
    Ok(PreparedRequest {
        provider,
        url: wire.url,
        method: "POST".into(),
        headers: wire.headers,
        body: s::canonical(&wire.body)?,
        decode: plan,
        warnings: wire.warnings,
    })
}
fn direct_wire(
    wire: &mut Wire,
    provider: UpstreamProvider,
    surface: ClientSurface,
    raw: &SourceJson,
    model: &ModelId,
    view: &OrdinarySourceRequest,
) -> s::Result<()> {
    let mut effort = None;
    if surface == ClientSurface::Chat {
        let messages = s::get(raw, "messages");
        let values = json::array(&messages)
            .filter(|values| !values.is_empty())
            .ok_or_else(|| invalid("messages must be a non-empty array", "messages"))?;
        if values.iter().any(|value| json::entries(value).is_none()) {
            return Err(invalid("messages must contain objects", "messages"));
        }
        effort = view.reasoning_effort()?;
        // The direct model tier rule precedes stream/stream_options at this surface.
        if let Some(value) = &effort {
            project_effort(value, profile(provider), "reasoning_effort")?;
        }
        wire.stream = view.stream()?;
        view.chat_stream_options()?;
    }
    let profile = profile(provider);
    let body = &mut wire.body;
    if surface == ClientSurface::Responses {
        let projection = source_chat::project(
            raw,
            model,
            wire.stream && profile != ChatProfile::Cerebras,
            profile,
        )?;
        *body = projection.body;
        wire.names = projection.names;
        wire.warnings = projection.warnings;
        if s::present(body, "reasoning_effort").is_none() {
            s::set(
                body,
                "reasoning_effort",
                s::text(default_effort(profile, model)),
            );
        }
        wire.decode = ResponseDecode::ChatToResponses(profile);
    } else {
        s::set(body, "model", s::quoted(model.as_js_string()));
        let chosen = effort.unwrap_or_else(|| {
            JsString16::from_utf8(
                if profile == ChatProfile::Deepseek && !source_chat::thinking(raw, None) {
                    "none"
                } else {
                    default_effort(profile, model)
                },
            )
        });
        s::set(
            body,
            "reasoning_effort",
            project_effort(&chosen, profile, "reasoning_effort")?,
        );
        if let Some(max) =
            s::present(raw, "max_completion_tokens").filter(|value| !s::is_null(value))
        {
            s::set(body, "max_tokens", max);
        }
        s::remove(body, "max_completion_tokens");
        if profile == ChatProfile::Deepseek {
            source_chat::validate_thinking(body, "reasoning_effort")?;
        }
    }
    if provider == UpstreamProvider::Deepseek {
        wire.url = "https://api.deepseek.com/chat/completions".into();
        s::remove(body, "thinking");
        if s::boolean(&s::get(body, "stream")) == Some(true) {
            if json::entries(&s::get(body, "stream_options")).is_none() {
                s::set(
                    body,
                    "stream_options",
                    s::generated(&serde_json::json!({"include_usage":true})),
                );
            }
        } else {
            s::remove(body, "stream_options");
        }
        controls::roles(body, "messages", false);
    } else if provider == UpstreamProvider::Lithos {
        wire.url = "https://api.lithosai.cloud/v1/chat/completions".into();
        if s::boolean(&s::get(body, "stream")) != Some(true) {
            s::remove(body, "stream_options");
        }
    } else {
        wire.url = "https://api.cerebras.ai/v1/chat/completions".into();
        if model.matches_scalar("gpt-oss-120b") && s::is(&s::get(body, "reasoning_effort"), "none")
        {
            return Err(invalid(
                "reasoning_effort 'none' is not supported by gpt-oss-120b",
                "reasoning_effort",
            ));
        }
        s::set(body, "stream", s::generated(&false));
        for key in ["stream_options", "store", "metadata"] {
            s::remove(body, key);
        }
        if s::boolean(&s::get(body, "logprobs")) != Some(true) {
            s::remove(body, "top_logprobs");
        }
        controls::cerebras(body)?;
        if wire.stream {
            wire.warnings.push("gpt_oss_stream_downgraded".into());
        }
    }
    Ok(())
}
fn codex_wire(
    wire: &mut Wire,
    surface: ClientSurface,
    raw: &SourceJson,
    model: &ModelId,
    options: &PrepareOptions,
) -> s::Result<()> {
    let normalized =
        controls::ordinary(raw, surface, model, options.ordinary_preparation.as_ref())?;
    if surface == ClientSurface::Chat {
        wire.stream = s::boolean(&s::get(&normalized, "stream")).unwrap_or(false);
    }
    wire.body = controls::codex(&normalized)?;
    controls::strip_codex(&mut wire.body, &mut wire.warnings);
    wire.decode = if surface == ClientSurface::Chat {
        ResponseDecode::ResponsesToChat
    } else {
        ResponseDecode::Responses
    };
    wire.url = "https://chatgpt.com/backend-api/codex/responses".into();
    let headers = &mut wire.headers;
    *headers = bearer_headers(&options.auth.secret, "text/event-stream");
    headers.insert(
        "ChatGPT-Account-ID".into(),
        options.auth.account_id.clone().ok_or_else(|| {
            unavailable(
                "Codex account identity is unavailable",
                "codex_auth_missing",
            )
        })?,
    );
    headers.insert("originator".into(), "codex_cli_rs".into());
    headers.insert(
        "user-agent".into(),
        codex_user_agent(&options.client_version),
    );
    if let Some(identity) = &options.native_session_id {
        for key in ["session-id", "thread-id", "x-client-request-id"] {
            headers.insert(key.into(), identity.clone());
        }
    }
    let conversation = options
        .native_session_id
        .as_ref()
        .or(options.conversation_id.as_ref())
        .ok_or_else(|| {
            unavailable(
                "Codex conversation identity is unavailable",
                "codex_session_identity_missing",
            )
        })?;
    headers.insert("conversation_id".into(), conversation.clone());
    Ok(())
}
fn paid_wire(
    wire: &mut Wire,
    provider: UpstreamProvider,
    surface: ClientSurface,
    raw: &SourceJson,
    model: &ModelId,
    options: &PrepareOptions,
) -> s::Result<()> {
    let normalized =
        controls::ordinary(raw, surface, model, options.ordinary_preparation.as_ref())?;
    let mut provider_projection = normalized.clone();
    if surface == ClientSurface::Responses && s::present(raw, "previous_response_id").is_some() {
        // Claude's native projection cannot represent response-store state.
        // Ignore it on the provider clone, not the original source echo.
        s::remove(&mut provider_projection, "previous_response_id");
        wire.warnings.push("previous_response_id_ignored".into());
    }
    if surface == ClientSurface::Chat {
        wire.stream = s::boolean(&s::get(&normalized, "stream")).unwrap_or(false);
    }
    wire.body = provider_projection.clone();
    wire.decode = if surface == ClientSurface::Chat {
        ResponseDecode::ResponsesToChat
    } else {
        ResponseDecode::Responses
    };
    wire.headers = bearer_headers(&options.auth.secret, "text/event-stream");
    let claude = crate::anthropic::is_claude_model(model);
    if provider == UpstreamProvider::Surplus {
        wire.headers.insert("X-SI-Mask-Secrets".into(), "on".into());
        if let Some(projection) = source_anthropic::chat(&provider_projection, options) {
            wire.body = projection.body;
            wire.names = projection.names;
            wire.url = "https://api.surplusintelligence.ai/v1/chat/completions".into();
            wire.decode = ResponseDecode::AnthropicChatToResponses;
            return Ok(());
        }
        wire.url = "https://api.surplusintelligence.ai/v1/responses".into();
        if options.supports_parallel_tools == Some(false) {
            s::remove(&mut wire.body, "parallel_tool_calls");
        }
        if claude {
            wire.warnings
                .push("surplus_claude_native_prompt_cache_unavailable".into());
        }
        controls::roles(&mut wire.body, "input", false);
        controls::ultra(&mut wire.body);
        return Ok(());
    }
    let mut native_options = options.clone();
    native_options.supports_native_effort = false;
    native_options.supports_disabled_thinking = false;
    // TS metered.ts passes the native tool map so flattened names are restored on output.
    if let Some((native, names)) = source_anthropic::native(&provider_projection, &native_options) {
        wire.body = native;
        wire.names = names;
        wire.url = "https://api.openlux.ai/v1/messages".into();
        wire.decode = ResponseDecode::AnthropicToResponses;
        wire.headers
            .insert("anthropic-version".into(), "2023-06-01".into());
        return Ok(());
    }
    wire.url = "https://api.openlux.ai/v1/responses".into();
    let effort = s::control(&s::path(&wire.body, &["reasoning", "effort"]));
    if model.matches_scalar("gpt-5.6-sol")
        && let Some(effort) = effort
    {
        let suffix = match effort.as_str() {
            "none" | "minimal" => "low",
            "ultra" => "max",
            other => other,
        };
        if ["low", "medium", "high", "xhigh", "max"].contains(&suffix) {
            let sol = s::text(&format!("gpt-5.6-sol-{suffix}"));
            s::set(&mut wire.body, "model", sol);
            s::remove(&mut wire.body, "reasoning");
        }
    }
    if claude {
        wire.warnings
            .push("openlux_claude_native_prompt_cache_unavailable".into());
    }
    Ok(())
}
fn source_prelude(
    provider: UpstreamProvider,
    surface: ClientSurface,
    raw: &SourceJson,
    selected_model: &ModelId,
    allowed_models: &[ModelId],
) -> s::Result<(OrdinarySourceRequest, bool, ModelId, ModelId)> {
    let document = SourceRequestDocument::new(raw.clone())?;
    let schema_surface = if surface == ClientSurface::Chat {
        RequestSurface::ChatCompletions
    } else {
        RequestSurface::Responses
    };
    let view = document.for_surface(schema_surface);
    // The caller owns the common envelope phase; selected-provider preparation must not repeat it.
    let native_openrouter = provider == UpstreamProvider::Openrouter;
    // OpenRouter's native wire uses `=== true`, not ordinary parse_stream.
    let client_stream = if native_openrouter {
        s::boolean(&s::get(raw, "stream")) == Some(true)
    } else if surface == ClientSurface::Responses {
        view.stream()?
    } else {
        false
    };
    let requested = match view.model_request()? {
        SourceModelRequest::Default => selected_model.clone(),
        SourceModelRequest::Named(name) => ModelId::from_source(name),
    };
    if surface == ClientSurface::Chat {
        view.max_completion_tokens()?;
    }
    let model =
        canonical_source_model(provider, selected_model, allowed_models).ok_or_else(|| {
            SourceRequestError::new(
                404,
                s::concat(&[
                    &JsString16::from_utf8("The model '"),
                    requested.as_js_string(),
                    &JsString16::from_utf8("' does not exist or is not available."),
                ]),
                "invalid_request_error",
                Some("model_not_found"),
                None,
            )
        })?;
    Ok((view, client_stream, requested, model))
}

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
        client_stream = s::boolean(&s::get(&normalized, "stream")).unwrap_or(false);
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
        let (mut body, tool_names) = source_anthropic::native_claude(&phase.normalized, options).ok_or_else(|| invalid(
            "Claude request cannot be represented losslessly with the current credential/workspace capability proof",
            "reasoning",
        ))?;
        controls::claude_transport(&mut body, options.kind);
        let stream = s::boolean(&s::get(&body, "stream")) == Some(true);
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
                tool_names,
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
fn profile(provider: UpstreamProvider) -> ChatProfile {
    match provider {
        UpstreamProvider::Deepseek => ChatProfile::Deepseek,
        UpstreamProvider::Lithos => ChatProfile::Lithos,
        _ => ChatProfile::Cerebras,
    }
}
fn default_effort(profile: ChatProfile, model: &ModelId) -> &'static str {
    match profile {
        ChatProfile::Deepseek => "high",
        ChatProfile::Lithos => "medium",
        _ => {
            if model.matches_scalar("qwen-3.8-27b") {
                "high"
            } else {
                "medium"
            }
        }
    }
}
fn project_effort(value: &JsString16, profile: ChatProfile, param: &str) -> s::Result<SourceJson> {
    source_chat::effort(value, profile, param)
}
