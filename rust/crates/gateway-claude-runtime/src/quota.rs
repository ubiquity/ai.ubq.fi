//! Current-account quota outcome authority; codecs are defined in the data owner.
use gateway_admin_runtime::claude::AccountBinding;
use gateway_claude_usage_quota::{
    AUTH_INVALID_PROOF_TTL_MS, CONFIG_COOLDOWN_MS, Exhaustion, MIN_BLOCK_MS, PoolProof, QuotaError,
    QuotaObservation, QuotaState, RESET_MARGIN_MS, TRANSIENT_COOLDOWN_MS,
};
use gateway_core::{js_string::JsString16, model::ModelId};
use std::collections::BTreeMap;
fn safe_integer(n: f64) -> bool {
    n.is_finite() && n.fract() == 0.0 && n.abs() <= gateway_core::identity::MAX_SAFE_INTEGER as f64
}
pub(crate) struct Outcome<'a> {
    pub model: &'a ModelId,
    pub observed_at_ms: i64,
    pub observation: Option<&'a QuotaObservation>,
    pub exhaustion: Option<&'a Exhaustion>,
    pub auth_invalid: bool,
    pub transient_retry_ms: Option<Option<i64>>,
    pub config_error: Option<QuotaError>,
    pub config_retry_ms: Option<i64>,
    pub served: bool,
}

pub fn applicable_proof<'a>(
    state: Option<&'a QuotaState>,
    binding: &AccountBinding,
    model: &ModelId,
    now: i64,
) -> Option<&'a PoolProof> {
    state?.pool_proofs.as_ref()?.values().find(|p| {
        p.evidence_generation == binding.evidence_generation
            && safe_integer(p.observed_at_ms)
            && p.observed_at_ms > 0.0
            && p.observed_at_ms <= now as f64
            && safe_integer(p.expires_at_ms)
            && p.expires_at_ms > p.observed_at_ms
            && p.expires_at_ms > now as f64
            && (p.kind == "auth_invalid"
                || (p.kind == "quota_exhausted" && p.model_id.as_ref() == Some(model)))
    })
}
pub fn blocked(state: Option<&QuotaState>, now: i64) -> bool {
    state
        .and_then(|s| s.blocked_until_ms)
        .is_some_and(|at| at > now as f64)
}
pub fn cooling_down(state: Option<&QuotaState>, now: i64) -> bool {
    state
        .and_then(|s| s.cooldown_until_ms)
        .is_some_and(|at| at > now as f64)
}
pub(crate) fn merge(
    previous: Option<&QuotaState>,
    binding: &AccountBinding,
    input: &Outcome<'_>,
    now: i64,
) -> QuotaState {
    let empty = QuotaObservation::empty(now);
    let observation = input.observation.unwrap_or(&empty);
    let mut next = previous.cloned().unwrap_or(QuotaState {
        usage: None,
        usage_snapshot: None,
        pool_proofs: None,
        windows: BTreeMap::new(),
        standard: BTreeMap::new(),
        representative_claim: None,
        blocked_until_ms: None,
        block_window: None,
        cooldown_until_ms: None,
        last_error: None,
        updated_at_ms: now as f64,
    });
    merge_observation(&mut next,observation);
    if next.blocked_until_ms.is_some_and(|at| at <= now as f64) {
        next.blocked_until_ms = None;
        next.block_window = None;
    }
    if let Some(exhaustion) = input.exhaustion {
        let candidate = exhaustion
            .reset_at_ms
            .map_or((now + MIN_BLOCK_MS) as f64, |at| {
                at + RESET_MARGIN_MS as f64
            })
            .max((now + MIN_BLOCK_MS) as f64);
        if next.blocked_until_ms.is_none_or(|at| candidate > at) {
            next.blocked_until_ms = Some(candidate);
            next.block_window = Some(exhaustion.window.clone());
        }
    }
    if let Some(retry) = input.transient_retry_ms {
        next.cooldown_until_ms =
            Some((now + retry.filter(|v| *v > 0).unwrap_or(TRANSIENT_COOLDOWN_MS)) as f64);
    }
    if let Some(error) = &input.config_error {
        next.cooldown_until_ms = Some(
            (now + input
                .config_retry_ms
                .filter(|v| *v > 0)
                .unwrap_or(CONFIG_COOLDOWN_MS)) as f64,
        );
        next.last_error = Some(error.clone());
    } else if input.served {
        next.last_error = None;
    }
    let mut proofs = previous
        .and_then(|s| s.pool_proofs.clone())
        .unwrap_or_default();
    proofs.retain(|_, p| {
        p.expires_at_ms > now as f64
            && !(input.served
                && p.observed_at_ms <= input.observed_at_ms as f64
                && (p.kind == "auth_invalid" || p.model_id.as_ref() == Some(input.model)))
    });
    let expiry = if input.auth_invalid {
        Some((input.observed_at_ms + AUTH_INVALID_PROOF_TTL_MS) as f64)
    } else {
        input
            .exhaustion
            .and_then(|e| e.reset_at_ms)
            .filter(|at| *at > input.observed_at_ms as f64)
            .map(|at| at + RESET_MARGIN_MS as f64)
    };
    if !input.model.as_js_string().as_units().is_empty()
        && let Some(expires) = expiry.filter(|at| *at > now as f64)
    {
        let key = if input.auth_invalid {
            JsString16::from_utf8("credential")
        } else {
            JsString16::from_units(
                "model:"
                    .encode_utf16()
                    .chain(input.model.as_js_string().as_units().iter().copied())
                    .collect(),
            )
        };
        if proofs
            .get(&key)
            .is_none_or(|p| p.observed_at_ms <= input.observed_at_ms as f64)
        {
            proofs.insert(
                key,
                PoolProof {
                    kind: if input.auth_invalid {
                        "auth_invalid"
                    } else {
                        "quota_exhausted"
                    }
                    .into(),
                    model_id: (!input.auth_invalid).then(|| input.model.clone()),
                    observed_at_ms: input.observed_at_ms as f64,
                    expires_at_ms: expires,
                    evidence_generation: binding.evidence_generation.clone(),
                },
            );
        }
    }
    next.pool_proofs = Some(proofs);
    next.updated_at_ms = now as f64;
    next
}
pub(crate) fn clear(previous: Option<&QuotaState>, now: i64) -> QuotaState {
    let mut next = previous.cloned().unwrap_or(QuotaState {
        usage: None,
        usage_snapshot: None,
        pool_proofs: None,
        windows: BTreeMap::new(),
        standard: BTreeMap::new(),
        representative_claim: None,
        blocked_until_ms: None,
        block_window: None,
        cooldown_until_ms: None,
        last_error: None,
        updated_at_ms: now as f64,
    });
    next.pool_proofs = Some(BTreeMap::new());
    next.blocked_until_ms = None;
    next.block_window = None;
    next.cooldown_until_ms = None;
    next.last_error = None;
    next.updated_at_ms = now as f64;
    next
}

/// Usage has no model, served outcome, credential-invalidity or exhaustion proof.
/// The same current roster revision and quota revision are checked in the write.
pub async fn record_usage<
    S: gateway_core::state::AuthorityStore,
    H: gateway_auth::types::AuthHost,
>(
    store: &S,
    host: &H,
    binding: &AccountBinding,
    observation: Option<&QuotaObservation>,
    usage: &gateway_claude_usage_protocol::state::UsageState,
) -> Result<Option<QuotaState>, gateway_core::error::GatewayError> {
    use gateway_auth::store::{check, commit};
    use gateway_core::state::RecordMutation;
    let now = usage.attempted_at_ms;
    for _ in 0..9 {
        let Some(current) =
            gateway_admin_runtime::claude::current_binding(store, binding, now).await?
        else {
            return Ok(None);
        };
        let key = crate::persistence::quota_key(binding);
        let row = store.read(&key).await?;
        let previous = row
            .as_ref()
            .map(|r| gateway_claude_usage_quota::parse_record(r, now))
            .transpose()?
            .flatten();
        let mut next = previous.unwrap_or(QuotaState {
            usage: None,
            usage_snapshot: None,
            pool_proofs: None,
            windows: BTreeMap::new(),
            standard: BTreeMap::new(),
            representative_claim: None,
            blocked_until_ms: None,
            block_window: None,
            cooldown_until_ms: None,
            last_error: None,
            updated_at_ms: now as f64,
        });
        if next.blocked_until_ms.is_some_and(|at| at <= now as f64) {
            next.blocked_until_ms = None;
            next.block_window = None;
        }
        if let Some(observation)=observation {merge_observation(&mut next,observation);}
        if next
            .usage
            .as_ref()
            .is_none_or(|old| old.attempted_at_ms <= usage.attempted_at_ms)
        {
            next.usage = Some(usage.clone());
        }
        next.updated_at_ms = now as f64;
        // A bound source outcome records an empty proof map when no proof exists.
        if next.pool_proofs.is_none() { next.pool_proofs=Some(BTreeMap::new()); }
        let revision = gateway_auth::crypto::uuid_from_random(&host.random_bytes(16)?)?;
        let record = gateway_claude_usage_quota::stored_state(&next, revision)?;
        if commit(
            store,
            vec![current, check(&key, row.as_ref())],
            vec![RecordMutation::Put { key, record }],
        )
        .await?
        {
            return Ok(Some(next));
        }
    }
    Ok(None)
}

fn merge_observation(next:&mut QuotaState,observation:&QuotaObservation) {
    for (id, window) in &observation.windows {
        if next
            .windows
            .get(id)
            .is_none_or(|old| old.observed_at_ms <= window.observed_at_ms)
        {
            next.windows.insert(id.clone(), window.clone());
        }
    }
    for (id, window) in &observation.standard {
        if next
            .standard
            .get(id)
            .is_none_or(|old| old.observed_at_ms <= window.observed_at_ms)
        {
            next.standard.insert(id.clone(), window.clone());
        }
    }
    if let Some(snapshot) = &observation.usage_snapshot {
        let at = |s: &gateway_core::source_json::SourceJson| {
            gateway_claude_usage_protocol::snapshot::provenance(s).map(|(_, at)| at)
        };
        if next
            .usage_snapshot
            .as_ref()
            .and_then(at)
            .is_none_or(|old| at(snapshot).is_some_and(|new| new >= old))
        {
            next.usage_snapshot = Some(snapshot.clone());
        }
    }
    if observation.representative_claim.is_some() {
        next.representative_claim = observation.representative_claim.clone();
    }
}
