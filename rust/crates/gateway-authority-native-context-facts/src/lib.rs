//! Complete fixed native context observations. DATA never grants a native effect.
mod account;
mod cache;
mod inference;
pub mod terminal;
use cache::ReadCache;
use gateway_auth_credentials::provider_credentials::{
    codex_access_expiry_ms, codex_source_pool_key, parse_codex_source_pool,
};
use gateway_auth_native_proof::native_auth::NativeTransportIdentity;
use gateway_authority_actor_currentness::source_actor::SourceActorCurrentData;
use gateway_authority_actor_records::{
    scope::NativeActorScope, source::StoredNativeActorV2, versions,
};
use gateway_authority_contracts::{AuthorityClock, NativeRenewalCancellation, validate_time};
use gateway_authority_keys as keys;
use gateway_authority_native_records::{
    live::mapping_key, owner::RegisteredRenewalOwner, rows::fail,
};
use gateway_authority_paid_control_records::{
    PAID_CONTROL_CONFIG_VERSION, PaidControlConfig, PaidControlOwner,
};
use gateway_authority_routing_records::types::SubscriptionPool;
use gateway_cache_experiment_evidence::CacheEvidenceReader;
use gateway_cache_experiment_records::RefreshContext;
use gateway_contracts::identity::{PrincipalId, RequestContext};
use gateway_core::state::{AuthorityStore, RevisionCheck};
use gateway_model_selection::selection as model_selection;
use gateway_native_authority_wire::{
    context::NativeRenewalContext, source_fence::SourceNativeRenewalFenceV3,
};
use gateway_schema::catalog_store::runtime_config_key;
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde::Serialize;
use serde_json::Value;
struct Control {
    principal: PrincipalId,
    policy_digest: String,
    account_hash: String,
    checks_digest: String,
}
struct CredentialData<'a> {
    id: gateway_auth_credentials::provider_credentials::CodexCredentialIdentity,
    native: &'a gateway_auth_credentials::provider_credentials::CodexNativeOwner,
    source: &'a gateway_auth_credentials::provider_credentials::CodexSourcePool,
    source_key: gateway_kv_codec::key::RecordKey,
    physical: String,
    expiry: Option<i64>,
}
pub struct SourceNativeControlFactsData {
    principal: PrincipalId,
    policy_digest: String,
    account_hash: String,
    control_digest: String,
    checks: Vec<RevisionCheck>,
    observed_at_ms: i64,
}
impl SourceNativeControlFactsData {
    pub fn principal(&self) -> &PrincipalId {
        &self.principal
    }
    pub fn policy_digest(&self) -> &str {
        &self.policy_digest
    }
    pub fn account_hash(&self) -> &str {
        &self.account_hash
    }
    pub fn control_digest(&self) -> &str {
        &self.control_digest
    }
    pub fn revision_checks(&self) -> &[RevisionCheck] {
        &self.checks
    }
    pub fn observed_at_ms(&self) -> i64 {
        self.observed_at_ms
    }
}
pub struct SourceNativeContextFactsData {
    fence: SourceNativeRenewalFenceV3,
    checks_digest: String,
    control_checks_digest: String,
    account_digests: Vec<String>,
    checks: Vec<RevisionCheck>,
    observed_at_ms: i64,
}
impl SourceNativeContextFactsData {
    pub fn fence(&self) -> &SourceNativeRenewalFenceV3 {
        &self.fence
    }
    pub fn checks_digest(&self) -> &str {
        &self.checks_digest
    }
    pub fn control_checks_digest(&self) -> &str {
        &self.control_checks_digest
    }
    pub fn account_digests(&self) -> &[String] {
        &self.account_digests
    }
    pub fn revision_checks(&self) -> &[RevisionCheck] {
        &self.checks
    }
    pub fn observed_at_ms(&self) -> i64 {
        self.observed_at_ms
    }
}
pub struct SourceNativeContextFactsReader<'a, S, C> {
    cache: ReadCache<'a, S>,
    identity: &'a NativeTransportIdentity,
    registered: &'a RegisteredRenewalOwner,
    clock: Option<&'a dyn AuthorityClock>,
    cancellation: &'a C,
}
impl<'a, S: AuthorityStore, C: NativeRenewalCancellation> SourceNativeContextFactsReader<'a, S, C> {
    pub fn new(
        snapshot: &'a S,
        identity: &'a NativeTransportIdentity,
        registered: &'a RegisteredRenewalOwner,
        clock: Option<&'a dyn AuthorityClock>,
        cancellation: &'a C,
        now: i64,
    ) -> Result<Self, GatewayError> {
        Ok(Self {
            cache: ReadCache::new(snapshot, now)?,
            identity,
            registered,
            clock,
            cancellation,
        })
    }
    pub async fn read_control(
        mut self,
        actor: &StoredNativeActorV2,
        original: &RequestContext,
        phase: &SourceActorCurrentData,
        context: &NativeRenewalContext,
    ) -> Result<SourceNativeControlFactsData, GatewayError> {
        let control = self.live_control(actor, original, phase, context).await?;
        Ok(SourceNativeControlFactsData {
            principal: control.principal,
            policy_digest: control.policy_digest,
            account_hash: control.account_hash,
            control_digest: control.checks_digest,
            checks: self.cache.checks(),
            observed_at_ms: self.cache.now,
        })
    }
    pub async fn read_context(
        mut self,
        actor: &StoredNativeActorV2,
        original: &RequestContext,
        phase: &SourceActorCurrentData,
        context: &NativeRenewalContext,
    ) -> Result<SourceNativeContextFactsData, GatewayError> {
        if matches!(context, NativeRenewalContext::Inference { .. }) {
            return self
                .inference_context(actor, original, phase, Some(context))
                .await;
        }
        let request_id = original.request_id.0.as_str();
        let control = self.live_control(actor, original, phase, context).await?;
        let source_key = codex_source_pool_key();
        let raw: Value = self.cache.read(&source_key).await?.ok_or_else(fail)?;
        let source = parse_codex_source_pool(&raw)?.ok_or_else(fail)?;
        let normalized: SubscriptionPool =
            self.cache.read(&keys::pool()).await?.ok_or_else(fail)?;
        gateway_authority_routing_evidence::validation::validate_pool(&normalized)?;
        if normalized.pool_versionstamp.0 != self.cache.revision(&source_key).ok_or_else(fail)?
            || source.accounts.len() != normalized.accounts.len()
        {
            return Err(fail());
        }
        let (slot, account) = source
            .accounts
            .iter()
            .enumerate()
            .find(|(_, a)| a.identity().routing_account_hash.0 == control.account_hash)
            .ok_or_else(fail)?;
        let id = account.identity();
        let projected = &normalized.accounts[slot];
        if projected.account_id_hash != id.routing_account_hash
            || projected.admin_account_hash != id.administrative_account_hash
            || projected.credential_version != id.credential_version
        {
            return Err(fail());
        }
        if let NativeRenewalContext::LiveCreate { binding, .. } = context {
            if binding.slot != slot as u64
                || binding.pool_versionstamp != normalized.pool_versionstamp
                || binding.credential_version != id.credential_version
            {
                return Err(fail());
            }
            let selection: Option<Value> = self.cache.read(&keys::provider_selection()).await?;
            if selection
                .as_ref()
                .filter(|v| !v.is_null())
                .is_some_and(|v| {
                    model_selection::ProviderSelection::normalize(v)
                        .is_none_or(|s| !s.account_eligible(&id.administrative_account_hash.0))
                })
            {
                return Err(fail());
            }
        }
        let native = account.native_owner.as_ref().ok_or_else(fail)?;
        if native.codex_home != self.registered.registered_home()
            || native.generation_hash != id.native_generation_hash
            || (native.external_user_id.is_none()
                && (native.managed_source.is_none() || !account.refresh_token.expose().is_empty()))
        {
            return Err(fail());
        }
        let raw_expiry = codex_access_expiry_ms(&account.access_token);
        // Source auth.ts needsRefresh: JWT truthy expiry <2min; otherwise updated age >7min.
        let needs = raw_expiry.filter(|v| *v != 0.0).map_or(
            (self.time(self.cache.now)? as f64) - account.updated_at_ms > 420_000.0,
            |v| v - (self.cache.now as f64) < 120_000.0,
        );
        if !needs && !matches!(context, NativeRenewalContext::CacheExperiment { .. }) {
            return Err(GatewayError::new(
                409,
                "Current live access does not require native renewal.",
                Some("native_live_renewal_not_required"),
            ));
        }
        let expiry = raw_expiry
            .filter(|v| {
                v.is_finite() && v.fract() == 0.0 && *v >= 0.0 && *v <= 9_007_199_254_740_991.0
            })
            .map(|v| v as i64);
        let physical = native
            .managed_source
            .as_ref()
            .map(|s| s.generation_hash.clone())
            .unwrap_or_else(|| native.generation_hash.clone());
        self.cache.now = self.time(self.cache.now)?;
        self.lifetime(actor, phase, self.cache.now)?;
        if let NativeRenewalContext::LiveMapped { call_id, .. } = context
            && self
                .cache
                .expiry(&mapping_key(call_id, false))
                .is_some_and(|at| at <= self.cache.now)
        {
            return Err(fail());
        }
        if let NativeRenewalContext::CacheExperiment { context } = context {
            let now = self.cache.now;
            self.validate_cache_context(context, now).await?;
        }
        self.finish_context(
            control,
            context,
            request_id,
            CredentialData {
                id,
                native,
                source: &source,
                source_key,
                physical,
                expiry,
            },
        )
    }
    fn finish_context(
        &self,
        control: Control,
        context: &NativeRenewalContext,
        request_id: &str,
        credential: CredentialData<'_>,
    ) -> Result<SourceNativeContextFactsData, GatewayError> {
        Ok(SourceNativeContextFactsData {
            checks: self.cache.checks(),
            observed_at_ms: self.cache.now,
            fence: SourceNativeRenewalFenceV3 {
                v: gateway_authority_actor_records::versions::RENEWAL_FENCE_V3,
                owner_id: self.identity.owner_id().into(),
                owner_epoch: self.identity.owner_epoch().into(),
                owner_revision: self
                    .cache
                    .revision(&keys::paid_control_owner())
                    .ok_or_else(fail)?,
                native_owner_id: self.registered.native_owner_id().into(),
                source_revision: self
                    .cache
                    .revision(&credential.source_key)
                    .ok_or_else(fail)?,
                normalized_revision: self.cache.revision(&keys::pool()).ok_or_else(fail)?,
                account_id_hash: credential.id.routing_account_hash.0,
                credential_version: credential.id.credential_version.0,
                source_native_generation: credential.physical,
                access_generation: credential.native.generation_hash.clone(),
                access_expires_at_ms: credential.expiry,
                native_refreshed_at: credential.native.native_refreshed_at.clone(),
                principal: control.principal.as_source().clone(),
                principal_policy_digest: control.policy_digest,
                original_request_id: request_id.into(),
                context: context.clone(),
            },
            checks_digest: checks_digest(&self.cache.checks())?,
            control_checks_digest: control.checks_digest,
            account_digests: credential
                .source
                .accounts
                .iter()
                .map(|a| legacy_digest(&a.private_source_value()))
                .collect::<Result<_, _>>()?,
        })
    }
    async fn live_control(
        &mut self,
        actor: &StoredNativeActorV2,
        original: &RequestContext,
        phase: &SourceActorCurrentData,
        context: &NativeRenewalContext,
    ) -> Result<Control, GatewayError> {
        if matches!(context, NativeRenewalContext::Inference { .. }) {
            return self
                .inference_control(actor, original, phase, context)
                .await;
        }

        let owner: PaidControlOwner = self
            .cache
            .read(&keys::paid_control_owner())
            .await?
            .ok_or_else(fail)?;
        if owner.v != 1
            || owner.owner_id != self.identity.owner_id()
            || owner.owner_epoch != self.identity.owner_epoch()
            || owner.key_fingerprint != self.identity.key_fingerprint()
        {
            return Err(fail());
        }
        let config: PaidControlConfig = self
            .cache
            .read(&keys::paid_control_config())
            .await?
            .ok_or_else(fail)?;
        let runtime_key = runtime_config_key();
        self.cache.observe_revision(&runtime_key).await?;
        if config.v != PAID_CONTROL_CONFIG_VERSION
            || config.owner_id != owner.owner_id
            || config.owner_epoch != owner.owner_epoch
            || config.source_checks.len() != 1
            || config.source_checks[0].key != runtime_key.0
            || config.source_checks[0].expected_revision != self.cache.revision(&runtime_key)
        {
            return Err(fail());
        }
        self.require_source(actor, original, phase).await?;
        let scope = if matches!(context, NativeRenewalContext::CacheExperiment { .. }) {
            NativeActorScope::Admin
        } else {
            NativeActorScope::Client
        };
        if phase.scope() != scope {
            return Err(fail());
        }
        let account_hash = self
            .live_account_hash(context, original.principal.as_source())
            .await?;
        self.cache.now = self.time(self.cache.now)?;
        self.lifetime(actor, phase, self.cache.now)?;
        if let NativeRenewalContext::LiveMapped { call_id, .. } = context
            && self
                .cache
                .expiry(&mapping_key(call_id, false))
                .is_some_and(|at| at <= self.cache.now)
        {
            return Err(fail());
        }
        // Credential replacement is proven separately by current facts/known terminal publication.
        // Do not mistake the operation's own source+projection CAS for caller/configuration drift.
        let credential_keys = [codex_source_pool_key(), keys::pool()];
        let control_checks = self
            .cache
            .checks()
            .into_iter()
            .filter(|c| !credential_keys.contains(&c.key))
            .collect::<Vec<_>>();
        Ok(Control {
            principal: original.principal.clone(),
            policy_digest: phase.policy_digest().to_owned(),
            account_hash,
            checks_digest: checks_digest(&control_checks)?,
        })
    }
    async fn require_source(
        &mut self,
        actor: &StoredNativeActorV2,
        original: &RequestContext,
        phase: &SourceActorCurrentData,
    ) -> Result<(), GatewayError> {
        if actor.v != 2
            || actor.principal != original.principal
            || &actor.principal != phase.principal()
            || actor.scope != phase.scope()
            || actor.policy_digest()? != phase.policy_digest()
        {
            return Err(fail());
        }
        self.lifetime(actor, phase, self.cache.now)?;
        for check in phase.revision_checks() {
            self.cache.observe_revision(&check.key).await?;
            if !self.cache.expect(&check.key, &check.expected_revision) {
                return Err(fail());
            }
        }
        Ok(())
    }
    fn lifetime(
        &self,
        actor: &StoredNativeActorV2,
        phase: &SourceActorCurrentData,
        now: i64,
    ) -> Result<(), GatewayError> {
        validate_time(now)?;
        if actor
            .source_valid_until_ms
            .is_some_and(|expiry| expiry <= now)
            || actor.source_expiry().is_some_and(|expiry| expiry <= now)
            || phase.minimum_expiry().is_some_and(|expiry| expiry <= now)
        {
            return Err(fail());
        }
        Ok(())
    }
    async fn validate_cache_context(
        &mut self,
        context: &RefreshContext,
        now: i64,
    ) -> Result<(), GatewayError> {
        let observed = self.cache.now;
        let evidence = {
            let store = self.cache.fixed_store();
            CacheEvidenceReader::new(&store, observed)?
                .validate_refresh_context(context, now)
                .await?
        };
        for check in evidence.revision_checks() {
            if !self.cache.expect(&check.key, &check.expected_revision) {
                return Err(fail());
            }
        }
        Ok(())
    }
    fn time(&self, anchor: i64) -> Result<i64, GatewayError> {
        if self.cancellation.cancelled() {
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
fn legacy_digest<T: Serialize>(value: &T) -> Result<String, GatewayError> {
    Ok(gateway_auth_primitives::crypto::sha256_hex(
        &serde_json::to_vec(value).map_err(|_| fail())?,
    ))
}
fn checks_digest(checks: &[RevisionCheck]) -> Result<String, GatewayError> {
    let prefix = gateway_authority_native_records::rows::prefix();
    let values = checks
        .iter()
        .filter(|check| !check.key.0.starts_with(&prefix.0))
        .map(|check| (&check.key.0, &check.expected_revision))
        .collect::<Vec<_>>();
    let document = SourceJson::from_serializable(&(versions::CONTROL_CHECKS_V2_DOMAIN, values))?;
    Ok(gateway_auth_primitives::crypto::sha256_hex(
        document.as_bytes(),
    ))
}
