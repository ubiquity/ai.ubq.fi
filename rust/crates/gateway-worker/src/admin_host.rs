//! Finite administrative host bindings. Missing external owners remain explicit.
use gateway_admin_runtime_contracts::{
    RuntimeConfig, capabilities::AnalyticsRequest, capabilities::CatalogRequest,
    capabilities::Context, capabilities::ProviderRequest, capabilities::RuntimeCapabilities,
};
use gateway_contracts::identity::RequestId;
use gateway_source::error::GatewayError;
use worker::Env;
use {gateway_auth_contracts::types::AuthResponse, gateway_catalog_admin::admin::CatalogAdmin};
use {gateway_schema_values::scalar::js_trim, std::cmp::Ordering};

pub fn config(env: &Env) -> RuntimeConfig {
    let raw = crate::host::setting(env, "CLAUDE_CODE_ALLOWED_KEY_IDS").unwrap_or_default();
    let mut ids = Vec::new();
    for value in
        raw.split(|character: char| character == ',' || js_trim(&character.to_string()).is_empty())
    {
        let value = js_trim(value);
        if !value.is_empty() && !ids.iter().any(|old| old == value) {
            ids.push(value.to_owned());
        }
    }
    RuntimeConfig {
        claude_allowed_key_ids: ids,
    }
}

pub struct Capabilities {
    request_id: String,
    original_body_bytes: usize,
    catalog_service: gateway_catalog_runtime::CatalogRuntime,
    catalog_host: crate::catalog_host::Host,
    observations: crate::observability_host::Runtime,
    support: crate::admin_support_host::Binding,
    original: gateway_auth_contracts::types::AuthRequest,
    auth_config: gateway_auth_contracts::types::AuthConfig,
    actor: Option<std::rc::Rc<gateway_authority::native_control::actor::NativeRenewalActor>>,
}

impl Capabilities {
    pub fn new(
        request_id: &RequestId,
        original_body: &[u8],
        original_request: &gateway_auth_contracts::types::AuthRequest,
        store: gateway_worker_authority_binding::authority::RemoteAuthorityStore,
        platform: &crate::host::PlatformHost,
        context: std::rc::Rc<worker::Context>,
        actor: Option<std::rc::Rc<gateway_authority::native_control::actor::NativeRenewalActor>>,
    ) -> Result<Self, GatewayError> {
        let observations = crate::observability_host::bind_admin(
            request_id,
            original_body,
            store.clone(),
            platform,
            context.clone(),
            actor.clone(),
        )?;
        let catalog_host = crate::catalog_host::Host {
            store: store.clone(),
            env: platform.env.clone(),
            platform: platform.clone(),
            context,
        };
        let support =
            crate::admin_support_host::Binding::new(catalog_host.clone(), observations.clone());
        // AuthService consumes the exact original authentication metadata. Its
        // raw body stays in the separately measured request receipt, not a second copy.
        let original = gateway_auth_contracts::types::AuthRequest {
            method: original_request.method.clone(),
            url: original_request.url.clone(),
            headers: original_request.headers.clone(),
            body: Vec::new(),
            native_peer: original_request.native_peer.clone(),
        };
        Ok(Self {
            request_id: request_id.0.clone(),
            original_body_bytes: original_body.len(),
            catalog_service: crate::catalog::service(),
            catalog_host,
            observations,
            support,
            original,
            auth_config: crate::host::auth_config(&platform.env),
            actor,
        })
    }
    fn measured_body_bytes(&self, context: &Context) -> Result<usize, GatewayError> {
        if context.request_id != self.request_id {
            return Err(GatewayError::unavailable(
                "The administrative body receipt does not match this request.",
                "admin_body_receipt_mismatch",
            ));
        }
        Ok(self.original_body_bytes)
    }
}

impl RuntimeCapabilities for Capabilities {
    async fn migration(
        &self,
        request: gateway_admin_runtime_contracts::capabilities::MigrationRequest,
        context: Context,
    ) -> Result<AuthResponse, GatewayError> {
        self.measured_body_bytes(&context)?;
        self.catalog_host
            .store
            .profile_migration(request, context)
            .await
    }
    async fn capture(
        &self,
        request: gateway_admin_runtime_contracts::capabilities::CaptureRequest,
        context: Context,
    ) -> Result<AuthResponse, GatewayError> {
        self.measured_body_bytes(&context)?;
        use gateway_admin_runtime_contracts::capabilities::CaptureRequest;
        match request {
            CaptureRequest::Request { request_id } => {
                self.observations
                    .exports()
                    .export_capture_request(&request_id)
                    .await
            }
            CaptureRequest::Interval {
                after_ms,
                before_ms,
                cursor,
            } => {
                self.observations
                    .exports()
                    .export_capture_interval(after_ms, before_ms, cursor.as_deref())
                    .await
            }
            CaptureRequest::Incident {
                incident_id,
                cursor,
            } => {
                self.observations
                    .exports()
                    .export_incident_captures(&incident_id, cursor.as_deref())
                    .await
            }
            CaptureRequest::Incidents {
                incident_id,
                cursor,
                limit,
            } => {
                self.observations
                    .incidents()
                    .list_incidents(incident_id.as_deref(), cursor.as_deref(), limit)
                    .await
            }
        }
    }
    async fn codex(
        &self,
        request: gateway_admin_runtime_contracts::capabilities::CodexRequest,
        context: Context,
    ) -> Result<AuthResponse, GatewayError> {
        self.measured_body_bytes(&context)?;
        crate::codex_admin_host::execute(
            &self.catalog_host.store,
            &self.catalog_host.platform,
            crate::codex_admin_host::OriginalAdmin {
                request: &self.original,
                config: &self.auth_config,
                actor: self.actor.as_deref(),
            },
            request,
            context,
            &self.observations,
        )
        .await
    }
    async fn provider_picker(&self, context: Context) -> Result<serde_json::Value, GatewayError> {
        self.measured_body_bytes(&context)?;
        self.support.provider_picker(context).await
    }
    async fn metered_diagnostics(&self) -> Result<serde_json::Value, GatewayError> {
        self.support.metered_diagnostics().await
    }
    async fn pricing(
        &self,
        models: &[gateway_contracts::model::ModelId],
    ) -> Result<gateway_admin_runtime_contracts::capabilities::PricingSnapshot, GatewayError> {
        self.support.pricing(models).await
    }
    async fn error_diagnostics(
        &self,
        context: Context,
    ) -> Result<gateway_admin_runtime_contracts::capabilities::ErrorDiagnostics, GatewayError> {
        self.measured_body_bytes(&context)?;
        self.support.error_diagnostics(context).await
    }
    async fn supervisor_brief(
        &self,
        target: gateway_admin_runtime_contracts::capabilities::SupervisorBriefTarget,
        context: Context,
    ) -> Result<AuthResponse, GatewayError> {
        self.measured_body_bytes(&context)?;
        self.support.supervisor_brief(target, context).await
    }
    async fn claude(
        &self,
        request: gateway_admin_runtime_contracts::capabilities::ClaudeRequest,
        context: Context,
    ) -> Result<AuthResponse, GatewayError> {
        self.measured_body_bytes(&context)?;
        crate::claude_host::admin::admin(
            &self.catalog_host.store,
            &self.catalog_host.platform,
            &self.observations,
            request,
            context,
        )
        .await
    }
    async fn native(
        &self,
        request: gateway_auth_native_wire::native_read::NativeReadRequest,
        context: Context,
    ) -> Result<AuthResponse, GatewayError> {
        self.measured_body_bytes(&context)?;
        let platform = &self.catalog_host.platform;
        platform.native.rpc(request, platform).await
    }
    async fn catalog(
        &self,
        request: CatalogRequest,
        context: Context,
    ) -> Result<AuthResponse, GatewayError> {
        self.measured_body_bytes(&context)?;
        use gateway_catalog_admin::admin::AdminCatalogOperation as Operation;
        let operation = match request {
            CatalogRequest::CodexModelsGet => Operation::CodexModelsGet,
            CatalogRequest::CodexModelsSet { document } => Operation::CodexModelsSet { document },
            CatalogRequest::WhitelistGet => Operation::WhitelistGet,
            CatalogRequest::WhitelistSet { document } => Operation::WhitelistSet { document },
            CatalogRequest::Catalog => Operation::Catalog,
            CatalogRequest::Refresh => Operation::Refresh,
        };
        Ok(gateway_catalog_admin::response::into_auth_response(
            self.catalog_service
                .admin_catalog(&self.catalog_host.store, &self.catalog_host, operation)
                .await,
        ))
    }
    async fn provider(
        &self,
        request: ProviderRequest,
        context: Context,
    ) -> Result<AuthResponse, GatewayError> {
        self.measured_body_bytes(&context)?;
        self.observations.provider(request, context).await
    }
    async fn analytics(
        &self,
        request: AnalyticsRequest,
        context: Context,
    ) -> Result<AuthResponse, GatewayError> {
        self.measured_body_bytes(&context)?;
        self.observations.analytics(request, context).await
    }
    fn compare_admin_labels(
        &self,
        left: &gateway_source::js_string::JsString16,
        right: &gateway_source::js_string::JsString16,
    ) -> Result<Ordering, GatewayError> {
        gateway_worker_platform::source_collator::shared().compare(left, right)
    }
}
