//! Coherent target-scoped campaign records and fixed source target selection.
use crate::cache::ReadCache;
use gateway_cache_experiment_records::{
    model::{Evidence, Outcome, State, TargetBinding, unavailable},
    source::record_key,
};
use gateway_core::state::AuthorityStore;
use gateway_source::error::GatewayError;
use serde_json::Value;
fn parse<T: serde::de::DeserializeOwned>(
    raw: Option<Value>,
    valid: impl FnOnce(&T) -> bool,
    label: &str,
) -> Result<Option<T>, GatewayError> {
    match raw {
        None | Some(Value::Null) => Ok(None),
        Some(value) => {
            let value = serde_json::from_value(value).map_err(|_| unavailable(label))?;
            if !valid(&value) {
                return Err(unavailable(label));
            }
            Ok(Some(value))
        }
    }
}
pub(crate) async fn records<S: AuthorityStore>(
    tx: &mut ReadCache<'_, S>,
    target: &TargetBinding,
) -> Result<(Option<State>, Option<Evidence>), GatewayError> {
    let state = tx.read::<Value>(&record_key("state", target)).await?;
    let evidence = tx.read::<Value>(&record_key("evidence", target)).await?;
    let state = parse(
        state,
        |s: &State| s.valid(tx.now),
        "Prompt-cache scope experiment has malformed target-scoped state.",
    )?;
    let evidence = parse(
        evidence,
        |e: &Evidence| e.valid(tx.now),
        "Prompt-cache scope experiment has malformed target-scoped evidence.",
    )?;
    if let Some(state) = &state {
        let coherent = if state.classifications.is_empty() {
            evidence.is_none()
        } else {
            evidence.as_ref().is_some_and(|e| e.consistent_with(state))
        };
        if !coherent {
            return Err(unavailable(
                "Prompt-cache scope experiment has inconsistent target-scoped durable evidence.",
            ));
        }
    }
    Ok((state, evidence))
}
pub(crate) async fn select_target<S: AuthorityStore>(
    tx: &mut ReadCache<'_, S>,
    targets: Vec<TargetBinding>,
) -> Result<TargetBinding, GatewayError> {
    if targets.is_empty() {
        return Err(unavailable(
            "Prompt-cache scope experiment has no probeable Codex target in the current inventory.",
        ));
    }
    let mut all = Vec::new();
    for target in targets {
        let (state, evidence) = records(tx, &target).await?;
        all.push((target, state, evidence));
    }
    let active = all
        .iter()
        .filter(|(_, s, _)| s.as_ref().is_some_and(|s| s.expires_at_ms > tx.now))
        .collect::<Vec<_>>();
    if active.len() > 1 {
        return Err(unavailable(
            "Prompt-cache scope experiment found more than one active provider campaign.",
        ));
    }
    if let Some((target, Some(state), _)) = active.first().copied() {
        if !state.target.same_core(target) {
            return Err(unavailable(
                "Prompt-cache scope experiment target changed while its campaign was active.",
            ));
        }
        if state.target.inventory_fingerprint != target.inventory_fingerprint {
            return Err(unavailable(
                "Prompt-cache scope experiment inventory changed while its campaign was active.",
            ));
        }
        return Ok(target.clone());
    }
    for (target, _, evidence) in all {
        if !evidence
            .is_some_and(|e| e.outcome == Outcome::Completed && e.target.same_definition(&target))
        {
            return Ok(target);
        }
    }
    Err(unavailable(
        "Prompt-cache scope experiment has no nonterminal probeable Codex target in the current inventory.",
    ))
}
