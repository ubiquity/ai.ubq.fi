//! Source administrative runtime over the common authority. External effects are finite injected capabilities.
pub mod brief;
pub mod capabilities;
pub mod claude;
pub mod claude_access;
pub mod claude_generation;
mod defaults;
mod errors;
mod kernel;
mod kernel_data;
mod paid;
pub mod parse;
mod state;
mod store;

use capabilities::*;
use gateway_admin_contracts::fields;
use gateway_admin_contracts::{
    operations::{AdminHost, AdminInput, PaidKeyFields, missing_capability},
    routes::AdminOperation,
    source_input as source_body,
};
use gateway_auth::{
    native::NativeReadRequest,
    types::{AuthHost, AuthResponse, invalid},
};
use gateway_core::{error::GatewayError, query::QueryStore, state::RevisionCheck};
use serde_json::{Value, json};

#[derive(Clone, Debug, Default)]
pub struct RuntimeConfig {
    /// Explicit existing source configuration passed at bootstrap, never discovered by this crate.
    pub claude_allowed_key_ids: Vec<String>,
}

pub struct AdminRuntime<'a, S, H, C> {
    pub store: &'a S,
    pub host: &'a H,
    pub capabilities: &'a C,
    pub config: &'a RuntimeConfig,
}

impl<S: QueryStore, H: AuthHost, C: RuntimeCapabilities> AdminHost for AdminRuntime<'_, S, H, C> {
    async fn execute(
        &self,
        operation: AdminOperation,
        input: AdminInput,
    ) -> Result<AuthResponse, GatewayError> {
        // Defense in depth for callers outside AdminService. There is deliberately no super-admin tier.
        if !input.actor.is_admin {
            return Err(gateway_auth::types::unauthorized());
        }
        if !(0..=gateway_core::identity::MAX_SAFE_INTEGER).contains(&input.now_ms) {
            return Err(invalid("Admin request clock is invalid"));
        }
        let context = Context {
            request_id: input.request_id.clone(),
            now_ms: input.now_ms,
        };
        use AdminOperation::*;
        match operation {
            DefaultsGet | DefaultsSet => self.defaults(operation, &input).await,
            DebugGet | DebugSet | DebugDelete => self.debug(operation, &input).await,
            ProviderSelectionGet | ProviderSelectionSet => self.selection(operation, &input).await,
            ClaudeAccessGet | ClaudeAccessSet => self.claude_access(operation, &input).await,
            KernelUsageGet | KernelUsageSet | KernelUsageDelete | KernelQueue => {
                self.kernel(operation, &input).await
            }
            Errors => self.errors(&input).await,
            PromptsPurge => self.purge_prompts(input.now_ms).await,
            CodexModelsGet | CodexModelsSet | ModelsWhitelistGet | ModelsWhitelistSet
            | ModelsCatalog | ModelsRefresh => {
                let request = match operation {
                    CodexModelsGet => CatalogRequest::CodexModelsGet,
                    CodexModelsSet => CatalogRequest::CodexModelsSet {
                        document: input.body,
                    },
                    ModelsWhitelistGet => CatalogRequest::WhitelistGet,
                    ModelsWhitelistSet => CatalogRequest::WhitelistSet {
                        document: input.body,
                    },
                    ModelsCatalog => CatalogRequest::Catalog,
                    ModelsRefresh => CatalogRequest::Refresh,
                    _ => return Err(missing_capability(operation)),
                };
                self.capabilities.catalog(request, context).await
            }
            CodexAuthUpload | CodexResetGet | CodexResetConsume | CodexResetUpdate
            | CodexOverageUpdate | CodexOverageGet | CodexResetShadowList | CodexRecheck
            | CacheExperimentGet | CacheExperimentSet => {
                self.dispatch_codex(operation, input, context).await
            }
            Providers | HealthProviders | HealthUpstream | Capacity | CapacityObservations
            | CapacityRollups => {
                let request = match operation {
                    Providers => ProviderRequest::Inventory,
                    HealthProviders => ProviderRequest::PassiveHealth,
                    HealthUpstream => ProviderRequest::UpstreamHealth,
                    Capacity => ProviderRequest::Capacity {
                        live: fields::first(&input.query, "refresh") == Some("live"),
                    },
                    CapacityObservations => ProviderRequest::Observations {
                        document: input.body,
                    },
                    CapacityRollups => ProviderRequest::Rollups { query: input.query },
                    _ => return Err(missing_capability(operation)),
                };
                self.capabilities.provider(request, context).await
            }
            ClaudeAuthGet | ClaudeAuthSet | ClaudeAuthDelete | ClaudeAccountSelect
            | ClaudeQuotaClear => {
                let request = match operation {
                    ClaudeAuthGet => ClaudeRequest::State,
                    ClaudeAuthSet => {
                        ClaudeRequest::SetCredential(parse::claude_source(&input.body)?)
                    }
                    ClaudeAuthDelete => ClaudeRequest::DeleteAccount {
                        account_id: fields::first(&input.query, "account_id")
                            .map(gateway_core::js_string::JsString16::from_utf8),
                    },
                    ClaudeAccountSelect => {
                        let id = source_body::member(&input.body, "account_id")
                            .as_ref()
                            .and_then(source_body::text)
                            .ok_or_else(|| invalid("account_id is required"))?;
                        ClaudeRequest::SelectAccount { account_id: id }
                    }
                    ClaudeQuotaClear => {
                        let account_value = source_body::member(&input.body, "account_id");
                        if account_value
                            .as_ref()
                            .is_some_and(|value| source_body::text(value).is_none())
                        {
                            return Err(invalid("account_id must be a string"));
                        }
                        let account_id = account_value.as_ref().and_then(source_body::text);
                        let cohort_id = source_body::member(&input.body, "account_cohort_id");
                        // Account lookup and the targeted-cohort check belong to the
                        // actual owner in this source order; no early UUID/scalar gate.
                        ClaudeRequest::ClearQuota {
                            account_id,
                            cohort_id,
                        }
                    }
                    _ => return Err(missing_capability(operation)),
                };
                self.capabilities.claude(request, context).await
            }
            ReplayCaptures
            | SentinelIncidents
            | KvMigrationImport
            | KvMigrationValidate
            | PromptCacheAnalytics
            | QuotaProjection
            | QuotaProjectionBackfill
            | ApiKeyPaidFallbacks => self.dispatch_reporting(operation, input, context).await,
            SupervisorSessions => {
                self.native_response(NativeReadRequest::SupervisorSessions, context)
                    .await
            }
            SupervisorOutput | SupervisorBrief => {
                self.dispatch_supervisor(operation, input, context).await
            }
            // AdminService owns these operations; never duplicate key/passkey/public-key writers.
            _ => Err(missing_capability(operation)),
        }
    }
    async fn prepare_key_deletion(&self, key_id: &str) -> Result<Vec<RevisionCheck>, GatewayError> {
        self.prepare_deletion(key_id).await
    }
    async fn finish_key_deletion(
        &self,
        key_id: &str,
        prepared_checks: &[RevisionCheck],
    ) -> Result<(), GatewayError> {
        self.finish_deletion(key_id, prepared_checks).await
    }
    async fn abort_key_deletion(
        &self,
        key_id: &str,
        prepared_checks: &[RevisionCheck],
    ) -> Result<(), GatewayError> {
        self.abort_deletion(key_id, prepared_checks).await
    }
    async fn paid_key_policy(
        &self,
        body: &gateway_core::source_json::SourceJson,
    ) -> Result<PaidKeyFields, GatewayError> {
        self.paid_policy(body).await
    }
}

fn supervisor_target(
    operation: AdminOperation,
    input: &AdminInput,
) -> Result<SupervisorBriefTarget, GatewayError> {
    use AdminOperation::SupervisorOutput;
    let (source_id, thread_id) = if operation == SupervisorOutput {
        (
            fields::first(&input.query, "source")
                .unwrap_or("")
                .trim_matches(gateway_schema::util::js_whitespace)
                .to_owned(),
            fields::first(&input.query, "id")
                .unwrap_or("")
                .trim_matches(gateway_schema::util::js_whitespace)
                .to_owned(),
        )
    } else {
        (
            source_body::member(&input.body, "source")
                .as_ref()
                .and_then(source_body::text)
                .map(|value| source_body::trim(&value))
                .and_then(|value| value.try_utf8().ok())
                .unwrap_or_default(),
            source_body::member(&input.body, "id")
                .as_ref()
                .and_then(source_body::text)
                .map(|value| source_body::trim(&value))
                .and_then(|value| value.try_utf8().ok())
                .unwrap_or_default(),
        )
    };
    if source_id.is_empty()
        || source_id.len() > 32
        || !source_id.as_bytes()[0].is_ascii_lowercase()
            && !source_id.as_bytes()[0].is_ascii_digit()
        || !source_id
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b"_-".contains(&b))
    {
        return Err(invalid("Invalid supervisor source id"));
    }
    if thread_id.is_empty()
        || thread_id.len() > 200
        || !thread_id
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"._:-".contains(&b))
    {
        return Err(invalid("Invalid thread id"));
    }
    Ok(SupervisorBriefTarget {
        source_id,
        thread_id,
    })
}

fn parse_int_prefix(value: Option<&str>) -> Option<i64> {
    let value = value?.trim_start_matches(gateway_schema::util::js_whitespace);
    let end = value
        .char_indices()
        .take_while(|(i, c)| c.is_ascii_digit() || *i == 0 && (*c == '-' || *c == '+'))
        .last()
        .map(|(i, c)| i + c.len_utf8())?;
    value[..end].parse().ok()
}

impl<S: QueryStore, H: AuthHost, C: RuntimeCapabilities> AdminRuntime<'_, S, H, C> {
    async fn dispatch_codex(
        &self,
        operation: AdminOperation,
        input: AdminInput,
        context: Context,
    ) -> Result<AuthResponse, GatewayError> {
        use AdminOperation::*;
        let request = match operation {
            CodexAuthUpload => CodexRequest::ImportCredentials {
                document: input.body,
            },
            CodexResetGet => CodexRequest::ResetSettings,
            CodexOverageGet => CodexRequest::OverageSettings,
            CodexResetShadowList => CodexRequest::ShadowDecisions,
            CodexResetConsume => {
                let fields = gateway_core::source_serialization::entries(&input.body)
                    .ok_or_else(|| invalid("Only account_id_hash is accepted."))?;
                let hash = source_body::member(&input.body, "account_id_hash")
                    .as_ref()
                    .and_then(source_body::text)
                    .filter(|hash| {
                        hash.as_units().len() == 64
                            && hash
                                .as_units()
                                .iter()
                                .all(|unit| (48..=57).contains(unit) || (97..=102).contains(unit))
                    })
                    .filter(|_| fields.len() == 1)
                    .ok_or_else(|| invalid("Only account_id_hash is accepted."))?;
                let account_id_hash = String::from_utf16(hash.as_units())
                    .map_err(|_| invalid("Only account_id_hash is accepted."))?;
                CodexRequest::ConsumeReset { account_id_hash }
            }
            CodexResetUpdate | CodexOverageUpdate => {
                let name = if operation == CodexResetUpdate {
                    "enabled"
                } else {
                    "allow"
                };
                if source_body::member(&input.body, "account_id_hash")
                    .as_ref()
                    .and_then(source_body::text)
                    .is_none()
                    || source_body::member(&input.body, name)
                        .as_ref()
                        .and_then(source_body::boolean)
                        .is_none()
                {
                    return Err(invalid(&format!(
                        "account_id_hash and boolean {name} are required"
                    )));
                }
                if operation == CodexResetUpdate {
                    CodexRequest::SetResetSettings {
                        document: input.body,
                    }
                } else {
                    CodexRequest::SetOverageSettings {
                        document: input.body,
                    }
                }
            }
            CodexRecheck => {
                let slot = input
                    .parameter
                    .as_deref()
                    .and_then(|s| s.parse::<u8>().ok())
                    .filter(|s| (1..=2).contains(s))
                    .ok_or_else(|| {
                        GatewayError::new(404, "Codex account slot not found", Some("not_found"))
                    })?;
                CodexRequest::Recheck { slot }
            }
            CacheExperimentGet => CodexRequest::CacheExperimentBaseline,
            CacheExperimentSet => {
                let body: String = input.body.deserialize().map_err(|_| {
                    GatewayError::unavailable(
                        "Scope experiment requires its original raw body",
                        "admin_raw_body_unavailable",
                    )
                })?;
                if !body
                    .trim_matches(gateway_schema::util::js_whitespace)
                    .is_empty()
                {
                    return Err(invalid(
                        "Prompt-cache scope experiment does not accept request fields",
                    ));
                }
                CodexRequest::RunCacheExperiment
            }
            _ => return Err(missing_capability(operation)),
        };
        self.capabilities.codex(request, context).await
    }

    async fn dispatch_reporting(
        &self,
        operation: AdminOperation,
        input: AdminInput,
        context: Context,
    ) -> Result<AuthResponse, GatewayError> {
        use AdminOperation::*;
        match operation {
            ReplayCaptures | SentinelIncidents => {
                self.capabilities
                    .capture(
                        parse::capture(&input, operation == SentinelIncidents)?,
                        context,
                    )
                    .await
            }
            KvMigrationImport => {
                self.capabilities
                    .migration(MigrationRequest::Import(parse::migration(&input)?), context)
                    .await
            }
            KvMigrationValidate => {
                self.capabilities
                    .migration(MigrationRequest::Validate, context)
                    .await
            }
            PromptCacheAnalytics => {
                self.capabilities
                    .analytics(
                        AnalyticsRequest::PromptCache {
                            group_by: parse::prompt_groups(&input)?,
                        },
                        context,
                    )
                    .await
            }
            QuotaProjection => {
                let window = parse_int_prefix(fields::first(&input.query, "window_days"))
                    .filter(|n| [7, 30, 90].contains(n))
                    .unwrap_or(30);
                let balance = fields::first(&input.query, "balance_window_days")
                    .and_then(|s| {
                        s.trim_matches(gateway_schema::util::js_whitespace)
                            .parse::<f64>()
                            .ok()
                    })
                    .filter(|n| [7.0, 30.0, 90.0, 365.0].contains(n))
                    .unwrap_or(7.0) as u32;
                self.capabilities
                    .analytics(
                        AnalyticsRequest::QuotaProjection {
                            window_days: window as u32,
                            balance_window_days: balance,
                        },
                        context,
                    )
                    .await
            }
            QuotaProjectionBackfill => {
                let limit = parse_int_prefix(fields::first(&input.query, "limit"))
                    .filter(|n| *n > 0)
                    .unwrap_or(5000)
                    .min(10000) as u32;
                self.capabilities
                    .analytics(AnalyticsRequest::QuotaProjectionBackfill { limit }, context)
                    .await
            }
            ApiKeyPaidFallbacks => {
                let id = input
                    .parameter
                    .as_deref()
                    .map(gateway_schema::util::js_trim)
                    .filter(|s| !s.is_empty() && s.encode_utf16().count() <= 200)
                    .ok_or_else(|| invalid("Invalid API key id"))?;
                if store::read(
                    self.store,
                    &gateway_auth::api_keys::id_key(id),
                    input.now_ms,
                )
                .await?
                .is_none()
                {
                    return Err(GatewayError::new(404, "Not found", Some("not_found")));
                }
                let limit =
                    fields::bounded_positive(fields::first(&input.query, "limit"), 20, 100)?;
                self.capabilities
                    .analytics(
                        AnalyticsRequest::PaidHistory {
                            key_id: id.into(),
                            limit,
                        },
                        context,
                    )
                    .await
            }
            _ => Err(missing_capability(operation)),
        }
    }
    async fn dispatch_supervisor(
        &self,
        operation: AdminOperation,
        input: AdminInput,
        context: Context,
    ) -> Result<AuthResponse, GatewayError> {
        use AdminOperation::*;
        match operation {
            SupervisorOutput | SupervisorBrief => {
                let target = supervisor_target(operation, &input)?;
                if operation == SupervisorBrief {
                    return self.supervisor_brief_response(target, context).await;
                }
                let SupervisorBriefTarget {
                    source_id,
                    thread_id,
                } = target;
                let request = {
                    let cursor: String = fields::first(&input.query, "cursor")
                        .unwrap_or("")
                        .trim_matches(gateway_schema::util::js_whitespace)
                        .chars()
                        .take(240)
                        .collect();
                    NativeReadRequest::SupervisorOutput {
                        source_id,
                        thread_id,
                        cursor: (!cursor.is_empty()).then_some(cursor),
                    }
                };
                self.native_response(request, context).await
            }
            _ => Err(missing_capability(operation)),
        }
    }
    async fn supervisor_brief_response(
        &self,
        target: SupervisorBriefTarget,
        context: Context,
    ) -> Result<AuthResponse, GatewayError> {
        let response = self
            .capabilities
            .supervisor_brief(target.clone(), context)
            .await?;
        if response.status >= 400 && response.stream.is_none() {
            return Ok(response);
        }
        let body = response.body.as_json().ok_or_else(|| {
            GatewayError::unavailable(
                "Supervisor brief capability did not return the final public source response",
                "supervisor_brief_invalid_response",
            )
        })?;
        let fields = [
            "sourceId",
            "threadId",
            "machine",
            "about",
            "status",
            "model",
            "effort",
            "state",
            "sampledAtMs",
            "generatedAtMs",
            "transcript",
        ];
        let transcript_fields = ["turns", "bytes", "truncated", "redactions", "available"];
        let nullable = |name| body.get(name).is_some_and(|v| v.is_null() || v.is_string());
        let transcript = body.get("transcript");
        let valid = response.status == 200
            && response.stream.is_none()
            && body
                .as_object()
                .is_some_and(|v| v.keys().all(|key| fields.contains(&key.as_str())))
            && body.get("sourceId").and_then(Value::as_str) == Some(&target.source_id)
            && body.get("threadId").and_then(Value::as_str) == Some(&target.thread_id)
            && ["machine", "about", "status"].into_iter().all(|name| {
                body.get(name)
                    .and_then(Value::as_str)
                    .is_some_and(|s| !gateway_schema::util::js_trim(s).is_empty())
            })
            && ["model", "effort", "state"].into_iter().all(nullable)
            && ["sampledAtMs", "generatedAtMs"].into_iter().all(|name| {
                body.get(name)
                    .and_then(Value::as_i64)
                    .is_some_and(|n| (0..=gateway_core::identity::MAX_SAFE_INTEGER).contains(&n))
            })
            && transcript.and_then(Value::as_object).is_some_and(|v| {
                v.keys()
                    .all(|key| transcript_fields.contains(&key.as_str()))
            })
            && ["turns", "bytes", "redactions"].into_iter().all(|name| {
                transcript
                    .and_then(|v| v.get(name))
                    .and_then(Value::as_u64)
                    .is_some()
            })
            && ["truncated", "available"].into_iter().all(|name| {
                transcript
                    .and_then(|v| v.get(name))
                    .is_some_and(Value::is_boolean)
            })
            && (transcript.and_then(|v| v.get("available")) != Some(&Value::Bool(false))
                || body["model"].is_null() && body["effort"].is_null());
        if !valid {
            return Err(GatewayError::unavailable(
                "Supervisor brief capability did not return the final public source response",
                "supervisor_brief_invalid_response",
            ));
        }
        Ok(response)
    }
    async fn native_response(
        &self,
        request: NativeReadRequest,
        context: Context,
    ) -> Result<AuthResponse, GatewayError> {
        use gateway_auth::native::{NATIVE_WIRE_VERSION, NativeRpcEnvelope, NativeRpcResponse};
        let envelope = NativeRpcEnvelope {
            v: NATIVE_WIRE_VERSION,
            request: request.clone(),
        };
        let response = self.capabilities.native(request, context).await?;
        // The carrier validates its actual wire version. This shared validator also
        // prevents an internal capability from substituting JSON or another follow target.
        NativeRpcResponse {
            v: NATIVE_WIRE_VERSION,
            status: response.status,
            body: response.body.as_json().cloned().ok_or_else(|| {
                GatewayError::unavailable(
                    "Native protocol response is invalid",
                    "native_protocol_invalid",
                )
            })?,
            stream: response.stream.clone(),
        }
        .validate_for(&envelope)?;
        Ok(response)
    }
    async fn purge_prompts(&self, now: i64) -> Result<AuthResponse, GatewayError> {
        use gateway_auth::store::{check, commit};
        use gateway_core::state::RecordMutation;
        let key = store::key(json!(["uos_ai", "codex_instructions"]));
        let row = self.store.read(&key).await?;
        let mut deleted = 0;
        if store::value(row.as_ref(), now)?.is_some() {
            if !commit(
                self.store,
                vec![check(&key, row.as_ref())],
                vec![RecordMutation::Delete { key }],
            )
            .await?
            {
                return Err(gateway_auth::types::conflict(
                    "Prompt instructions changed concurrently; retry",
                ));
            }
            deleted += 1;
        }
        for entry in store::scan(
            self.store,
            store::key(json!(["uos_ai", "codex_instructions_chunk"])),
            false,
            now,
        )
        .await?
        {
            if !commit(
                self.store,
                vec![check(&entry.key, Some(&entry.record))],
                vec![RecordMutation::Delete { key: entry.key }],
            )
            .await?
            {
                return Err(gateway_auth::types::conflict(
                    "Prompt instructions changed concurrently; retry",
                ));
            }
            deleted += 1;
        }
        Ok(AuthResponse::json(200, json!({"deleted":deleted})))
    }
}
