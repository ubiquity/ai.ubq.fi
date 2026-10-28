use crate::{
    host::{PlatformHost, provider_secret},
    observability_host,
};
use gateway_admin_runtime::claude::{self, AccountBinding, Bootstrap, CredentialKind};
use gateway_auth_contracts::{
    types::AuthHost, upstream::AuthUpstreamRequest, upstream::AuthUpstreamResponse,
    webauthn::CosePublicKey, webauthn::SignatureInput, webauthn::WebAuthnCrypto,
};
use gateway_catalog_provider::host::ProviderBinding;
use gateway_claude_runtime::host::{ClaudePoolHost, HealthObservation, WorkspaceResponse};
use gateway_claude_usage_quota::observation::AcceptedObservation;
use gateway_core::state::RevisionCheck;
use gateway_observability::{
    ObservabilityRuntime,
    types::{HealthEvent, HealthProvider, HealthTarget},
};
use gateway_schema::catalog::ProviderCatalog;
use gateway_source::error::GatewayError;
use gateway_worker_authority_binding::authority::RemoteAuthorityStore;
use serde_json::json;
use std::{cmp::Ordering, rc::Rc};

#[derive(Clone)]
pub(crate) struct Host {
    pub platform: PlatformHost,
    pub store: RemoteAuthorityStore,
}
impl Host {
    pub fn new(
        store: RemoteAuthorityStore,
        platform: PlatformHost,
        _context: Rc<worker::Context>,
    ) -> Self {
        Self { store, platform }
    }
}
pub(super) fn bootstrap(platform: &PlatformHost) -> Result<Bootstrap, GatewayError> {
    let now = platform.now_ms();
    if now <= 0 || now > gateway_contracts::identity::MAX_SAFE_INTEGER {
        return Err(super::invalid("Claude source clock is invalid."));
    }
    let credential = if let Some(secret) = provider_secret(&platform.env, "CLAUDE_API_KEY") {
        let mut credential = claude::parse_credential(
            &json!({"secret":secret,"updated_at_ms":now}),
            "CLAUDE_API_KEY",
        )
        .ok_or_else(|| super::invalid("Configured Claude credential is invalid."))?;
        credential.updated_at_ms = None;
        Some(credential)
    } else if let Some(secret) = provider_secret(&platform.env, "CLAUDE_CODE_OAUTH_TOKEN") {
        let mut credential = claude::parse_credential(
            &json!({"secret":secret,"kind":"oauth","updated_at_ms":now}),
            "CLAUDE_CODE_OAUTH_TOKEN",
        )
        .ok_or_else(|| super::invalid("Configured Claude credential is invalid."))?;
        credential.updated_at_ms = None;
        Some(credential)
    } else {
        None
    };
    let workspace = provider_secret(&platform.env, "CLAUDE_CODE_WORKSPACE_ID")
        .filter(|id| id.starts_with("wrkspc_"))
        .and_then(|id| claude::parse_workspace(&json!({"id":id,"source":"env"})));
    Ok(Bootstrap {
        credential,
        workspace,
    })
}
pub(crate) async fn bootstrap_for_source(
    store: &RemoteAuthorityStore,
    platform: &PlatformHost,
) -> Result<Bootstrap, GatewayError> {
    // Existing roster/tombstone and source legacy credential win over environment values.
    if claude::read_roster(store, platform.now_ms())
        .await?
        .is_some()
    {
        return Ok(Bootstrap::default());
    }
    if claude::load_roster(store, platform, &Bootstrap::default())
        .await?
        .revision
        .is_some()
    {
        return Ok(Bootstrap::default());
    }
    bootstrap(platform)
}
impl WebAuthnCrypto for Host {
    async fn fetch_certificate_revocation_list(&self, url: &str) -> Result<Vec<u8>, GatewayError> {
        self.platform.fetch_certificate_revocation_list(url).await
    }
    async fn validate_key(&self, key: &CosePublicKey) -> Result<(), GatewayError> {
        self.platform.validate_key(key).await
    }
    async fn verify_signature(&self, input: SignatureInput) -> Result<bool, GatewayError> {
        self.platform.verify_signature(input).await
    }
}
impl AuthHost for Host {
    fn now_ms(&self) -> i64 {
        self.platform.now_ms()
    }
    fn random_bytes(&self, length: usize) -> Result<Vec<u8>, GatewayError> {
        self.platform.random_bytes(length)
    }
    async fn fetch_auth(
        &self,
        request: AuthUpstreamRequest,
    ) -> Result<AuthUpstreamResponse, GatewayError> {
        self.platform.fetch_auth(request).await
    }
    async fn kernel_principal(
        &self,
        owner: &str,
        repo: &str,
        state_id: &str,
        path: &str,
    ) -> Result<String, GatewayError> {
        self.platform
            .kernel_principal(owner, repo, state_id, path)
            .await
    }
}
impl ClaudePoolHost for Host {
    fn compare_principals(
        &self,
        left: &gateway_source::js_string::JsString16,
        right: &gateway_source::js_string::JsString16,
    ) -> Result<Ordering, GatewayError> {
        gateway_worker_platform::source_collator::shared().compare(left, right)
    }
    fn parse_retry_after_date(&self, raw: &str) -> Option<i64> {
        let value = worker::js_sys::Date::parse(raw);
        value.is_finite().then_some(value as i64)
    }
    async fn credential_checks(
        &self,
        binding: &AccountBinding,
    ) -> Result<Vec<RevisionCheck>, GatewayError> {
        // These are all source roster parser variants. There is no native-Claude producer.
        if !["kv", "CLAUDE_API_KEY", "CLAUDE_CODE_OAUTH_TOKEN"]
            .contains(&binding.credential.source.as_str())
        {
            return Err(super::invalid(
                "Claude credential has no supported source owner.",
            ));
        }
        claude::current_binding(&self.store, binding, self.now_ms())
            .await?
            .map(|check| vec![check])
            .ok_or_else(|| super::invalid("Claude source credential changed."))
    }
    async fn catalog(
        &self,
        binding: &AccountBinding,
        identity: &ProviderBinding,
    ) -> Result<ProviderCatalog, GatewayError> {
        super::catalog::for_account(self, binding, identity).await
    }
    async fn workspace_list(
        &self,
        binding: &AccountBinding,
    ) -> Result<WorkspaceResponse, GatewayError> {
        let response = super::catalog::workspace_list(self, binding).await?;
        Ok(WorkspaceResponse {
            status: response.status,
            body: response.body,
        })
    }
    async fn accepted_observation(&self, event: &AcceptedObservation) -> Result<(), GatewayError> {
        let event =
            gateway_claude_usage_history_records::types::ClaudeAcceptedObservation::from_source(
                &event.source_value()?,
            )
            .map_err(|_| super::invalid("Accepted Claude history DTO is invalid."))?;
        let runtime = ObservabilityRuntime::with_state(
            self.store.clone(),
            observability_host::Host,
            observability_host::shared_state(),
        );
        let _accepted = runtime.record_claude_capacity_observation(&event).await?;
        Ok(())
    }
    async fn health_observation(&self, event: &HealthObservation) -> Result<(), GatewayError> {
        let kind = match event.event.as_str() {
            "success" => HealthEvent::Success,
            "reachable" => HealthEvent::Reachable,
            "auth_invalid" => HealthEvent::AuthInvalid,
            "quota_exhausted" => HealthEvent::QuotaExhausted,
            "upstream_error" => HealthEvent::UpstreamError,
            _ => return Err(super::invalid("Unknown Claude health event.")),
        };
        let runtime = ObservabilityRuntime::with_state(
            self.store.clone(),
            observability_host::Host,
            observability_host::shared_state(),
        );
        let _written = runtime
            .observe_provider(
                HealthTarget {
                    provider: HealthProvider::ClaudeCode,
                    identity: event.identity.clone(),
                    source_checks: event.checks.clone(),
                },
                kind,
                event.status,
                event.provider_request_id.as_deref(),
            )
            .await;
        Ok(())
    }
}

/// Local physical construction only; deliberately no provider status or raw JS error.
#[derive(Debug)]
pub(super) struct ConstructionFailure;

pub(super) fn credential_kind(
    credential: &claude::Credential,
) -> gateway_provider_claude_body::types::ClaudeCredentialKind {
    match credential.kind {
        CredentialKind::Oauth => gateway_provider_claude_body::types::ClaudeCredentialKind::Oauth,
        CredentialKind::ApiKey => gateway_provider_claude_body::types::ClaudeCredentialKind::ApiKey,
    }
}

pub(super) fn headers(
    binding: &AccountBinding,
    workspace: bool,
) -> Result<worker::Headers, ConstructionFailure> {
    credential_headers(
        &binding.credential,
        if workspace {
            binding.workspace.as_ref().map(|value| &value.id)
        } else {
            None
        },
        false,
    )
}

pub(super) fn credential_headers(
    credential: &claude::Credential,
    workspace: Option<&gateway_source::js_string::JsString16>,
    stream: bool,
) -> Result<worker::Headers, ConstructionFailure> {
    let headers = worker::Headers::new();
    // Projection is late and field-local; workspace must not mask auth setter failure.
    {
        let secret = credential
            .secret
            .try_scalar_secret()
            .map_err(|_| ConstructionFailure)?;
        match credential.kind {
            CredentialKind::Oauth => {
                headers
                    .set("Authorization", &format!("Bearer {}", secret.expose()))
                    .map_err(|_| ConstructionFailure)?;
                headers
                    .set("anthropic-beta", "oauth-2025-04-20")
                    .map_err(|_| ConstructionFailure)?;
            }
            CredentialKind::ApiKey => headers
                .set("x-api-key", secret.expose())
                .map_err(|_| ConstructionFailure)?,
        }
    }
    headers
        .set("Content-Type", "application/json")
        .map_err(|_| ConstructionFailure)?;
    headers
        .set(
            "Accept",
            if stream {
                "text/event-stream"
            } else {
                "application/json"
            },
        )
        .map_err(|_| ConstructionFailure)?;
    headers
        .set("anthropic-version", "2023-06-01")
        .map_err(|_| ConstructionFailure)?;
    if let Some(workspace) = workspace.and_then(trim_workspace) {
        let value = workspace.try_utf8().map_err(|_| ConstructionFailure)?;
        headers
            .set("anthropic-workspace-id", &value)
            .map_err(|_| ConstructionFailure)?;
    }
    Ok(headers)
}

fn trim_workspace(
    value: &gateway_source::js_string::JsString16,
) -> Option<gateway_source::js_string::JsString16> {
    let units = value.as_units();
    let whitespace = |unit: &u16| {
        char::from_u32(u32::from(*unit)).is_some_and(gateway_schema::util::js_whitespace)
    };
    let start = units
        .iter()
        .position(|unit| !whitespace(unit))
        .unwrap_or(units.len());
    let end = units
        .iter()
        .rposition(|unit| !whitespace(unit))
        .map_or(start, |at| at + 1);
    (start < end)
        .then(|| gateway_source::js_string::JsString16::from_units(units[start..end].to_vec()))
}

pub(super) fn physical_request(
    binding: &AccountBinding,
    body: &gateway_provider_claude_body::types::PreparedClaudeBody,
) -> Result<crate::transport::outbound::ConstructedRequest, ConstructionFailure> {
    if body.kind() != credential_kind(&binding.credential) {
        return Err(ConstructionFailure);
    }
    let headers = credential_headers(
        &binding.credential,
        binding.workspace.as_ref().map(|value| &value.id),
        body.stream(),
    )?;
    let target = crate::transport::outbound::PinnedDestination::new(body.url())
        .map_err(|_| ConstructionFailure)?;
    crate::transport::outbound::construct_request(
        &target,
        worker::Method::Post,
        headers,
        body.body().as_bytes(),
    )
    .map_err(|_| ConstructionFailure)
}

impl gateway_claude_usage_runtime::fetch::UsageHost for Host {
    fn parse_usage_date(&self, text: &gateway_source::js_string::JsString16) -> Option<i64> {
        gateway_claude_usage_runtime::platform::parse_date(text)
    }
    async fn accepted_usage(&self, event: &AcceptedObservation) -> Result<(), GatewayError> {
        self.accepted_observation(event).await
    }
    async fn usage_get(
        &self,
        account: &AccountBinding,
    ) -> Result<
        gateway_claude_usage_runtime::fetch::Response,
        gateway_claude_usage_runtime::fetch::GetFailure,
    > {
        gateway_claude_usage_runtime::platform::get(
            account,
            &self.platform.cancellation.source_signal(),
            &|| self.now_ms(),
        )
        .await
    }
}

#[cfg(test)]
mod physical_tests {
    use gateway_source::js_string::JsString16;
    #[test]
    fn actual_prepared_body_kind_rejects_wrong_binding_before_any_host_header()
    -> Result<(), Box<dyn std::error::Error>> {
        use gateway_admin_runtime::claude::{
            AccountBinding, Credential, CredentialKind, source_secret::ClaudeSourceSecret,
        };
        use gateway_provider_claude_body::{
            types::ClaudeBodyOptions, types::ClaudeCredentialKind, types::PreparedClaudeBody,
        };
        use gateway_provider_protocol::types::ClientSurface;
        use gateway_source::source_json::SourceJson;
        let binding = AccountBinding {
            account_id: "fixture-account".into(),
            account_cohort_id: "fixture-cohort".into(),
            credential_fingerprint: "fixture-fingerprint".into(),
            evidence_generation: "fixture-generation".into(),
            credential: Credential {
                kind: CredentialKind::Oauth,
                secret: std::sync::Arc::new(ClaudeSourceSecret::from_source(
                    JsString16::from_units(vec![0xd800]),
                )),
                hint: JsString16::from_utf8("configured"),
                label: None,
                source: "fixture".into(),
                updated_at_ms: None,
            },
            workspace: None,
        };
        let body = PreparedClaudeBody::prepare(
            ClientSurface::Responses,
            &SourceJson::parse(r#"{"model":"claude-fixture","input":"hello"}"#)?,
            &"claude-fixture".into(),
            &ClaudeBodyOptions::new(ClaudeCredentialKind::ApiKey),
        )
        .map_err(|error| error.diagnostic().clone())?;
        // This executes the real closed helper guard; calling host Headers on native would panic.
        assert!(super::physical_request(&binding, &body).is_err());
        Ok(())
    }
    #[test]
    fn workspace_source_trim_retains_lone_units_until_late_field_projection() {
        let source = JsString16::from_units(vec![0xfeff, 32, 0xd800, 32, 0xa0]);
        let trimmed = super::trim_workspace(&source).unwrap();
        assert_eq!(trimmed.as_units(), &[0xd800]);
        assert!(trimmed.try_utf8().is_err());
    }
    #[test]
    fn workspace_source_trim_omits_blank_and_preserves_scalar_data() {
        assert!(super::trim_workspace(&JsString16::from_units(vec![0xfeff, 0xa0, 32])).is_none());
        for value in ["ascii", "é", "�", "中", "😀"] {
            assert_eq!(
                super::trim_workspace(&JsString16::from_utf8(&format!(" {value} ")))
                    .unwrap()
                    .try_utf8()
                    .unwrap(),
                value
            );
        }
    }
}
