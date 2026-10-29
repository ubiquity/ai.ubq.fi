//! Complete original admin/native import source observations. DATA never authorizes import or custody.
mod cache;
use cache::ReadCache;
use gateway_auth_credentials::provider_credentials::{
    codex_source_pool_key, parse_codex_source_pool,
};
use gateway_auth_native_contracts::native_import::NativeImportTarget;
use gateway_auth_native_proof::native_auth::NativeTransportIdentity;
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_authority_actor_currentness::source_actor::SourceActorCurrentData;
use gateway_authority_actor_records::{
    scope::NativeActorScope, source::StoredNativeActorV2, versions,
};
use gateway_authority_contracts::{AuthorityClock, NativeRenewalCancellation, validate_time};
use gateway_authority_keys as keys;
use gateway_authority_native_records::{
    import_facts::ImportFacts, owner::RegisteredRenewalOwner, rows,
};
use gateway_authority_paid_control_records::{
    PAID_CONTROL_CONFIG_VERSION, PaidControlConfig, PaidControlOwner,
};
use gateway_authority_routing_records::types::SubscriptionPool;
use gateway_contracts::identity::RequestContext;
use gateway_core::state::{AuthorityStore, RevisionCheck};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde::Serialize;
use serde_json::Value;
pub struct SourceImportFactsData {
    facts: ImportFacts,
    checks: Vec<RevisionCheck>,
    observed_at_ms: i64,
}
impl SourceImportFactsData {
    pub fn facts(&self) -> &ImportFacts {
        &self.facts
    }
    pub fn revision_checks(&self) -> &[RevisionCheck] {
        &self.checks
    }
    pub fn observed_at_ms(&self) -> i64 {
        self.observed_at_ms
    }
}
pub struct SourceNativeImportFactsReader<'a, S, C> {
    cache: ReadCache<'a, S>,
    identity: &'a NativeTransportIdentity,
    registered: &'a RegisteredRenewalOwner,
    clock: Option<&'a dyn AuthorityClock>,
    cancellation: &'a C,
}
impl<'a, S: AuthorityStore, C: NativeRenewalCancellation> SourceNativeImportFactsReader<'a, S, C> {
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
    async fn require_phase(
        &mut self,
        actor: &StoredNativeActorV2,
        original: &RequestContext,
        phase: &SourceActorCurrentData,
    ) -> Result<(), GatewayError> {
        if actor.v != 2
            || actor.scope != NativeActorScope::Admin
            || phase.scope() != NativeActorScope::Admin
            || actor.principal != original.principal
            || &actor.principal != phase.principal()
            || actor.policy_digest()? != phase.policy_digest()
        {
            return Err(unavailable());
        }
        self.lifetime(actor, phase, self.cache.now)?;
        for check in phase.revision_checks() {
            self.cache.observe_revision(&check.key).await?;
            if !self.cache.expect(&check.key, &check.expected_revision) {
                return Err(unavailable());
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
        if actor.source_valid_until_ms.is_some_and(|at| at <= now)
            || actor.source_expiry().is_some_and(|at| at <= now)
            || phase.minimum_expiry().is_some_and(|at| at <= now)
        {
            return Err(unavailable());
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
    pub async fn read(
        mut self,
        actor: &StoredNativeActorV2,
        original: &RequestContext,
        phase: &SourceActorCurrentData,
        target: &NativeImportTarget,
    ) -> Result<SourceImportFactsData, GatewayError> {
        let NativeImportTarget::ExistingAccount { account_id_hash } = target else {
            return Err(unavailable());
        };
        let owner_key = keys::paid_control_owner();
        let owner: PaidControlOwner = self.cache.read(&owner_key).await?.ok_or_else(unavailable)?;
        if owner.v != 1
            || owner.owner_id != self.identity.owner_id()
            || owner.owner_epoch != self.identity.owner_epoch()
            || owner.key_fingerprint != self.identity.key_fingerprint()
        {
            return Err(unavailable());
        }
        let config: PaidControlConfig = self
            .cache
            .read(&keys::paid_control_config())
            .await?
            .ok_or_else(unavailable)?;
        let runtime = gateway_schema::catalog_store::runtime_config_key();
        self.cache.observe_revision(&runtime).await?;
        if config.v != PAID_CONTROL_CONFIG_VERSION
            || config.owner_id != owner.owner_id
            || config.owner_epoch != owner.owner_epoch
            || config.source_checks.len() != 1
            || config.source_checks[0].key != runtime.0
            || config.source_checks[0].expected_revision != self.cache.revision(&runtime)
        {
            return Err(unavailable());
        }
        self.require_phase(actor, original, phase).await?;
        if phase.scope() != gateway_authority_actor_records::scope::NativeActorScope::Admin {
            return Err(unavailable());
        }
        let key = codex_source_pool_key();
        let raw: Value = self.cache.read(&key).await?.ok_or_else(unavailable)?;
        let source = parse_codex_source_pool(&raw)?.ok_or_else(unavailable)?;
        let normalized: SubscriptionPool = self
            .cache
            .read(&keys::pool())
            .await?
            .ok_or_else(unavailable)?;
        gateway_authority_routing_evidence::validation::validate_pool(&normalized)?;
        if normalized.pool_versionstamp.0 != self.cache.revision(&key).ok_or_else(unavailable)?
            || normalized.accounts.len() != source.accounts.len()
        {
            return Err(unavailable());
        }
        let mut selected = None;
        let mut account_digests = Vec::new();
        for (index, account) in source.accounts.iter().enumerate() {
            account_digests.push(legacy_digest(&account.private_source_value())?);
            let identity = account.identity();
            let projected = &normalized.accounts[index];
            if projected.account_id_hash != identity.routing_account_hash
                || projected.admin_account_hash != identity.administrative_account_hash
                || projected.credential_version != identity.credential_version
            {
                return Err(unavailable());
            }
            if identity.routing_account_hash.0 == *account_id_hash {
                let native = account.native_owner.as_ref().ok_or_else(unavailable)?;
                if native.codex_home != self.registered.registered_home()
                    || native.generation_hash != identity.native_generation_hash
                    || native.external_user_id.is_none()
                        && (native.managed_source.is_none()
                            || !account.refresh_token.expose().is_empty())
                {
                    return Err(unavailable());
                }
                let physical = native
                .managed_source
                .as_ref()
                .filter(|source| {
                    source.v == gateway_auth_credentials::provider_credentials::MANAGED_NATIVE_SOURCE_VERSION
                })
                .ok_or_else(unavailable)?;
                if native.external_user_id.is_some() || !account.refresh_token.expose().is_empty() {
                    return Err(unavailable());
                }
                selected = Some((
                    index as u64,
                    identity.credential_version.0,
                    native.generation_hash.clone(),
                    physical.generation_hash.clone(),
                ));
            }
        }
        let (slot, credential_version, access_generation, source_native_generation) =
            selected.ok_or_else(unavailable)?;
        self.cache.now = self.time(self.cache.now)?;
        self.lifetime(actor, phase, self.cache.now)?;
        let facts = ImportFacts {
            owner_id: owner.owner_id,
            owner_epoch: owner.owner_epoch,
            native_owner_id: self.registered.native_owner_id().into(),
            slot,
            account_id_hash: account_id_hash.clone(),
            credential_version,
            access_generation,
            source_native_generation,
            owner_revision: self.cache.revision(&owner_key).ok_or_else(unavailable)?,
            source_revision: self.cache.revision(&key).ok_or_else(unavailable)?,
            normalized_revision: self.cache.revision(&keys::pool()).ok_or_else(unavailable)?,
            runtime_revision: self.cache.revision(&runtime),
            control_checks_digest: control_digest(&self.cache.checks())?,
            account_digests,
            checks_digest: checks_digest(&self.cache.checks())?,
        };
        Ok(SourceImportFactsData {
            facts,
            checks: self.cache.checks(),
            observed_at_ms: self.cache.now,
        })
    }
}
fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Native import preparation is unavailable, changed, expired, or unsupported.",
        "native_import_authority_unavailable",
    )
}
fn legacy_digest<T: Serialize>(value: &T) -> Result<String, GatewayError> {
    Ok(sha256_hex(
        &serde_json::to_vec(value).map_err(|_| rows::fail())?,
    ))
}
fn checks_digest(checks: &[RevisionCheck]) -> Result<String, GatewayError> {
    let prefix = rows::prefix();
    let values = checks
        .iter()
        .filter(|check| !check.key.0.starts_with(&prefix.0))
        .map(|check| (&check.key.0, &check.expected_revision))
        .collect::<Vec<_>>();
    let source = SourceJson::from_serializable(&(versions::CONTROL_CHECKS_V2_DOMAIN, values))?;
    Ok(sha256_hex(source.as_bytes()))
}
fn control_digest(checks: &[RevisionCheck]) -> Result<String, GatewayError> {
    let source = codex_source_pool_key();
    let pool = keys::pool();
    let runtime = gateway_schema::catalog_store::runtime_config_key();
    checks_digest(
        &checks
            .iter()
            .filter(|check| check.key != source && check.key != pool && check.key != runtime)
            .cloned()
            .collect::<Vec<_>>(),
    )
}
