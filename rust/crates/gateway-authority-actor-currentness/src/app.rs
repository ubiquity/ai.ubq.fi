//! Actual C AuthService preflight. No actor proof or central grant deserializes from DATA.
use crate::cache::{ReadCache, source_document};
use gateway_auth::resolve::AuthService;
use gateway_auth_contracts::types::{AuthConfig, AuthHost};
use gateway_auth_passport_contracts::{
    actor_data::{PassportActorDataV2, PassportAppSessionBinding},
    session::PassportDate,
};
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_authority_actor_records::{
    legacy::config_digest, scope::NativeActorScope, source::StoredNativeActorV2,
};
use gateway_authority_keys as keys;
use gateway_contracts::{identity::PrincipalId, identity::RequestContext};
use gateway_core::{state::AuthorityStore, state::RevisionCheck};
use gateway_kv_codec::key::RecordKey;
use gateway_kv_codec::tuple_key::{ScalarKeyPart, TupleKey};
use gateway_source::{error::GatewayError, js_string::JsString16};
use serde::Deserialize;

pub struct ActorService<'a, S, H> {
    pub(crate) store: &'a S,
    pub(crate) host: &'a H,
    pub(crate) config: &'a AuthConfig,
    pub(crate) auth: AuthService<'a, S, H>,
}
/// One actual source operation, not DATA. No Clone/Deserialize/constructor/getter.
/// ```compile_fail
/// use gateway_authority_actor_currentness::app::CheckedAppActor;
/// fn duplicate<S>(value: CheckedAppActor<'_, S>) { let _ = (value, value); }
/// ```
pub struct CheckedAppActor<'a, S> {
    store: &'a S,
    actor: StoredNativeActorV2,
    context: RequestContext,
    binding: PassportAppSessionBinding,
}
pub struct CurrentAppActorData {
    principal: PrincipalId,
    scope: NativeActorScope,
    policy_digest: String,
    checks: Vec<RevisionCheck>,
    minimum_expiry: Option<i64>,
}
impl CurrentAppActorData {
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
impl<'a, S: AuthorityStore, H: AuthHost + PassportDate> ActorService<'a, S, H> {
    pub fn new(store: &'a S, host: &'a H, config: &'a AuthConfig) -> Self {
        Self {
            store,
            host,
            config,
            auth: AuthService::new(store, host, config),
        }
    }
    /// Call before opening the actual effect snapshot. C owns401 deletion/503 retention and Date.parse.
    pub async fn check_app_effect(
        &self,
        actor: &StoredNativeActorV2,
        context: &RequestContext,
        required: NativeActorScope,
    ) -> Result<CheckedAppActor<'a, S>, GatewayError> {
        if actor.v != 2
            || actor.scope != required
            || actor.principal != context.principal
            || !actor.principal_matches_source()
            || actor.config_digest != config_digest(self.config)?
        {
            return Err(fail());
        }
        let PassportActorDataV2::PassportAppSession(binding) = &actor.source else {
            return Err(fail());
        };
        let current = self.auth.check_passport_app_session(binding).await?;
        if current.binding != *binding || current.identity.subject != binding.subject {
            return Err(fail());
        }
        if required == NativeActorScope::Admin && !current.identity.is_admin {
            return Err(GatewayError::new(
                403,
                "Administrator authentication is required.",
                Some("admin_auth_required"),
            ));
        }
        Ok(CheckedAppActor {
            store: self.store,
            actor: actor.clone(),
            context: context.clone(),
            binding: current.binding,
        })
    }
    /// Consume the checked phase once into fixed nonsecret observations for the original final CAS.
    pub async fn consume_app_snapshot<R: AuthorityStore>(
        &self,
        checked: CheckedAppActor<'_, S>,
        snapshot: &R,
        actor: &StoredNativeActorV2,
        context: &RequestContext,
        now: i64,
    ) -> Result<CurrentAppActorData, GatewayError> {
        if !std::ptr::eq(self.store, checked.store)
            || checked.actor != *actor
            || checked.context != *context
            || actor.config_digest != config_digest(self.config)?
            || actor
                .source_valid_until_ms
                .is_some_and(|expiry| expiry <= now)
        {
            return Err(fail());
        }
        let mut cache = ReadCache::new(snapshot, now)?;
        let owner = observed(&mut cache, &keys::paid_control_owner()).await?;
        let config = observed(&mut cache, &keys::paid_control_config()).await?;
        let runtime = observed(&mut cache, &runtime_key()).await?;
        if owner != actor.owner_revision
            || config != actor.config_revision
            || runtime != actor.runtime_revision
        {
            return Err(fail());
        }
        let key = app_key(&checked.binding.session_digest);
        let row = cache.cached_record(&key).await?.cloned().ok_or_else(fail)?;
        if row.revision != checked.binding.row_revision || row.expires_at_ms.is_some() {
            return Err(fail());
        }
        let local: AppRow = source_document(&row)?.deserialize().map_err(|_| fail())?;
        if local.version != 1.0
            || local.binding(&checked.binding.session_digest, &row.revision) != checked.binding
        {
            return Err(fail());
        }
        Ok(CurrentAppActorData {
            principal: actor.principal.clone(),
            scope: actor.scope,
            policy_digest: actor.policy_digest()?,
            checks: cache.checks(),
            minimum_expiry: cache.minimum_expiry(),
        })
    }
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct AppRow {
    version: f64,
    subject: JsString16,
    client_id: String,
    audience: String,
    origin: String,
    redirect_uri: String,
    created_at: JsString16,
    app_session_token: String,
}
impl AppRow {
    fn binding(&self, digest: &str, revision: &str) -> PassportAppSessionBinding {
        PassportAppSessionBinding {
            session_digest: digest.into(),
            row_revision: revision.into(),
            subject: self.subject.clone(),
            created_at: self.created_at.clone(),
            app_token_digest: sha256_hex(self.app_session_token.as_bytes()),
            client_id: self.client_id.clone(),
            audience: self.audience.clone(),
            origin: self.origin.clone(),
            redirect_uri: self.redirect_uri.clone(),
        }
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
    TupleKey::from_scalar_parts(&[
        ScalarKeyPart::String("uos_ai"),
        ScalarKeyPart::String("runtime_config"),
        ScalarKeyPart::String("v2"),
    ])
    .to_record_key()
}
fn app_key(digest: &str) -> RecordKey {
    TupleKey::from_scalar_parts(&[
        ScalarKeyPart::String("uos_ai"),
        ScalarKeyPart::String("auth"),
        ScalarKeyPart::String("passport"),
        ScalarKeyPart::String("sessions"),
        ScalarKeyPart::String(digest),
    ])
    .to_record_key()
}
fn fail() -> GatewayError {
    GatewayError::unavailable(
        "Original-request native actor authority is unavailable or changed.",
        "native_actor_unavailable",
    )
}
