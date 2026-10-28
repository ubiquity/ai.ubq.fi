use crate::host::CommittedOutcome;
use gateway_auth_primitives::store::{check, commit};
use gateway_claude_policy::{
    host::{ClaudePoolHost, PoolConfig, ResponseObservation, now, revision},
    quota::{self, Outcome, quota_key},
    response::{Classification, ResponseKind},
    selection::same_pool,
};
use gateway_claude_roster::claude::{self, AccountBinding, RosterRead};
use gateway_claude_usage_quota::{QuotaError, QuotaState};
use gateway_contracts::model::ModelId;
use gateway_core::{state::AuthorityStore, state::RecordMutation, state::RevisionCheck};
use gateway_source::error::GatewayError;

pub(crate) async fn load_quota<S: AuthorityStore>(
    store: &S,
    binding: &AccountBinding,
    now: i64,
) -> Result<(Option<QuotaState>, RevisionCheck), GatewayError> {
    let key = quota_key(binding);
    let row = store.read(&key).await?;
    let parsed = row
        .as_ref()
        .map(|row| gateway_claude_usage_quota::parse_record(row, now))
        .transpose()?
        .flatten();
    Ok((parsed, check(&key, row.as_ref())))
}
pub(crate) struct PoolView {
    pub roster: RosterRead,
    pub accounts: Vec<(AccountBinding, Option<QuotaState>)>,
    pub quota_checks: Vec<RevisionCheck>,
    pub fresh: bool,
}
pub(crate) async fn view<S: AuthorityStore, H: ClaudePoolHost>(
    store: &S,
    host: &H,
    config: &PoolConfig,
) -> Result<PoolView, GatewayError> {
    let roster = claude::load_roster(store, host, &config.bootstrap).await?;
    let mut accounts = Vec::new();
    let mut quota_checks = Vec::new();
    for account in &roster.roster.accounts {
        if claude::current_binding(store, account, now(host)?)
            .await?
            .is_none()
        {
            accounts.push((account.clone(), None));
            continue;
        }
        let (state, check) = load_quota(store, account, now(host)?).await?;
        quota_checks.push(check);
        accounts.push((account.clone(), state));
    }
    let current = claude::load_roster(store, host, &config.bootstrap).await?;
    let fresh = same_pool(&roster, &current);
    Ok(PoolView {
        roster: current,
        accounts,
        quota_checks,
        fresh,
    })
}
pub(crate) async fn rotate<S: AuthorityStore, H: ClaudePoolHost>(
    store: &S,
    host: &H,
    view: &PoolView,
    expected: &AccountBinding,
    next_id: &str,
    model: &ModelId,
) -> Result<bool, GatewayError> {
    for _ in 0..12 {
        let Some(mut current) = claude::read_roster(store, now(host)?).await? else {
            return Ok(false);
        };
        if current.roster.active_account_id.as_deref() != Some(expected.account_id.as_str())
            || !current
                .roster
                .accounts
                .iter()
                .any(|a| claude::same_binding(a, expected))
            || !current
                .roster
                .accounts
                .iter()
                .any(|a| a.account_id == next_id)
        {
            return Ok(false);
        }
        if !view
            .accounts
            .iter()
            .find(|(a, _)| a.account_id == expected.account_id)
            .is_some_and(|(a, q)| {
                quota::applicable_proof(q.as_ref(), a, model, host.now_ms()).is_some()
            })
        {
            return Ok(false);
        }
        if current.roster.active_account_id.as_deref() == Some(next_id) {
            return Ok(true);
        }
        let mut checks = view.quota_checks.clone();
        checks.push(RevisionCheck {
            key: claude::account_store_key(),
            expected_revision: current.revision.take(),
        });
        current.roster.active_account_id = Some(next_id.into());
        current.roster.active_reason = "authoritative_unavailability".into();
        current.roster.updated_at_ms = now(host)?;
        if commit(
            store,
            checks,
            vec![RecordMutation::Put {
                key: claude::account_store_key(),
                record: claude::encode_roster_record(&revision(host)?, &current.roster, None)?,
            }],
        )
        .await?
        {
            return Ok(true);
        }
        if !gateway_catalog_inputs::state::checks_current(store, &view.quota_checks).await? {
            return Ok(false);
        }
    }
    Ok(false)
}

pub(crate) async fn persist<S: AuthorityStore, H: ClaudePoolHost>(
    store: &S,
    host: &H,
    binding: &AccountBinding,
    model: &ModelId,
    response: &ResponseObservation,
    classification: &Classification,
    observation: Option<&gateway_claude_usage_quota::QuotaObservation>,
) -> Result<Option<CommittedOutcome>, GatewayError> {
    let now = now(host)?;
    let config = classification.kind.is_configuration();
    if observation.is_none()
        && classification.exhaustion.is_none()
        && !matches!(
            classification.kind,
            ResponseKind::Ok | ResponseKind::AuthInvalid | ResponseKind::TransientRateLimited
        )
        && !config
    {
        return Ok(None);
    }
    for _ in 0..9 {
        let Some(roster_check) = claude::current_binding(store, binding, now).await? else {
            return Ok(None);
        };
        let (previous, quota_check) = load_quota(store, binding, now).await?;
        let input = Outcome {
            model,
            observed_at_ms: response.observed_at_ms,
            observation,
            exhaustion: classification.exhaustion.as_ref(),
            auth_invalid: classification.kind == ResponseKind::AuthInvalid,
            transient_retry_ms: (classification.kind == ResponseKind::TransientRateLimited)
                .then_some(classification.retry_after_ms),
            config_error: config.then(|| QuotaError {
                kind: classification.kind.id().into(),
                status: Some(f64::from(response.status)),
                message: classification.detail.clone(),
                at_ms: now as f64,
            }),
            config_retry_ms: classification.retry_after_ms,
            served: classification.kind == ResponseKind::Ok,
        };
        let mut state = quota::merge(previous.as_ref(), binding, &input, now);
        state.response_headers = gateway_claude_usage_quota::headers::merge(
            state.response_headers.as_ref(),
            observation,
        );
        if commit(
            store,
            vec![roster_check, quota_check],
            vec![RecordMutation::Put {
                key: quota_key(binding),
                record: gateway_claude_usage_quota::stored_state(&state, revision(host)?)?,
            }],
        )
        .await?
        {
            return Ok(Some(CommittedOutcome { state }));
        }
    }
    Ok(None)
}

pub async fn clear_guards<S: AuthorityStore, H: ClaudePoolHost>(
    store: &S,
    host: &H,
    binding: &AccountBinding,
) -> Result<Option<QuotaState>, GatewayError> {
    let now = now(host)?;
    let Some(mut roster) = claude::read_roster(store, now).await? else {
        return Ok(None);
    };
    let Some(account) = roster
        .roster
        .accounts
        .iter_mut()
        .find(|a| claude::same_binding(a, binding))
    else {
        return Ok(None);
    };
    let (previous, quota_check) = load_quota(store, binding, now).await?;
    let cleared = quota::clear(previous.as_ref(), now);
    account.evidence_generation = revision(host)?;
    let roster_key = claude::account_store_key();
    let checks = vec![
        RevisionCheck {
            key: roster_key.clone(),
            expected_revision: roster.revision,
        },
        quota_check,
    ];
    let mutations = vec![
        RecordMutation::Put {
            key: roster_key,
            record: claude::encode_roster_record(&revision(host)?, &roster.roster, None)?,
        },
        RecordMutation::Put {
            key: quota_key(binding),
            record: gateway_claude_usage_quota::stored_state(&cleared, revision(host)?)?,
        },
    ];
    Ok(commit(store, checks, mutations).await?.then_some(cleared))
}
