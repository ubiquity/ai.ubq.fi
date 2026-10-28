//! Actual source2 original-request action3 preparation; only complete same-store CAS mints the opaque action.
mod backend;
use backend::Transaction;
use gateway_auth_contracts::types::{AuthConfig, AuthHost};
use gateway_auth_native_proof::native_auth::NativeTransportIdentity;
use gateway_auth_native_wire::control::NATIVE_RENEWAL_MAX_MS;
use gateway_auth_passport_contracts::session::PassportDate;
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_authority_actor_currentness::{
    app::ActorService,
    source_actor::{SourceActorPreparationData, SourceNativeRenewalActor},
};
use gateway_authority_actor_records::{
    scope::NativeActorScope, source::StoredNativeActorV2, versions,
};
use gateway_authority_contracts::{
    AuthorityClock, NativeRenewalCancellation, TokenSource, add, validate_time,
};
use gateway_authority_native_context_facts::{
    SourceNativeContextFactsData, SourceNativeContextFactsReader,
};
use gateway_authority_native_records::{
    live::mapping_key,
    owner::RegisteredRenewalOwner,
    rows::{self, conflict, fail},
    source_rows::SourceRenewalActionRowV3,
};
use gateway_authority_routing_selection::types::LiveAccountSelection;
use gateway_cache_experiment_dispatch::RefreshPermit;
use gateway_contracts::identity::{AccountBinding, RequestContext};
use gateway_core::state::AuthorityStore;
use gateway_native_authority_wire::{
    context::NativeRenewalContext, source_fence::SourceNativeRenewalFenceV3,
};
use gateway_source::{error::GatewayError, source_json::SourceJson};
/// Trusted constructor capabilities only; no request selects registry, identity or cancellation.
pub struct SourceNativePreparationContext<'a, C> {
    identity: &'a NativeTransportIdentity,
    registered: &'a RegisteredRenewalOwner,
    cancellation: &'a C,
}
impl<'a, C> SourceNativePreparationContext<'a, C> {
    pub fn configured(
        identity: &'a NativeTransportIdentity,
        registered: &'a RegisteredRenewalOwner,
        cancellation: &'a C,
    ) -> Self {
        Self {
            identity,
            registered,
            cancellation,
        }
    }
}
/// Neither deserialized DATA nor an earlier authorization-only CAS mints this action.
/// ```compile_fail
/// use gateway_authority_native_preparation::SourceNativeRenewalAction;
/// let _:SourceNativeRenewalAction=serde_json::from_str("{}").unwrap();
/// ```
pub struct SourceNativeRenewalAction {
    action_ref: String,
    deadline_ms: i64,
    fence: SourceNativeRenewalFenceV3,
}
impl SourceNativeRenewalAction {
    pub fn action_ref(&self) -> &str {
        &self.action_ref
    }
    pub fn deadline_ms(&self) -> i64 {
        self.deadline_ms
    }
    pub fn fence(&self) -> &SourceNativeRenewalFenceV3 {
        &self.fence
    }
}
pub struct SourceNativeRenewalService<'a, S, I, H, C> {
    store: &'a S,
    tokens: &'a I,
    clock: Option<&'a dyn AuthorityClock>,
    actors: ActorService<'a, S, H>,
    context: SourceNativePreparationContext<'a, C>,
}
impl<
    'a,
    S: AuthorityStore,
    I: TokenSource,
    H: AuthHost + PassportDate,
    C: NativeRenewalCancellation,
> SourceNativeRenewalService<'a, S, I, H, C>
{
    pub fn new(
        store: &'a S,
        tokens: &'a I,
        clock: Option<&'a dyn AuthorityClock>,
        host: &'a H,
        config: &'a AuthConfig,
        context: SourceNativePreparationContext<'a, C>,
    ) -> Self {
        Self {
            store,
            tokens,
            clock,
            actors: ActorService::new(store, host, config),
            context,
        }
    }
    fn transaction(&self, now: i64) -> Result<Transaction<'_, S, I>, GatewayError> {
        Transaction::new(self.store, self.tokens, now)
    }
    pub async fn prepare_source_live_create_renewal(
        &self,
        actor: &SourceNativeRenewalActor<'_, S>,
        selected: &LiveAccountSelection,
    ) -> Result<SourceNativeRenewalAction, GatewayError> {
        self.prepare_context(
            actor,
            NativeRenewalContext::LiveCreate {
                binding: selected.binding.clone(),
                active_generation: selected.active_generation,
                model_utf16: selected
                    .model
                    .as_ref()
                    .map(|value| value.as_units().to_vec()),
            },
            None,
        )
        .await
    }
    pub async fn prepare_source_live_mapped_renewal(
        &self,
        actor: &SourceNativeRenewalActor<'_, S>,
        call_id: &str,
    ) -> Result<SourceNativeRenewalAction, GatewayError> {
        if call_id.is_empty() || call_id.len() > 4096 {
            return Err(fail());
        }
        let phase = self
            .actors
            .prepare_source_phase(actor, NativeActorScope::Client, self.time(0)?)
            .await?;
        let mut tx = self.transaction(self.time(0)?)?;
        hydrate(&mut tx, phase.current_data().revision_checks()).await?;
        let key = mapping_key(call_id, false);
        let _: SourceJson = tx.read_source_control(&key).await?.ok_or_else(fail)?;
        let revision = tx.revision(&key).ok_or_else(fail)?;
        self.lifetime(
            phase.actor_data(),
            phase.current_data().minimum_expiry(),
            self.time(tx.now)?,
        )?;
        if !tx.commit().await? {
            return Err(conflict());
        }
        self.prepare_context(
            actor,
            NativeRenewalContext::LiveMapped {
                call_id: call_id.into(),
                mapping_revision: revision,
            },
            None,
        )
        .await
    }
    pub async fn prepare_source_cache_experiment_renewal(
        &self,
        actor: &SourceNativeRenewalActor<'_, S>,
        permit: &RefreshPermit,
    ) -> Result<SourceNativeRenewalAction, GatewayError> {
        self.prepare_context(
            actor,
            NativeRenewalContext::CacheExperiment {
                context: Box::new(permit.context().clone()),
            },
            Some(permit.binding()),
        )
        .await
    }
    pub async fn prepare_source_inference_renewal(
        &self,
        actor: &SourceNativeRenewalActor<'_, S>,
    ) -> Result<SourceNativeRenewalAction, GatewayError> {
        self.prepare_context_inner(actor, None, None).await
    }
    async fn prepare_context(
        &self,
        actor: &SourceNativeRenewalActor<'_, S>,
        context: NativeRenewalContext,
        expected: Option<&AccountBinding>,
    ) -> Result<SourceNativeRenewalAction, GatewayError> {
        self.prepare_context_inner(actor, Some(context), expected)
            .await
    }
    async fn prepare_context_inner(
        &self,
        actor: &SourceNativeRenewalActor<'_, S>,
        context: Option<NativeRenewalContext>,
        expected: Option<&AccountBinding>,
    ) -> Result<SourceNativeRenewalAction, GatewayError> {
        let scope = if matches!(context, Some(NativeRenewalContext::CacheExperiment { .. })) {
            NativeActorScope::Admin
        } else {
            NativeActorScope::Client
        };
        if actor.scope() != scope {
            return Err(fail());
        }
        for _ in 0..5 {
            let phase = self
                .actors
                .prepare_source_phase(actor, scope, self.time(0)?)
                .await?;
            let now = self.time(0)?;
            let mut tx = self.transaction(now)?;
            let facts = self.read_facts(&mut tx, &phase, context.as_ref()).await?;
            let resolved_context = facts.fence().context.clone();
            if expected.is_some_and(|binding| {
                binding.slot != 0
                    || binding.account_id_hash.0 != facts.fence().account_id_hash
                    || binding.credential_version.0 != facts.fence().credential_version
                    || binding.pool_versionstamp.0 != facts.fence().source_revision
            }) {
                return Err(fail());
            }
            if facts.fence().principal_policy_digest != phase.current_data().policy_digest() {
                return Err(fail());
            }
            let reference = action_reference(
                phase.context(),
                phase.current_data().policy_digest(),
                &resolved_context,
                &facts.fence().source_native_generation,
            )?;
            let key = rows::key("action", &reference);
            let prior: Option<SourceJson> = tx.read_source_control(&key).await?;
            let row = if let Some(raw) = prior {
                let row = SourceRenewalActionRowV3::from_source_json(&raw)?;
                if row.actor != *phase.actor_data()
                    || row.context != *phase.context()
                    || row.fence != *facts.fence()
                    || row.checks_digest != facts.checks_digest()
                {
                    return Err(conflict());
                }
                row
            } else {
                let row = SourceRenewalActionRowV3 {
                    v: versions::RENEWAL_ACTION_V3,
                    action_ref: reference.clone(),
                    actor: phase.actor_data().clone(),
                    context: phase.context().clone(),
                    created_at_ms: now,
                    deadline_ms: add(now, NATIVE_RENEWAL_MAX_MS)?,
                    fence: facts.fence().clone(),
                    checks_digest: facts.checks_digest().into(),
                    control_checks_digest: facts.control_checks_digest().into(),
                    account_digests: facts.account_digests().to_vec(),
                    operation_id: None,
                };
                tx.put_source_control(&key, &row.to_source_json()?, None)?;
                row
            };
            tx.now = self.time(now)?;
            self.require_row_time(&row, tx.now)?;
            let current = self
                .read_facts(&mut tx, &phase, Some(&row.fence.context))
                .await?;
            if current.fence() != &row.fence
                || current.checks_digest() != row.checks_digest
                || current.control_checks_digest() != row.control_checks_digest
                || current.account_digests() != row.account_digests
            {
                return Err(fail());
            }
            self.lifetime(
                &row.actor,
                phase.current_data().minimum_expiry(),
                self.time(tx.now)?,
            )?;
            self.require_row_time(&row, self.time(tx.now)?)?;
            if tx.commit().await? {
                return Ok(SourceNativeRenewalAction {
                    action_ref: reference,
                    deadline_ms: row.deadline_ms,
                    fence: row.fence,
                });
            }
        }
        Err(conflict())
    }
    async fn read_facts(
        &self,
        tx: &mut Transaction<'_, S, I>,
        phase: &SourceActorPreparationData,
        context: Option<&NativeRenewalContext>,
    ) -> Result<SourceNativeContextFactsData, GatewayError> {
        let now = tx.now;
        let data = {
            let snapshot = tx.routing_read_store();
            let reader = SourceNativeContextFactsReader::new(
                &snapshot,
                self.context.identity,
                self.context.registered,
                self.clock,
                self.context.cancellation,
                now,
            )?;
            match context {
                Some(context) => {
                    reader
                        .read_context(
                            phase.actor_data(),
                            phase.context(),
                            phase.current_data(),
                            context,
                        )
                        .await?
                }
                None => {
                    reader
                        .read_inference(phase.actor_data(), phase.context(), phase.current_data())
                        .await?
                }
            }
        };
        for check in data.revision_checks() {
            if !tx.expect(&check.key, &check.expected_revision) {
                return Err(fail());
            }
        }
        tx.now = data.observed_at_ms();
        Ok(data)
    }
    fn require_row_time(
        &self,
        row: &SourceRenewalActionRowV3,
        now: i64,
    ) -> Result<(), GatewayError> {
        if row.v != versions::RENEWAL_ACTION_V3
            || now >= row.deadline_ms
            || row.deadline_ms > add(row.created_at_ms, NATIVE_RENEWAL_MAX_MS)?
        {
            return Err(fail());
        }
        Ok(())
    }
    fn lifetime(
        &self,
        actor: &StoredNativeActorV2,
        minimum: Option<i64>,
        now: i64,
    ) -> Result<(), GatewayError> {
        validate_time(now)?;
        if actor
            .source_valid_until_ms
            .is_some_and(|expiry| expiry <= now)
            || actor.source_expiry().is_some_and(|expiry| expiry <= now)
            || minimum.is_some_and(|expiry| expiry <= now)
        {
            return Err(fail());
        }
        Ok(())
    }
    fn time(&self, anchor: i64) -> Result<i64, GatewayError> {
        if self.context.cancellation.cancelled() {
            return Err(GatewayError::new(
                499,
                "Native renewal was cancelled.",
                Some("request_cancelled"),
            ));
        }
        let now = self
            .clock
            .ok_or_else(|| {
                gateway_authority_contracts::unavailable(
                    "authority_clock_unavailable",
                    "Time-sensitive authority actions require a trusted current clock.",
                )
            })?
            .now_ms()?;
        validate_time(now)?;
        if now < anchor {
            return Err(gateway_authority_contracts::unavailable(
                "invalid_clock",
                "Authority clock moved backwards.",
            ));
        }
        Ok(now)
    }
}
async fn hydrate<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    checks: &[gateway_core::state::RevisionCheck],
) -> Result<(), GatewayError> {
    for check in checks {
        tx.observe_revision(&check.key).await?;
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(fail());
        }
    }
    Ok(())
}
fn action_reference(
    original: &RequestContext,
    policy_digest: &str,
    context: &NativeRenewalContext,
    physical_generation: &str,
) -> Result<String, GatewayError> {
    let document = SourceJson::from_serializable(&(
        versions::RENEWAL_ACTION_V3_DOMAIN,
        original.to_source_json()?,
        policy_digest,
        context,
        physical_generation,
    ))?;
    Ok(sha256_hex(document.as_bytes()))
}
