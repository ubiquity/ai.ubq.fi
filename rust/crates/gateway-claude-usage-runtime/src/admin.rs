use crate::{
    collect::{self, Refresh},
    fetch::{ResultData, UsageHost},
};
use gateway_auth_contracts::{types::AuthHost, types::AuthResponse};
use gateway_claude_roster::{claude, claude::AccountBinding, claude::Bootstrap};
use gateway_core::state::AuthorityStore;
use gateway_source::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};

pub struct Target {
    pub account_id: JsString16,
    pub cohort_id: Option<JsString16>,
}
/// Passive compatibility DATA; this value carries no administrative authority.
pub struct PassiveRead {
    pub observed_at_ms: Option<f64>,
}
pub async fn passive_read<S: AuthorityStore, H: AuthHost>(
    store: &S,
    host: &H,
    seed: &Bootstrap,
    body: &SourceJson,
) -> Result<PassiveRead, GatewayError> {
    let target = parse(body)?;
    let account = resolve(store, host, seed, &target).await?;
    let state = collect::load(store, &account, host.now_ms()).await?;
    Ok(PassiveRead {
        observed_at_ms: state
            .and_then(|q| q.response_headers)
            .map(|h| h.observed_at_ms),
    })
}
pub fn passive_response(read: PassiveRead, data: SourceJson) -> Result<AuthResponse, GatewayError> {
    AuthResponse::source_json(
        200,
        source::object(vec![
            ("data", data),
            ("refreshed", raw(&false)?),
            ("reason", raw(&"response_headers")?),
            ("observed_at_ms", raw(&read.observed_at_ms)?),
            ("usage", raw(&Option::<u8>::None)?),
            (
                "message",
                raw(&"Quota updates from response headers on ordinary Claude requests.")?,
            ),
        ])?,
    )
}
/// Existing authenticated composition owns access; this owns the complete fenced action.
pub async fn refresh<S: AuthorityStore, H: UsageHost>(
    store: &S,
    host: &H,
    seed: &Bootstrap,
    body: &SourceJson,
) -> Result<Refresh, GatewayError> {
    let target = parse(body)?;
    let account = resolve(store, host, seed, &target).await?;
    let result = collect::refresh(store, host, &account).await?;
    if matches!(result, Refresh::Changed) {
        return Err(changed("Claude account changed during usage refresh"));
    }
    Ok(result)
}
/// Called only after existing administrative authentication; no input is authority.
pub fn parse(body: &SourceJson) -> Result<Target, GatewayError> {
    let account_id = source::member(body, "account_id")
        .as_ref()
        .and_then(source::string)
        .ok_or_else(|| gateway_auth_primitives::error::invalid("account_id is required"))?;
    let cohort_id = source::member(body, "account_cohort_id")
        .map(|v| {
            source::string(&v).ok_or_else(|| {
                gateway_auth_primitives::error::invalid("account_cohort_id must be a string")
            })
        })
        .transpose()?;
    Ok(Target {
        account_id,
        cohort_id,
    })
}
pub async fn resolve<S: AuthorityStore, H: AuthHost>(
    store: &S,
    host: &H,
    seed: &Bootstrap,
    target: &Target,
) -> Result<AccountBinding, GatewayError> {
    let roster = claude::load_roster(store, host, seed).await?;
    let account = roster
        .roster
        .accounts
        .into_iter()
        .find(|a| JsString16::from_utf8(&a.account_id) == target.account_id)
        .ok_or_else(|| GatewayError::new(404, "Claude account was not found", Some("not_found")))?;
    if target
        .cohort_id
        .as_ref()
        .is_some_and(|id| *id != JsString16::from_utf8(&account.account_cohort_id))
    {
        return Err(changed("Claude account generation changed"));
    }
    Ok(account)
}
fn raw<T: serde::Serialize>(v: &T) -> Result<SourceJson, GatewayError> {
    SourceJson::from_serializable(v)
}
/// The composition reads the current admin block after collection and supplies it here.
pub fn response(result: Refresh, data: SourceJson) -> Result<AuthResponse, GatewayError> {
    let mut fields = vec![("data", data)];
    match result {
        Refresh::Changed => return Err(changed("Claude account changed during usage refresh")),
        Refresh::Busy => fields.extend([
            ("refreshed", raw(&false)?),
            ("reason", raw(&"refresh_in_progress")?),
            ("message", raw(&"A usage refresh is already in progress.")?),
        ]),
        Refresh::Fresh(usage) => fields.extend([
            ("refreshed", raw(&false)?),
            ("reason", raw(&"fresh")?),
            ("observed_at_ms", raw(&usage.attempted_at_ms)?),
            ("usage", raw(&usage)?),
        ]),
        Refresh::Completed { result, usage } => {
            match result {
                ResultData::Success { observed_at_ms, .. } => fields.extend([
                    ("refreshed", raw(&true)?),
                    ("source", raw(&"oauth_usage")?),
                    ("observed_at_ms", raw(&observed_at_ms)?),
                    ("usage", raw(&usage)?),
                ]),
                ResultData::Failure(f)
                    if ["scope_missing", "unsupported_credential"].contains(&f.kind) =>
                {
                    fields.extend([
                        ("refreshed", raw(&false)?),
                        ("reason", raw(&f.kind)?),
                        (
                            "message",
                            raw(&f.message.unwrap_or_else(|| {
                                "This credential cannot read Claude usage.".into()
                            }))?,
                        ),
                        ("usage", raw(&usage)?),
                    ])
                }
                ResultData::Failure(f) => {
                    let mut response=AuthResponse::source_json(502,source::object(vec![("error",source::object(vec![("message",raw(&f.message.unwrap_or_else(||"Claude usage could not be read from the upstream service".into()))?),("type",raw(&"invalid_request_error")?),("code",raw(&"claude_usage_unavailable")?)])?)])?);
                    if let Some(retry) = f.retry_after_ms {
                        response.headers.insert(
                            "Retry-After".into(),
                            if retry.is_finite() {
                                gateway_profile_values::coercion::js_string(&serde_json::json!(
                                    (retry / 1000.0).ceil().max(1.0)
                                ))
                            } else {
                                "Infinity".into()
                            },
                        );
                    }
                    return Ok(response);
                }
            }
        }
    }
    Ok(AuthResponse::source_json(200, source::object(fields)?))
}
fn changed(message: &str) -> GatewayError {
    GatewayError::new(409, message, Some("claude_code_account_changed"))
}
