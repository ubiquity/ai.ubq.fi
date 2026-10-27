//! Checked full-pool settings migration, plan preparation and automatic permit mint.
mod backend;
mod facts;
pub mod types;
use backend::Transaction;
use gateway_auth::provider_credentials::codex_source_pool_key;
use gateway_auth::provider_credentials::parse_codex_source_pool;
use gateway_authority_contracts::{AuthorityClock, TokenSource, unavailable, validate_time};
use gateway_authority_keys as keys;
use gateway_authority_reset_records::full_pool::FullPoolShadowDecision;
use gateway_authority_reset_records::full_pool::SourceResetInventory;
use gateway_authority_reset_records::{ResetConfig, ResetMode, full_pool::CreditCandidateData};
use gateway_authority_reset_records::{full_pool::SourceCreditCollation, full_pool::selection};
use gateway_authority_routing_selection::types::CohortProof;
use gateway_contracts::identity::RequestContext;
use gateway_core::state::AuthorityStore;
use gateway_kv_codec::value::ValueEncoding;
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde_json::{Value, json};
use types::{AutomaticResetPermit, FullPoolEvaluation, FullPoolPreparation, FullPoolResetPlan};
fn invalid() -> GatewayError {
    GatewayError::unavailable(
        "Full-pool reset shadow schema or custody is invalid.",
        "codex_reset_shadow_schema_invalid",
    )
}
fn declined(reason: &str) -> GatewayError {
    GatewayError::unavailable("Automatic banked reset is not authorized.", reason)
}
fn reason(error: GatewayError) -> String {
    error
        .envelope
        .error
        .code
        .unwrap_or_else(|| "configuration_unavailable".into())
}
fn marker_key() -> gateway_kv_codec::key::RecordKey {
    gateway_auth::store::key(json!([
        "uos_ai",
        "migrations",
        "codex_reset_usage_legacy_global_v1"
    ]))
}
fn legacy_key() -> gateway_kv_codec::key::RecordKey {
    gateway_auth::store::key(json!(["uos_ai", "codex_banked_reset_usage", "v1"]))
}
fn opt_out(value: Option<&Value>) -> bool {
    value.is_some_and(|value| *value == false || (!value.is_null() && !value.is_boolean()))
}
async fn read_audit<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    episode_hash: &str,
) -> Result<(bool, Option<FullPoolShadowDecision>), GatewayError> {
    let key = keys::reset_shadow(episode_hash);
    let row = tx.read_full_pool_reset_audit(episode_hash).await?;
    let present = match row.as_ref() {
        None => false,
        Some(row) => match row.value_encoding {
            ValueEncoding::Json => !row.json_value()?.is_null(),
            ValueEncoding::ProfileV2 => {
                SourceJson::from_portable_json(&row.profile_v2_value()?)?.as_str() != "null"
            }
            _ => true,
        },
    };
    let parsed = if present {
        row.as_ref()
            .and_then(|row| FullPoolShadowDecision::validate_record(&key, row).ok())
    } else {
        None
    };
    Ok((present, parsed))
}
pub struct ResetPermitService<'a, S, I> {
    store: &'a S,
    tokens: &'a I,
    clock: Option<&'a dyn AuthorityClock>,
}
impl<'a, S: AuthorityStore, I: TokenSource> ResetPermitService<'a, S, I> {
    pub fn new(store: &'a S, tokens: &'a I, clock: Option<&'a dyn AuthorityClock>) -> Self {
        Self {
            store,
            tokens,
            clock,
        }
    }
    fn transaction(&self, now: i64) -> Result<Transaction<'_, S, I>, GatewayError> {
        Transaction::new(self.store, self.tokens, now)
    }
    fn fresh_time(&self, started_at: i64) -> Result<i64, GatewayError> {
        let now = self
            .clock
            .as_ref()
            .ok_or_else(|| {
                crate::unavailable(
                    "authority_clock_unavailable",
                    "Time-sensitive authority actions require a trusted current clock.",
                )
            })?
            .now_ms()?;
        validate_time(now)?;
        if now < started_at {
            return Err(crate::unavailable(
                "invalid_clock",
                "Authority clock moved backwards.",
            ));
        }
        Ok(now)
    }
    async fn commit_reset_selection(
        &self,
        plan: FullPoolResetPlan,
        selection: selection::Selection,
        decision: FullPoolShadowDecision,
        now: i64,
    ) -> Result<FullPoolEvaluation, GatewayError> {
        let selected = selection.selected.as_ref();
        for _ in 0..4 {
            let at = self.fresh_time(now)?;
            let mut tx = self.transaction(at)?;
            let current =
                match facts::capture(&mut tx, plan.facts.proof(), plan.facts.original()).await {
                    Ok(current) => current,
                    Err(error) => {
                        return Ok(FullPoolEvaluation::Declined {
                            reason: reason(error),
                        });
                    }
                };
            if !plan.facts.same(&current) {
                return Ok(FullPoolEvaluation::Declined {
                    reason: "routing_fence_stale".into(),
                });
            }
            if current.config().mode == ResetMode::Live {
                let selected = selected.ok_or_else(invalid)?;
                let permit = AutomaticResetPermit {
                    candidate: current.members()[selected.member].candidate.clone(),
                    facts: current,
                    decision: decision.clone(),
                    credit: selected.credit.clone(),
                    observed_at_ms: selected.observed,
                };
                tx.now = self.fresh_time(at)?;
                permit.check_deadline(tx.now)?;
                if tx.commit().await? {
                    return Ok(FullPoolEvaluation::Ready(Box::new(permit)));
                }
                continue;
            }
            let key = keys::reset_shadow(current.episode_hash());
            let (present, stored) = read_audit(&mut tx, current.episode_hash()).await?;
            if present || plan.audit_present {
                let Some(stored) = stored.filter(|stored| {
                    stored.episode_hash == decision.episode_hash && stored.expires_at_ms > at
                }) else {
                    return Ok(FullPoolEvaluation::Declined {
                        reason: "shadow_decision_unavailable".into(),
                    });
                };
                tx.now = self.fresh_time(at)?;
                if tx.commit().await? {
                    return Ok(FullPoolEvaluation::Shadow {
                        decision: stored,
                        duplicate: true,
                    });
                }
                continue;
            }
            tx.now = self.fresh_time(at)?;
            if decision.expires_at_ms <= tx.now {
                return Ok(FullPoolEvaluation::Declined {
                    reason: "shadow_decision_expired".into(),
                });
            }
            tx.put(&key, &decision, None)?;
            if tx.commit().await? {
                return Ok(FullPoolEvaluation::Shadow {
                    decision,
                    duplicate: false,
                });
            }
        }
        Ok(FullPoolEvaluation::Declined {
            reason: "shadow_decision_unavailable".into(),
        })
    }
}
pub trait ResetPermitFactory {
    type Store: AuthorityStore;
    type Tokens: TokenSource;
    fn reset_permit(&self) -> ResetPermitService<'_, Self::Store, Self::Tokens>;
}
pub trait ResetPermitOperations: ResetPermitFactory {
    fn prepare_full_pool_reset(
        &self,
        proof: &CohortProof,
        original: &RequestContext,
    ) -> impl std::future::Future<Output = Result<FullPoolPreparation, GatewayError>> {
        async move {
            let context = self.reset_permit();
            let at = context.fresh_time(0)?;
            let mut initial = context.transaction(at)?;
            let config: ResetConfig = initial
                .read(&keys::reset_config())
                .await?
                .unwrap_or_default();
            if let Err(error) = gateway_authority_reset_records::full_pool::policy(&config) {
                return Ok(FullPoolPreparation::Declined {
                    reason: reason(error),
                });
            }
            if !self.migrate_full_pool_reset_settings().await? {
                return Ok(FullPoolPreparation::Declined {
                    reason: "configuration_unavailable".into(),
                });
            }
            for _ in 0..4 {
                let now = context.fresh_time(at)?;
                let mut tx = context.transaction(now)?;
                let facts = match facts::capture(&mut tx, proof, original).await {
                    Ok(facts) => facts,
                    Err(error) => {
                        return Ok(FullPoolPreparation::Declined {
                            reason: reason(error),
                        });
                    }
                };
                let audit_present = if facts.config().mode == ResetMode::Shadow {
                    let (present, prior) = read_audit(&mut tx, facts.episode_hash()).await?;
                    if let Some(decision) = prior.as_ref() {
                        let matches = decision.episode_hash.as_str()
                            == facts.episode_hash().as_str()
                            && decision.fences.as_slice() == facts.fences();
                        if matches && decision.expires_at_ms > now {
                            let decision = decision.clone();
                            tx.now = context.fresh_time(now)?;
                            if decision.expires_at_ms <= tx.now {
                                continue;
                            }
                            if tx.commit().await? {
                                return Ok(FullPoolPreparation::Cached { decision });
                            }
                            continue;
                        }
                    }
                    present
                } else {
                    false
                };
                tx.now = context.fresh_time(now)?;
                if tx.commit().await? {
                    return Ok(FullPoolPreparation::Observe(Box::new(FullPoolResetPlan {
                        facts,
                        audit_present,
                    })));
                }
            }
            Ok(FullPoolPreparation::Declined {
                reason: "routing_fence_stale".into(),
            })
        }
    }
    fn finish_full_pool_reset<H: SourceCreditCollation>(
        &self,
        plan: FullPoolResetPlan,
        inventories: &[SourceResetInventory],
        collation: &H,
    ) -> impl std::future::Future<Output = Result<FullPoolEvaluation, GatewayError>> {
        async move {
            let context = self.reset_permit();
            let now = context.fresh_time(plan.facts.original().started_at_ms)?;
            let selection = match selection::select(
                &plan
                    .facts
                    .members()
                    .iter()
                    .map(|member| CreditCandidateData {
                        binding: member.candidate.binding.clone(),
                        enabled: member.enabled,
                    })
                    .collect::<Vec<_>>(),
                inventories,
                collation,
                now,
            ) {
                Ok(selection) => selection,
                Err(error) => {
                    return Ok(FullPoolEvaluation::Declined {
                        reason: reason(error),
                    });
                }
            };
            let selected = selection.selected.as_ref();
            let expires = plan
                .facts
                .fences()
                .iter()
                .map(|fence| fence.quota_reset_at_ms)
                .chain(selected.and_then(|selected| selected.credit.expires_at_ms))
                .min()
                .ok_or_else(invalid)?;
            if expires <= now {
                return Ok(FullPoolEvaluation::Declined {
                    reason: "shadow_decision_expired".into(),
                });
            }
            let decision = FullPoolShadowDecision {
                v: 1,
                episode_hash: plan.facts.episode_hash().clone(),
                created_at_ms: now,
                expires_at_ms: expires,
                decision_reason: selection.reason.into(),
                selected_account_id_hash: selected.map(|selected| {
                    plan.facts.members()[selected.member]
                        .administrative_hash
                        .clone()
                }),
                selected_credit_id_hash: selected.map(|selected| {
                    gateway_authority_reset_records::full_pool::audit::credit_hash(
                        &selected.credit.credit_id,
                    )
                }),
                selected_credit_expires_at_ms: selected
                    .and_then(|selected| selected.credit.expires_at_ms),
                fences: plan.facts.fences().to_vec(),
            };
            if plan.facts.config().mode == ResetMode::Live && selected.is_none() {
                return Ok(FullPoolEvaluation::Declined {
                    reason: selection.reason.into(),
                });
            }
            context
                .commit_reset_selection(plan, selection, decision, now)
                .await
        }
    }
    /// Existing one-way source reset-setting completion; no new state/key family.
    /// Other source settings readers may reuse this operation rather than duplicate it.
    fn migrate_full_pool_reset_settings(
        &self,
    ) -> impl std::future::Future<Output = Result<bool, GatewayError>> {
        async move {
            let context = self.reset_permit();
            for _ in 0..4 {
                let now = context.fresh_time(0)?;
                let mut tx = context.transaction(now)?;
                let marker = marker_key();
                if tx
                    .read::<Value>(&marker)
                    .await?
                    .is_some_and(|value| !value.is_null())
                {
                    return Ok(true);
                }
                let legacy = tx.read::<Value>(&legacy_key()).await?;
                let Some(raw) = tx.read::<Value>(&codex_source_pool_key()).await? else {
                    return Ok(false);
                };
                let Some(pool) = parse_codex_source_pool(&raw).ok().flatten() else {
                    return Ok(false);
                };
                if pool.accounts.is_empty() {
                    return Ok(false);
                }
                let preserved = opt_out(legacy.as_ref());
                if preserved {
                    for account in pool.accounts {
                        let hash = gateway_auth::crypto::sha256_hex(account.account_id.as_bytes());
                        let key = keys::reset_usage(&hash);
                        if tx
                            .read::<Value>(&key)
                            .await?
                            .is_none_or(|value| value.is_null())
                        {
                            tx.put(&key, &json!({"enabled":false}), None)?;
                        }
                    }
                }
                tx.put(
                    &marker,
                    &json!({"completed_at_ms":now,"preserved_opt_out":preserved}),
                    None,
                )?;
                tx.now = context.fresh_time(now)?;
                if tx.commit().await? {
                    return Ok(true);
                }
            }
            Ok(false)
        }
    }
}
impl<T: ResetPermitFactory> ResetPermitOperations for T {}
