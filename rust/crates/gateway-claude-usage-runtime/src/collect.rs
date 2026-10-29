use crate::fetch::{self, ResultData, UsageHost};
use gateway_auth_primitives::{crypto::uuid_from_random, store::check, store::commit};
use gateway_claude_roster::{claude, claude::AccountBinding};
use gateway_claude_usage_protocol::state::UsageState;
use gateway_claude_usage_quota::QuotaState;
use gateway_core::state::{AuthorityStore, RecordMutation, StoredRecord};
use gateway_kv_codec::key::RecordKey;
use gateway_kv_codec::tuple_key::{ScalarKeyPart, TupleKey};
use gateway_source::error::GatewayError;
use serde::{Deserialize, Serialize};

#[derive(Clone, Serialize, Deserialize)]
struct Lease {
    owner: String,
    until_ms: i64,
}
pub enum Refresh {
    Changed,
    Busy,
    Fresh(UsageState),
    Completed {
        result: ResultData,
        usage: UsageState,
    },
}
pub fn lease_key(account: &AccountBinding) -> RecordKey {
    TupleKey::from_scalar_parts(&[
        ScalarKeyPart::String("uos_ai"),
        ScalarKeyPart::String("claude_code_usage"),
        ScalarKeyPart::String("v1"),
        ScalarKeyPart::String("lease"),
        ScalarKeyPart::String(&account.account_id),
        ScalarKeyPart::String(&account.account_cohort_id),
    ])
    .to_record_key()
}
pub async fn load<S: AuthorityStore>(
    store: &S,
    account: &AccountBinding,
    now: i64,
) -> Result<Option<QuotaState>, GatewayError> {
    if claude::current_binding(store, account, now)
        .await?
        .is_none()
    {
        return Ok(None);
    }
    store
        .read(&gateway_claude_policy::quota::quota_key(account))
        .await?
        .as_ref()
        .map(|r| gateway_claude_usage_quota::parse_record(r, now))
        .transpose()
        .map(Option::flatten)
}
pub async fn refresh<S: AuthorityStore, H: UsageHost>(
    store: &S,
    host: &H,
    account: &AccountBinding,
) -> Result<Refresh, GatewayError> {
    let now = host.now_ms();
    let Some(current) = claude::current_binding(store, account, now).await? else {
        return Ok(Refresh::Changed);
    };
    if let Some(usage) = load(store, account, now)
        .await?
        .and_then(|q| q.usage)
        .filter(|u| u.fresh(now))
    {
        return Ok(Refresh::Fresh(usage));
    }
    let key = lease_key(account);
    let row = store.read(&key).await?;
    let lease = gateway_auth_primitives::store::decode::<Lease>(row.as_ref(), now)?;
    if lease.is_some_and(|v| v.until_ms > now) {
        return Ok(Refresh::Busy);
    }
    let owner = uuid_from_random(&host.random_bytes(16)?)?;
    let lease = Lease {
        owner: owner.clone(),
        until_ms: now + 10_000,
    };
    let record = StoredRecord::json(
        owner.clone(),
        serde_json::to_value(&lease).map_err(|_| unavailable())?,
        Some(now + 20_000),
    );
    if !commit(
        store,
        vec![current, check(&key, row.as_ref())],
        vec![RecordMutation::Put {
            key: key.clone(),
            record,
        }],
    )
    .await?
    {
        return Ok(Refresh::Busy);
    }
    let result = collect_claimed(store, host, account).await;
    let cleanup = async {
        let row = store.read(&key).await?;
        if gateway_auth_primitives::store::decode::<Lease>(row.as_ref(), host.now_ms())?
            .is_some_and(|l| l.owner == owner)
        {
            let _ = commit(
                store,
                vec![check(&key, row.as_ref())],
                vec![RecordMutation::Delete { key }],
            )
            .await?;
        }
        Ok::<(), GatewayError>(())
    }
    .await;
    cleanup?;
    result
}
fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Claude usage lease is unavailable",
        "claude_usage_unavailable",
    )
}

async fn collect_claimed<S: AuthorityStore, H: UsageHost>(
    store: &S,
    host: &H,
    account: &AccountBinding,
) -> Result<Refresh, GatewayError> {
    if let Some(usage) = load(store, account, host.now_ms())
        .await?
        .and_then(|q| q.usage)
        .filter(|u| u.fresh(host.now_ms()))
    {
        return Ok(Refresh::Fresh(usage));
    }
    if claude::current_binding(store, account, host.now_ms())
        .await?
        .is_none()
    {
        return Ok(Refresh::Changed);
    }
    let result = fetch::fetch(host, account).await?;
    let usage = result.usage();
    let Some(state) = gateway_claude_policy::quota::record_usage(
        store,
        host,
        account,
        result.observation(),
        &usage,
    )
    .await?
    else {
        return Ok(Refresh::Changed);
    };
    if let Some(observation) = result.observation() {
        let event = gateway_claude_usage_quota::observation::AcceptedObservation {
            source: None,
            v: 1,
            observation_id: uuid_from_random(&host.random_bytes(16)?)?,
            account_id: account.account_id.clone(),
            account_cohort_id: account.account_cohort_id.clone(),
            credential_kind: match account.credential.kind {
                claude::CredentialKind::Oauth => "oauth",
                claude::CredentialKind::ApiKey => "api_key",
            }
            .into(),
            workspace_id: account.workspace.as_ref().map(|w| w.id.clone()),
            source_observed_at_ms: usage.observed_at_ms,
            sampled_at_ms: usage.attempted_at_ms,
            response_status: usage.status,
            state: if gateway_claude_policy::quota::blocked(Some(&state), usage.attempted_at_ms)
                || gateway_claude_policy::quota::cooling_down(Some(&state), usage.attempted_at_ms)
            {
                "unavailable"
            } else {
                "available"
            }
            .into(),
            observation: observation.clone(),
        };
        // Source post-acceptance telemetry failure cannot revoke accepted quota.
        let _ = host.accepted_usage(&event).await;
    }
    Ok(Refresh::Completed { result, usage })
}
