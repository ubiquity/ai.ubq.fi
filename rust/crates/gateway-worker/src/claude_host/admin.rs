//! Already-authorized finite admin operations; account/quota policy stays in m12/m21.
use crate::{
    host::{PlatformHost, response_bytes},
    transport::outbound::{PinnedDestination, construct_request, send_constructed},
};
use gateway_admin_runtime_contracts::capabilities::{ClaudeRequest, ClaudeSubmission, Context};
use gateway_auth_contracts::types::{AuthHost, AuthResponse};
use gateway_claude_roster::claude::{self, AccountBinding, AccountUpdate, Credential};
use gateway_claude_usage_admin::{
    check_target_cohort, matching_target, readback::subscription_block, select_source_account,
    submitted_workspace,
};
use gateway_observability::{ObservabilityRuntime, types::ObservabilityHost};
use gateway_source::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use gateway_worker_authority_binding::authority::RemoteAuthorityStore;
use gateway_worker_http_body::body::discard_response;
use gateway_worker_platform::cancellation::Cancellation;
use serde::Serialize;
use std::collections::BTreeSet;

const UNVALIDATED: &str = "Stored unvalidated: the models listing could not be read.";
struct Validation {
    status: Option<u16>,
    detail: Option<&'static str>,
    snapshot: Option<(Vec<SourceJson>, i64)>,
}
impl Validation {
    fn unavailable(status: Option<u16>) -> Self {
        Self {
            status,
            detail: (status != Some(429)).then_some(UNVALIDATED),
            snapshot: None,
        }
    }
}

pub async fn admin<H: ObservabilityHost>(
    store: &RemoteAuthorityStore,
    platform: &PlatformHost,
    observations: &ObservabilityRuntime<RemoteAuthorityStore, H>,
    request: ClaudeRequest,
    context: Context,
) -> Result<AuthResponse, GatewayError> {
    match request {
        ClaudeRequest::RefreshUsage { document } => {
            let seed = super::platform::bootstrap_for_source(store, platform).await?;
            let host = super::platform::Host {
                store: store.clone(),
                platform: platform.clone(),
            };
            let result =
                gateway_claude_usage_runtime::admin::refresh(store, &host, &seed, &document)
                    .await?;
            gateway_claude_usage_runtime::admin::response(
                result,
                block(observations, context).await?,
            )
        }

        ClaudeRequest::State => response(observations, context).await,
        ClaudeRequest::SelectAccount { account_id } => {
            if !select_source_account(store, platform, &account_id).await? {
                return Err(not_found());
            }
            response(observations, context).await
        }
        ClaudeRequest::DeleteAccount { account_id } => {
            let account = target(store, platform, account_id.as_ref()).await?;
            let Some(account) = account else {
                return if account_id
                    .as_ref()
                    .is_some_and(|id| !id.as_units().is_empty())
                {
                    Err(not_found())
                } else {
                    response(observations, context).await
                };
            };
            let _removed = claude::remove_account(
                store,
                platform,
                &claude::Bootstrap::default(),
                Some(&account.account_id),
            )
            .await?;
            // This source endpoint directly removes an account; it is not the legacy clear wrapper.
            response(observations, context).await
        }
        ClaudeRequest::ClearQuota {
            account_id,
            cohort_id,
        } => {
            let account = target(store, platform, account_id.as_ref()).await?;
            let Some(account) = account else {
                if account_id
                    .as_ref()
                    .is_some_and(|id| !id.as_units().is_empty())
                {
                    return Err(not_found());
                }
                return cleared_response(observations, context, false).await;
            };
            check_target_cohort(
                account_id
                    .as_ref()
                    .is_some_and(|id| !id.as_units().is_empty()),
                cohort_id.as_ref(),
                &account.account_cohort_id,
            )?;
            let host = super::platform::Host {
                store: store.clone(),
                platform: platform.clone(),
            };
            if gateway_claude_runtime::persistence::clear_guards(store, &host, &account)
                .await?
                .is_none()
            {
                return Err(changed("Claude guards changed during clear"));
            }
            cleared_response(observations, context, true).await
        }
        ClaudeRequest::SetCredential(input) => {
            submit(store, platform, observations, input, context).await
        }
    }
}

async fn target(
    store: &RemoteAuthorityStore,
    platform: &PlatformHost,
    id: Option<&JsString16>,
) -> Result<Option<AccountBinding>, GatewayError> {
    let seed = super::platform::bootstrap_for_source(store, platform).await?;
    let roster = claude::load_roster(store, platform, &seed).await?;
    Ok(matching_target(&roster.roster, id).cloned())
}

async fn submit<H: ObservabilityHost>(
    store: &RemoteAuthorityStore,
    platform: &PlatformHost,
    observations: &ObservabilityRuntime<RemoteAuthorityStore, H>,
    input: ClaudeSubmission,
    context: Context,
) -> Result<AuthResponse, GatewayError> {
    let source_account_id = input.account_id.as_deref().map(JsString16::from_utf8);
    let old = if input.create_new {
        None
    } else {
        target(store, platform, source_account_id.as_ref())
            .await
            .map_err(|error| {
                gateway_auth_primitives::error::invalid(&error.envelope.error.message)
            })?
    };
    if input.account_id.is_some() && old.is_none() {
        return Err(gateway_auth_primitives::error::invalid(
            "Claude account was not found",
        ));
    }
    let secret = input
        .credential
        .as_ref()
        .or_else(|| old.as_ref().map(|account| &account.credential.secret))
        .ok_or_else(|| {
            GatewayError::new(
                409,
                "No Claude credential is configured to validate this workspace against",
                Some("credential_missing"),
            )
        })?;
    let mut candidate = claude::submitted_credential(
        std::sync::Arc::clone(secret),
        input.label.clone(),
        platform.now_ms(),
    )
    .ok_or_else(|| gateway_auth_primitives::error::invalid("credential is invalid"))?;
    let workspace_id = match &input.workspace_id {
        Some(id) => id.as_ref(),
        None => old
            .as_ref()
            .and_then(|account| account.workspace.as_ref())
            .map(|workspace| &workspace.id),
    };
    let validation = validate(platform, &candidate, workspace_id).await?;
    if validation.status == Some(401) || validation.status == Some(403) {
        return Err(GatewayError::new(
            400,
            if validation.status == Some(401) {
                "The credential was rejected as unauthenticated."
            } else {
                "The credential is not permitted for this endpoint."
            },
            Some("invalid_api_key"),
        ));
    }
    candidate.updated_at_ms = Some(platform.now_ms());
    let workspace = workspace_id.map(|id| {
        submitted_workspace(
            old.as_ref().and_then(|account| account.workspace.as_ref()),
            id,
            input.workspace_id.is_some(),
            validation.status,
            platform.now_ms(),
        )
    });
    let account = claude::update_account(
        store,
        platform,
        &claude::Bootstrap::default(),
        &AccountUpdate {
            account_id: old.as_ref().map(|account| account.account_id.clone()),
            create_new: input.create_new,
            credential: input.credential.as_ref().map(|_| candidate.clone()),
            workspace: Some(workspace),
            activate: input.activate,
            expected: old,
        },
    )
    .await
    .map_err(|error| changed(&error.envelope.error.message))?;
    if let Some((models, at)) = validation.snapshot {
        let host = super::platform::Host {
            store: store.clone(),
            platform: platform.clone(),
        };
        // A changed source binding merely declines this optional cache publication.
        let _cached = super::catalog::prime(&host, &account, models, at).await;
    }
    // These are direct account operations, not source legacy generation-advance events.
    let block = block(observations, context).await?;
    #[derive(Serialize)]
    struct ValidationView {
        status: Option<u16>,
        detail: Option<&'static str>,
    }
    #[derive(Serialize)]
    struct Body<'a> {
        data: &'a SourceJson,
        account_id: &'a str,
        validation: ValidationView,
        hint: Option<&'a JsString16>,
    }
    Ok(AuthResponse::source_json(
        200,
        SourceJson::from_serializable(&Body {
            data: &block,
            account_id: &account.account_id,
            validation: ValidationView {
                status: validation.status,
                detail: validation.detail,
            },
            hint: input.credential.as_ref().map(|_| &candidate.hint),
        })?,
    ))
}

async fn validate(
    platform: &PlatformHost,
    credential: &Credential,
    workspace: Option<&JsString16>,
) -> Result<Validation, GatewayError> {
    let cancellation = Cancellation::background()?;
    let _deadline = cancellation.set_timeout_ms(8_000)?;
    let mut models = Vec::<SourceJson>::new();
    let mut cursors = BTreeSet::new();
    let mut cursor: Option<JsString16> = None;
    for _ in 0..20 {
        let mut url = worker::Url::parse("https://api.anthropic.com/v1/models")
            .map_err(|_| super::invalid("Claude model URL is invalid."))?;
        url.query_pairs_mut().append_pair("limit", "100");
        if let Some(cursor) = &cursor {
            super::catalog::append_cursor(&mut url, cursor);
        }
        let destination = PinnedDestination::new(url.as_str())?;
        let headers = match super::platform::credential_headers(credential, workspace, false) {
            Ok(headers) => headers,
            Err(_) => return Ok(Validation::unavailable(None)),
        };
        let request = match construct_request(&destination, worker::Method::Get, headers, &[]) {
            Ok(request) => request,
            Err(_) => return Ok(Validation::unavailable(None)),
        };
        let mut response = match send_constructed(request, &cancellation, || async { Ok(()) }).await
        {
            Ok(response) => response,
            Err(_) => return Ok(Validation::unavailable(None)),
        };
        let status = response.status_code();
        if !(200..300).contains(&status) {
            discard_response(&mut response);
            return Ok(Validation::unavailable(Some(status)));
        }
        let bytes = match response_bytes(&mut response, 2 * 1024 * 1024).await {
            Ok(bytes) => bytes,
            Err(_) => return Ok(Validation::unavailable(Some(status))),
        };
        let parsed = match SourceJson::parse(&String::from_utf8_lossy(&bytes)) {
            Ok(value) => value,
            Err(_) => return Ok(Validation::unavailable(Some(status))),
        };
        let (page, next) = match gateway_catalog_provider::providers::parse_page(
            gateway_catalog_provider::host::CatalogProvider::ClaudeCode,
            &parsed,
        ) {
            Ok(page) => page,
            Err(_) => return Ok(Validation::unavailable(Some(status))),
        };
        for model in page {
            let id = source::member(&model, "id")
                .as_ref()
                .and_then(source::string);
            if let Some(old) = models
                .iter_mut()
                .find(|old| source::member(old, "id").as_ref().and_then(source::string) == id)
            {
                *old = model;
            } else {
                models.push(model);
            }
        }
        match next {
            None => {
                return Ok(Validation {
                    status: Some(200),
                    detail: None,
                    snapshot: Some((models, platform.now_ms())),
                });
            }
            Some(next) => {
                if !cursors.insert(next.clone()) {
                    return Ok(Validation::unavailable(Some(status)));
                }
                cursor = Some(next);
            }
        }
    }
    Ok(Validation::unavailable(None))
}

async fn block<H: ObservabilityHost>(
    observations: &ObservabilityRuntime<RemoteAuthorityStore, H>,
    context: Context,
) -> Result<SourceJson, GatewayError> {
    let response = observations
        .passive_provider_response(true, context)
        .await?;
    subscription_block(response.body)
}
async fn response<H: ObservabilityHost>(
    observations: &ObservabilityRuntime<RemoteAuthorityStore, H>,
    context: Context,
) -> Result<AuthResponse, GatewayError> {
    #[derive(Serialize)]
    struct Body {
        data: SourceJson,
    }
    Ok(AuthResponse::source_json(
        200,
        SourceJson::from_serializable(&Body {
            data: block(observations, context).await?,
        })?,
    ))
}
async fn cleared_response<H: ObservabilityHost>(
    observations: &ObservabilityRuntime<RemoteAuthorityStore, H>,
    context: Context,
    cleared: bool,
) -> Result<AuthResponse, GatewayError> {
    #[derive(Serialize)]
    struct Body {
        data: SourceJson,
        guards_cleared: bool,
    }
    Ok(AuthResponse::source_json(
        200,
        SourceJson::from_serializable(&Body {
            data: block(observations, context).await?,
            guards_cleared: cleared,
        })?,
    ))
}
fn not_found() -> GatewayError {
    GatewayError::new(404, "Claude account was not found", Some("not_found"))
}
fn changed(message: &str) -> GatewayError {
    GatewayError::new(409, message, Some("claude_code_account_changed"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn limited_validation_has_no_unvalidated_warning_and_source_errors_are_exact() {
        assert_eq!(Validation::unavailable(Some(429)).detail, None);
        assert_eq!(Validation::unavailable(None).detail, Some(UNVALIDATED));
        assert_eq!(Validation::unavailable(Some(502)).status, Some(502));
        assert_eq!(not_found().status, 404);
        let stale = changed("Claude account generation changed");
        assert_eq!(stale.status, 409);
        assert_eq!(
            stale.envelope.error.code.as_deref(),
            Some("claude_code_account_changed")
        );
    }
}
