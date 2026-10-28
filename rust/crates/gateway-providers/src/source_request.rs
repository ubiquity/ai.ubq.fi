use crate::request::codex_user_agent;
use gateway_contracts::model::ModelId;
use gateway_contracts::provider::UpstreamProvider;
use gateway_local_refusal::source_error::SourceRequestError;
use gateway_provider_input::request::bearer_headers;
use gateway_provider_input::source_controls as controls;
use gateway_provider_input::source_request::source_prelude;
use gateway_provider_protocol::types::ClientSurface;
use gateway_provider_protocol::types::DecodePlan;
use gateway_provider_protocol::types::PrepareOptions;
use gateway_provider_protocol::types::PreparedRequest;
use gateway_provider_protocol::types::ResponseDecode;
use gateway_provider_source::source as s;
use gateway_provider_source::types::ChatProfile;
use gateway_provider_tool_grammar::source_tools::ToolMap;
use gateway_schema::request::RequestSurface;
use gateway_source::error::GatewayError;
use gateway_source::js_string::JsString16;
use gateway_source::source_json::SourceJson;
use gateway_source::source_serialization as json;

fn invalid(message: &str, field: &str) -> SourceRequestError {
    s::invalid(JsString16::from_utf8(message), field)
}

fn unavailable(message: &str, code: &str) -> SourceRequestError {
    SourceRequestError::from_gateway(GatewayError::unavailable(message, code))
}

pub fn prepare_source_request(
    provider: UpstreamProvider,
    surface: ClientSurface,
    raw: &SourceJson,
    selected_model: &ModelId,
    options: &PrepareOptions,
) -> s::Result<PreparedRequest> {
    let (document, client_stream, requested, model) = source_prelude(
        provider,
        surface,
        raw,
        selected_model,
        &options.allowed_models,
    )?;
    let schema_surface = if surface == ClientSurface::Chat {
        RequestSurface::ChatCompletions
    } else {
        RequestSurface::Responses
    };
    let view = document.for_surface(schema_surface);
    if options.auth.secret.trim().is_empty() {
        return Err(unavailable(
            "The requested provider is not configured.",
            &format!(
                "{}_api_key_missing",
                gateway_provider_source::policy::direct_provider_label(provider)
            ),
        ));
    }
    if provider == UpstreamProvider::ClaudeCode {
        return gateway_provider_claude_body::source_request::prepare_scalar_claude(
            surface,
            raw,
            requested,
            model,
            client_stream,
            options,
        );
    }
    let direct = matches!(
        provider,
        UpstreamProvider::Deepseek | UpstreamProvider::Lithos | UpstreamProvider::Cerebras
    );
    let mut client_stream = client_stream;
    let mut effort = None;
    if surface == ClientSurface::Chat && direct {
        let values = s::present(raw, "messages")
            .and_then(|messages| json::array(&messages))
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
        client_stream = view.stream()?;
        view.chat_stream_options()?;
    }
    let input = Dispatch {
        provider,
        surface,
        raw,
        model: &model,
        options,
    };
    let mut transport = Transport::new(&input, client_stream);
    let url = match provider {
        UpstreamProvider::Deepseek | UpstreamProvider::Lithos | UpstreamProvider::Cerebras => {
            transport.direct(&input, effort)?
        }
        UpstreamProvider::Openrouter => transport.openrouter(&input)?,
        UpstreamProvider::ChatgptCodex => transport.codex(&input)?,
        UpstreamProvider::Surplus | UpstreamProvider::Metered => transport.ancillary(&input)?,
        _ => {
            return Err(unavailable(
                "This provider has no Chat/Responses transport",
                "provider_route_unavailable",
            ));
        }
    };
    transport.finish(&input, requested, url)
}

fn profile(provider: UpstreamProvider) -> ChatProfile {
    match provider {
        UpstreamProvider::Deepseek => ChatProfile::Deepseek,
        UpstreamProvider::Lithos => ChatProfile::Lithos,
        _ => ChatProfile::Cerebras,
    }
}

fn project_effort(value: &JsString16, profile: ChatProfile, param: &str) -> s::Result<SourceJson> {
    gateway_provider_chat::source_chat::effort(value, profile, param)
}

struct Dispatch<'a> {
    provider: UpstreamProvider,
    surface: ClientSurface,
    raw: &'a SourceJson,
    model: &'a ModelId,
    options: &'a PrepareOptions,
}
struct Transport {
    body: SourceJson,
    names: ToolMap,
    warnings: Vec<String>,
    decode: ResponseDecode,
    headers: gateway_provider_protocol::types::Headers,
    client_stream: bool,
}
impl Transport {
    fn new(input: &Dispatch<'_>, client_stream: bool) -> Self {
        Self {
            body: input.raw.clone(),
            names: ToolMap::new(),
            warnings: vec![],
            decode: if input.surface == ClientSurface::Responses {
                ResponseDecode::Responses
            } else {
                ResponseDecode::Chat
            },
            headers: bearer_headers(&input.options.auth.secret, "application/json"),
            client_stream,
        }
    }
    fn direct(&mut self, input: &Dispatch<'_>, effort: Option<JsString16>) -> s::Result<String> {
        let Dispatch {
            provider,
            surface,
            raw,
            model,
            ..
        } = *input;
        let url;
        let profile = profile(provider);
        if surface == ClientSurface::Responses {
            let projection = gateway_provider_chat::source_chat::project(
                raw,
                model,
                self.client_stream && profile != ChatProfile::Cerebras,
                profile,
            )?;
            self.body = projection.body;
            self.names = projection.names;
            self.warnings = projection.warnings;
            if s::present(&self.body, "reasoning_effort").is_none() {
                s::set(
                    &mut self.body,
                    "reasoning_effort",
                    s::text(gateway_provider_source::types::default_effort(
                        profile, model,
                    ))?,
                )?;
            }
            self.decode = ResponseDecode::ChatToResponses(profile);
        } else {
            s::set(&mut self.body, "model", s::quoted(model.as_js_string())?)?;
            let chosen = effort.unwrap_or_else(|| {
                JsString16::from_utf8(
                    if profile == ChatProfile::Deepseek
                        && !gateway_provider_chat::source_chat::thinking(raw, None)
                    {
                        "none"
                    } else {
                        gateway_provider_source::types::default_effort(profile, model)
                    },
                )
            });
            s::set(
                &mut self.body,
                "reasoning_effort",
                project_effort(&chosen, profile, "reasoning_effort")?,
            )?;
            if let Some(max) =
                s::present(raw, "max_completion_tokens").filter(|value| !s::is_null(value))
            {
                s::set(&mut self.body, "max_tokens", max)?;
            }
            s::remove(&mut self.body, "max_completion_tokens")?;
            if profile == ChatProfile::Deepseek {
                gateway_provider_chat::source_chat::validate_thinking(
                    &self.body,
                    "reasoning_effort",
                )?;
            }
        }
        if provider == UpstreamProvider::Deepseek {
            url = "https://api.deepseek.com/chat/completions".into();
            s::remove(&mut self.body, "thinking")?;
            if s::present(&self.body, "stream").and_then(|observed| s::boolean(&observed))
                == Some(true)
            {
                if s::present(&self.body, "stream_options")
                    .and_then(|observed| json::entries(&observed))
                    .is_none()
                {
                    s::set(
                        &mut self.body,
                        "stream_options",
                        s::scalar_json(&serde_json::json!({"include_usage":true}))?,
                    )?;
                }
            } else {
                s::remove(&mut self.body, "stream_options")?;
            }
            gateway_provider_chat_controls::projection::roles(&mut self.body, "messages", false)?;
        } else if provider == UpstreamProvider::Lithos {
            url = "https://api.lithosai.cloud/v1/chat/completions".into();
            if s::present(&self.body, "stream").and_then(|observed| s::boolean(&observed))
                != Some(true)
            {
                s::remove(&mut self.body, "stream_options")?;
            }
        } else {
            url = "https://api.cerebras.ai/v1/chat/completions".into();
            if model.matches_scalar("gpt-oss-120b")
                && s::present(&self.body, "reasoning_effort")
                    .is_some_and(|observed| s::is(&observed, "none"))
            {
                return Err(invalid(
                    "reasoning_effort 'none' is not supported by gpt-oss-120b",
                    "reasoning_effort",
                ));
            }
            gateway_provider_chat_controls::projection::prepare(&mut self.body)?;
            if self.client_stream {
                self.warnings.push("gpt_oss_stream_downgraded".into());
            }
        }
        Ok(url)
    }
    fn openrouter(&mut self, input: &Dispatch<'_>) -> s::Result<String> {
        let Dispatch { surface, model, .. } = *input;
        let url = format!(
            "https://openrouter.ai/api/v1/{}",
            if surface == ClientSurface::Responses {
                "responses"
            } else {
                "chat/completions"
            }
        );
        s::set(&mut self.body, "model", s::quoted(model.as_js_string())?)?;
        controls::openrouter(&mut self.body, model)?;
        Ok(url)
    }
    fn codex(&mut self, input: &Dispatch<'_>) -> s::Result<String> {
        let Dispatch {
            surface,
            raw,
            model,
            options,
            ..
        } = *input;
        let normalized =
            controls::ordinary(raw, surface, model, options.ordinary_preparation.as_ref())?;
        if surface == ClientSurface::Chat {
            self.client_stream = s::present(&normalized, "stream")
                .and_then(|observed| s::boolean(&observed))
                .unwrap_or(false);
        }
        self.body = controls::codex(&normalized)?;
        controls::strip_codex(&mut self.body, &mut self.warnings)?;
        self.decode = if surface == ClientSurface::Chat {
            ResponseDecode::ResponsesToChat
        } else {
            ResponseDecode::Responses
        };
        let url = "https://chatgpt.com/backend-api/codex/responses".into();
        self.headers = bearer_headers(&options.auth.secret, "text/event-stream");
        self.headers.insert(
            "ChatGPT-Account-ID".into(),
            options.auth.account_id.clone().ok_or_else(|| {
                unavailable(
                    "Codex account identity is unavailable",
                    "codex_auth_missing",
                )
            })?,
        );
        self.headers
            .insert("originator".into(), "codex_cli_rs".into());
        self.headers.insert(
            "user-agent".into(),
            codex_user_agent(&options.client_version),
        );
        if let Some(identity) = &options.native_session_id {
            for key in ["session-id", "thread-id", "x-client-request-id"] {
                self.headers.insert(key.into(), identity.clone());
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
        self.headers
            .insert("conversation_id".into(), conversation.clone());
        Ok(url)
    }
    fn surplus(
        &mut self,
        input: &Dispatch<'_>,
        provider_projection: &SourceJson,
    ) -> s::Result<String> {
        let Dispatch { model, options, .. } = *input;
        self.headers.insert("X-SI-Mask-Secrets".into(), "on".into());
        if let Some(projection) =
            gateway_provider_chat::source_anthropic::chat(provider_projection, options)?
        {
            self.body = projection.body;
            self.names = projection.names;
            let url = "https://api.surplusintelligence.ai/v1/chat/completions".into();
            self.decode = ResponseDecode::AnthropicChatToResponses;
            Ok(url)
        } else {
            let url = "https://api.surplusintelligence.ai/v1/responses".into();
            if options.supports_parallel_tools == Some(false) {
                s::remove(&mut self.body, "parallel_tool_calls")?;
            }
            if gateway_provider_source::anthropic::is_claude_model(model) {
                self.warnings
                    .push("surplus_claude_native_prompt_cache_unavailable".into());
            }
            gateway_provider_chat_controls::projection::roles(&mut self.body, "input", false)?;
            controls::ultra(&mut self.body)?;
            Ok(url)
        }
    }
    fn openlux(
        &mut self,
        input: &Dispatch<'_>,
        provider_projection: &SourceJson,
    ) -> s::Result<String> {
        let Dispatch { model, options, .. } = *input;
        let mut native_options = options.clone();
        native_options.supports_native_effort = false;
        native_options.supports_disabled_thinking = false;
        if let Some(native) = gateway_provider_claude_body::source_anthropic::native(
            provider_projection,
            &native_options,
        )? {
            self.body = native;
            let url = "https://api.openlux.ai/v1/messages".into();
            self.decode = ResponseDecode::AnthropicToResponses;
            self.headers
                .insert("anthropic-version".into(), "2023-06-01".into());
            Ok(url)
        } else {
            let url = "https://api.openlux.ai/v1/responses".into();
            if model.matches_scalar("gpt-5.6-sol")
                && let Some(effort) = s::present_path(&self.body, &["reasoning", "effort"])
                    .and_then(|observed| s::control(&observed))
            {
                let suffix = match effort.as_str() {
                    "none" | "minimal" => "low",
                    "ultra" => "max",
                    other => other,
                };
                if ["low", "medium", "high", "xhigh", "max"].contains(&suffix) {
                    s::set(
                        &mut self.body,
                        "model",
                        s::text(&format!("gpt-5.6-sol-{suffix}"))?,
                    )?;
                    s::remove(&mut self.body, "reasoning")?;
                }
            }
            if gateway_provider_source::anthropic::is_claude_model(model) {
                self.warnings
                    .push("openlux_claude_native_prompt_cache_unavailable".into());
            }
            Ok(url)
        }
    }
    fn ancillary(&mut self, input: &Dispatch<'_>) -> s::Result<String> {
        let Dispatch {
            provider,
            surface,
            raw,
            model,
            options,
        } = *input;
        let normalized =
            controls::ordinary(raw, surface, model, options.ordinary_preparation.as_ref())?;
        let mut provider_projection = normalized.clone();
        let previous_response_id_ignored = surface == ClientSurface::Responses
            && s::present(raw, "previous_response_id").is_some();
        if previous_response_id_ignored {
            // Claude's native projection cannot represent response-store state.
            // Ignore it on the provider clone, not the original source echo.
            s::remove(&mut provider_projection, "previous_response_id")?;
            self.warnings.push("previous_response_id_ignored".into());
        }
        if surface == ClientSurface::Chat {
            self.client_stream = s::present(&normalized, "stream")
                .and_then(|observed| s::boolean(&observed))
                .unwrap_or(false);
        }
        self.body = provider_projection.clone();
        self.decode = if surface == ClientSurface::Chat {
            ResponseDecode::ResponsesToChat
        } else {
            ResponseDecode::Responses
        };
        self.headers = bearer_headers(&options.auth.secret, "text/event-stream");
        let url = if provider == UpstreamProvider::Surplus {
            self.surplus(input, &provider_projection)?
        } else {
            self.openlux(input, &provider_projection)?
        };

        Ok(url)
    }
    fn finish(
        mut self,
        input: &Dispatch<'_>,
        requested: ModelId,
        url: String,
    ) -> s::Result<PreparedRequest> {
        let Dispatch {
            provider,
            surface,
            raw,
            model,
            options,
        } = *input;
        if surface == ClientSurface::Chat
            && matches!(
                provider,
                UpstreamProvider::Deepseek | UpstreamProvider::Lithos | UpstreamProvider::Cerebras
            )
        {
            let profile = match provider {
                UpstreamProvider::Deepseek => {
                    gateway_prompt_policy::admission::PromptProfile::Deepseek
                }
                UpstreamProvider::Lithos => gateway_prompt_policy::admission::PromptProfile::Lithos,
                _ => gateway_prompt_policy::admission::PromptProfile::Cerebras,
            };
            let admission = gateway_prompt_policy::admission::estimate(&self.body, profile)?;
            if !admission.admitted {
                let label = match provider {
                    UpstreamProvider::Deepseek => "DeepSeek",
                    UpstreamProvider::Lithos => "LithosAI",
                    _ => "Cerebras",
                };
                return Err(gateway_local_refusal::source_error::SourceRequestError::aggregate_context_overflow(JsString16::from_utf8(&gateway_prompt_policy::admission::failure_message(&admission, label))));
            }
        }
        // Codex's whitelist already excludes this, so detect the source field after its projection.
        if surface == ClientSurface::Responses
            && provider == UpstreamProvider::ChatgptCodex
            && s::present(raw, "previous_response_id").is_some()
        {
            s::remove(&mut self.body, "previous_response_id")?;
            self.warnings.push("previous_response_id_ignored".into());
        }
        let plan = DecodePlan {
            provider,
            surface,
            decode: self.decode,
            model: if surface == ClientSurface::Chat
                && matches!(
                    provider,
                    UpstreamProvider::Deepseek
                        | UpstreamProvider::Lithos
                        | UpstreamProvider::Cerebras
                ) {
                model.clone()
            } else {
                requested
            },
            upstream_model: model.clone(),
            response_id: options.response_id.clone(),
            created_at: options.created_at,
            stream: self.client_stream,
            request: raw.clone(),
            tool_names: self.names,
            reasoning_summary: options.reasoning_summary.clone(),
        };
        Ok(PreparedRequest {
            provider,
            url,
            method: "POST".into(),
            headers: self.headers,
            body: s::canonical(&self.body)?,
            decode: plan,
            warnings: self.warnings,
        })
    }
}
