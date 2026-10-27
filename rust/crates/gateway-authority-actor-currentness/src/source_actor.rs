//! Actual original-request source authentication and closed same-store phase preparation.
use crate::{
    app::{ActorService, CurrentAppActorData},
    cache::ReadCache,
};
use gateway_auth_contracts::types::{AuthHost, AuthMethod, AuthPrincipal, AuthRequest};
use gateway_auth_passport_contracts::{
    actor_data::{PassportActorDataV2, PassportJwtBindingV2, PassportSessionData},
    session::PassportDate,
};
use gateway_auth_primitives::store;
use gateway_authority_actor_records::{
    legacy::config_digest, scope::NativeActorScope, source::StoredNativeActorV2,
};
use gateway_authority_contracts::{text, validate_time};
use gateway_authority_keys as keys;
use gateway_core::{
    identity::{PrincipalId, RequestContext},
    state::{AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, RevisionCheck},
};
use gateway_source::error::GatewayError;
use serde_json::json;

/// Only actual C authentication mints this original-request proof.
/// ```compile_fail
/// use gateway_authority_actor_currentness::source_actor::SourceNativeRenewalActor;
/// fn duplicate<S>(value: SourceNativeRenewalActor<'_,S>) {let _=(value,value);}
/// ```
pub struct SourceNativeRenewalActor<'a, S> {
    store: &'a S,
    actor: StoredNativeActorV2,
    context: RequestContext,
}
impl<S> SourceNativeRenewalActor<'_, S> {
    pub fn principal(&self) -> &PrincipalId {
        &self.actor.principal
    }
    pub fn context(&self) -> &RequestContext {
        &self.context
    }
    pub fn scope(&self) -> NativeActorScope {
        self.actor.scope
    }
}
/// Ordinary phase observations; no public effect accepts this DATA as an authorization credential.
pub struct SourceActorPreparationData {
    actor: StoredNativeActorV2,
    context: RequestContext,
    current: SourceActorCurrentData,
}
impl SourceActorPreparationData {
    pub fn actor_data(&self) -> &StoredNativeActorV2 {
        &self.actor
    }
    pub fn context(&self) -> &RequestContext {
        &self.context
    }
    pub fn current_data(&self) -> &SourceActorCurrentData {
        &self.current
    }
}
pub struct SourceActorCurrentData {
    principal: PrincipalId,
    scope: NativeActorScope,
    policy_digest: String,
    checks: Vec<RevisionCheck>,
    minimum_expiry: Option<i64>,
}
impl SourceActorCurrentData {
    pub fn principal(&self) -> &PrincipalId {
        &self.principal
    }
    pub fn scope(&self) -> NativeActorScope {
        self.scope
    }
    pub fn policy_digest(&self) -> &str {
        &self.policy_digest
    }
    pub fn revision_checks(&self) -> &[RevisionCheck] {
        &self.checks
    }
    pub fn minimum_expiry(&self) -> Option<i64> {
        self.minimum_expiry
    }
}
impl From<CurrentAppActorData> for SourceActorCurrentData {
    fn from(data: CurrentAppActorData) -> Self {
        Self {
            principal: data.principal().clone(),
            scope: data.scope(),
            policy_digest: data.policy_digest().to_owned(),
            checks: data.revision_checks().to_vec(),
            minimum_expiry: data.minimum_expiry(),
        }
    }
}
impl<'a, S: AuthorityStore, H: AuthHost + PassportDate> ActorService<'a, S, H> {
    pub async fn authenticate_source_client_actor(
        &self,
        original: &AuthRequest,
        context: &RequestContext,
    ) -> Result<SourceNativeRenewalActor<'a, S>, GatewayError> {
        let principal = self.auth.authenticate_client(original).await?;
        self.mint_source(original, context, principal, NativeActorScope::Client)
            .await
    }
    pub async fn authenticate_source_admin_actor(
        &self,
        original: &AuthRequest,
        context: &RequestContext,
    ) -> Result<SourceNativeRenewalActor<'a, S>, GatewayError> {
        let principal = self.auth.authenticate_admin(original).await?;
        self.mint_source(original, context, principal, NativeActorScope::Admin)
            .await
    }
    async fn mint_source(
        &self,
        original: &AuthRequest,
        context: &RequestContext,
        principal: AuthPrincipal,
        scope: NativeActorScope,
    ) -> Result<SourceNativeRenewalActor<'a, S>, GatewayError> {
        let now = self.source_time(0)?;
        text(&context.request_id.0)?;
        text(&context.route)?;
        let identity = principal
            .method
            .principal_id(principal.token.as_ref().map(|token| token.expose()));
        if context.principal != identity
            || context.started_at_ms < 0
            || context.started_at_ms > now
            || context.key_id.is_some()
        {
            return Err(fail());
        }
        if matches!(original.method.as_str(), "GET" | "HEAD")
            && original
                .url
                .split(['?', '#'])
                .next()
                .is_some_and(|url| url.ends_with("/health"))
        {
            return Err(fail());
        }
        let source = source_provenance(principal, scope)?;
        let mut cache = ReadCache::new(self.store, now)?;
        let owner_revision = observed(&mut cache, &keys::paid_control_owner()).await?;
        let config_revision = observed(&mut cache, &keys::paid_control_config()).await?;
        let runtime_revision = observed(&mut cache, &runtime_key()).await?;
        let source_valid_until_ms = cache.minimum_expiry();
        drop(cache);
        let actor = StoredNativeActorV2 {
            v: 2,
            principal: identity,
            scope,
            config_digest: config_digest(self.config)?,
            owner_revision,
            config_revision,
            runtime_revision,
            source_valid_until_ms,
            source,
        };
        let current = self
            .check_stored_source_phase(&actor, context, scope, self.source_time(now)?)
            .await?;
        self.source_lifetime(&actor, self.source_time(now)?)?;
        if self
            .store
            .commit(AtomicTransition {
                checks: current.revision_checks().to_vec(),
                mutations: vec![],
            })
            .await?
            != CommitOutcome::Committed
        {
            return Err(fail());
        }
        self.source_lifetime(&actor, self.source_time(now)?)?;
        Ok(SourceNativeRenewalActor {
            store: self.store,
            actor,
            context: context.clone(),
        })
    }
    /// Fixed producer-owned snapshot, never caller-supplied storage/readsets or reconstructed proof DATA.
    pub async fn prepare_source_phase(
        &self,
        actor: &SourceNativeRenewalActor<'_, S>,
        required: NativeActorScope,
        now: i64,
    ) -> Result<SourceActorPreparationData, GatewayError> {
        if !std::ptr::eq(self.store, actor.store) || actor.scope() != required {
            return Err(fail());
        }
        let current = self
            .check_stored_source_phase(&actor.actor, &actor.context, required, now)
            .await?;
        Ok(SourceActorPreparationData {
            actor: actor.actor.clone(),
            context: actor.context.clone(),
            current,
        })
    }
    /// Actual authenticated effect owners call this internally before their original final CAS.
    pub async fn check_stored_source_phase(
        &self,
        actor: &StoredNativeActorV2,
        context: &RequestContext,
        required: NativeActorScope,
        now: i64,
    ) -> Result<SourceActorCurrentData, GatewayError> {
        self.source_lifetime(actor, now)?;
        if actor.v != 2
            || actor.scope != required
            || actor.principal != context.principal
            || !actor.principal_matches_source()
            || actor.config_digest != config_digest(self.config)?
        {
            return Err(fail());
        }
        match &actor.source {
            PassportActorDataV2::PassportAppSession(_) => {
                let checked = self.check_app_effect(actor, context, required).await?;
                let data = self
                    .consume_app_snapshot(
                        checked,
                        self.store,
                        actor,
                        context,
                        self.source_time(now)?,
                    )
                    .await?;
                Ok(data.into())
            }
            PassportActorDataV2::PassportJwt(jwt) => {
                if required != NativeActorScope::Admin || jwt.expires_at_ms <= now {
                    return Err(fail());
                }
                actor.to_source_json()?;
                let mut cache = ReadCache::new(self.store, now)?;
                let owner = observed(&mut cache, &keys::paid_control_owner()).await?;
                let config = observed(&mut cache, &keys::paid_control_config()).await?;
                let runtime = observed(&mut cache, &runtime_key()).await?;
                if owner != actor.owner_revision
                    || config != actor.config_revision
                    || runtime != actor.runtime_revision
                {
                    return Err(fail());
                }
                self.source_lifetime(actor, self.source_time(now)?)?;
                Ok(SourceActorCurrentData {
                    principal: actor.principal.clone(),
                    scope: actor.scope,
                    policy_digest: actor.policy_digest()?,
                    checks: cache.checks(),
                    minimum_expiry: cache.minimum_expiry(),
                })
            }
        }
    }
    fn source_time(&self, anchor: i64) -> Result<i64, GatewayError> {
        let now = self.host.now_ms();
        validate_time(now)?;
        if now < anchor {
            return Err(fail());
        }
        Ok(now)
    }
    fn source_lifetime(&self, actor: &StoredNativeActorV2, now: i64) -> Result<(), GatewayError> {
        validate_time(now)?;
        if actor
            .source_valid_until_ms
            .is_some_and(|expiry| expiry <= now)
            || actor.source_expiry().is_some_and(|expiry| expiry <= now)
        {
            return Err(fail());
        }
        Ok(())
    }
}
fn source_provenance(
    principal: AuthPrincipal,
    scope: NativeActorScope,
) -> Result<PassportActorDataV2, GatewayError> {
    match principal.method {
        AuthMethod::PassportSession {
            identity: PassportSessionData::App(value),
        } if scope != NativeActorScope::Admin
            || (principal.is_admin && value.identity.is_admin) =>
        {
            Ok(PassportActorDataV2::PassportAppSession(value.binding))
        }
        AuthMethod::PassportSession {
            identity: PassportSessionData::Jwt(claims),
        } if scope == NativeActorScope::Admin
            && principal.is_admin
            && claims.is_admin
            && claims.agent_token_id.is_none() =>
        {
            Ok(PassportActorDataV2::PassportJwt(PassportJwtBindingV2 {
                sub: claims.sub,
                jti: claims.jti,
                expires_at_ms: claims.exp.checked_mul(1000).ok_or_else(fail)?,
            }))
        }
        _ => Err(fail()),
    }
}
async fn observed<S: AuthorityStore>(
    cache: &mut ReadCache<'_, S>,
    key: &RecordKey,
) -> Result<Option<String>, GatewayError> {
    cache.observe_revision(key).await?;
    Ok(cache.revision(key))
}
fn runtime_key() -> RecordKey {
    store::key(json!(["uos_ai", "runtime_config", "v2"]))
}
fn fail() -> GatewayError {
    GatewayError::unavailable(
        "Original-request native actor authority is unavailable or changed.",
        "native_actor_unavailable",
    )
}
