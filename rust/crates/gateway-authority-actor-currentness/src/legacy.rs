//! Complete historical actor-v1 current predicate. DATA results never mint live actors.
use crate::cache::ReadCache;
use gateway_auth_attestation::upstream;
use gateway_auth_contracts::{passkeys::PasskeyUser, types::AuthConfig};
use gateway_auth_passkeys::{passkeys, session_index};
use gateway_auth_primitives::{api_keys, crypto::sha256_hex};
use gateway_authority_actor_records::{
    legacy::{ConfigMethod, SourceActor, StoredNativeActor, config_digest, policy_digest},
    scope::NativeActorScope,
};
use gateway_authority_keys as keys;
use gateway_core::{state::AuthorityStore, state::RevisionCheck};
use gateway_kv_codec::key::RecordKey;
use gateway_kv_codec::tuple_key::{ScalarKeyPart, TupleKey};
use gateway_source::error::GatewayError;

struct CurrentNativeActor {
    principal: String,
    scope: NativeActorScope,
    policy_digest: String,
}
pub struct LegacyCurrentData {
    principal: String,
    scope: NativeActorScope,
    policy_digest: String,
    checks: Vec<RevisionCheck>,
}
impl LegacyCurrentData {
    pub fn principal(&self) -> &str {
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
}
pub struct LegacyActorReader<'a, S> {
    cache: ReadCache<'a, S>,
    config: &'a AuthConfig,
}
impl<'a, S: AuthorityStore> LegacyActorReader<'a, S> {
    pub fn new(store: &'a S, config: &'a AuthConfig, now: i64) -> Result<Self, GatewayError> {
        Ok(Self {
            cache: ReadCache::new(store, now)?,
            config,
        })
    }
    pub async fn read(
        mut self,
        stored: &StoredNativeActor,
    ) -> Result<LegacyCurrentData, GatewayError> {
        let current = read_current(&mut self.cache, stored, self.config).await?;
        Ok(LegacyCurrentData {
            principal: current.principal,
            scope: current.scope,
            policy_digest: current.policy_digest,
            checks: self.cache.checks(),
        })
    }
}
async fn read_current<S: AuthorityStore>(
    tx: &mut ReadCache<'_, S>,
    stored: &StoredNativeActor,
    config: &AuthConfig,
) -> Result<CurrentNativeActor, GatewayError> {
    if stored.v != 1 || stored.config_digest != config_digest(config)? {
        return Err(fail());
    }
    ensure_lifetime(stored, tx.now)?;
    if observed(tx, &keys::paid_control_owner()).await? != stored.owner_revision
        || observed(tx, &keys::paid_control_config()).await? != stored.config_revision
        || observed(tx, &runtime_key()).await? != stored.runtime_revision
    {
        return Err(fail());
    }
    let principal = match &stored.source {
        SourceActor::Session {
            digest,
            index_revision,
            session_revision,
            user_id,
            user_revision,
            expires_at_ms,
        } => {
            let key = session_index::index_key(digest)?;
            let index: session_index::SessionRevisionFence =
                tx.read(&key).await?.ok_or_else(fail)?;
            let user_key = passkeys::key("users", user_id);
            let user: PasskeyUser = tx.read(&user_key).await?.ok_or_else(fail)?;
            if index.v != 1
                || index.session_digest != *digest
                || index.session_revision != *session_revision
                || index.user_id != *user_id
                || index.expires_at_ms != *expires_at_ms
                || tx.revision(&key).as_ref() != Some(index_revision)
                || tx.revision(&user_key).as_ref() != Some(user_revision)
                || user.id != *user_id
                || (stored.scope == NativeActorScope::Admin && !user.is_admin)
            {
                return Err(fail());
            }
            format!("passkey-user:{user_id}")
        }
        SourceActor::ApiKey {
            key_id,
            token_hash,
            hash_revision,
            policy_revision,
            ..
        } => {
            if stored.scope == NativeActorScope::Admin
                || token_hash.len() != 43
                || !token_hash
                    .bytes()
                    .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_'))
            {
                return Err(fail());
            }
            let key = api_keys::hash_key(token_hash);
            let record: api_keys::ApiKeyHashRecord = tx.read(&key).await?.ok_or_else(fail)?;
            let policy = api_keys::policy_from_record(token_hash.clone(), record, tx.now)?;
            let policy_key = keys::api_policy(key_id);
            let normalized: Option<gateway_authority_api_protocol::ApiKeyPolicy> =
                tx.read(&policy_key).await?;
            if let Some(normalized) = &normalized {
                normalized.validate(
                    &gateway_contracts::identity::ApiKeyId(key_id.clone()),
                    tx.now,
                )?;
            }
            if policy.key_id != *key_id
                || tx.revision(&key).as_ref() != Some(hash_revision)
                || tx.revision(&policy_key) != *policy_revision
                || normalized.as_ref().is_some_and(|normalized| {
                    policy.expires_at_ms != normalized.expires_at_ms
                        || policy.window_ms != normalized.window_ms
                        || policy.usage_limit_requests != normalized.usage_limit_requests
                })
            {
                return Err(fail());
            }
            format!("api-key:{key_id}")
        }
        SourceActor::Configuration {
            method, token_hash, ..
        } => configuration_principal(stored, config, method, token_hash)?,

        SourceActor::Passport {
            sub,
            jti,
            expires_at_ms,
        } => {
            if stored.scope != NativeActorScope::Admin
                || sub.is_empty()
                || jti.is_empty()
                || *expires_at_ms <= tx.now
            {
                return Err(fail());
            }
            format!("passport-user:{sub}")
        }
        SourceActor::Kernel { .. } => kernel_principal(tx, stored).await?,
    };
    if principal != stored.principal {
        return Err(fail());
    }
    Ok(CurrentNativeActor {
        principal,
        scope: stored.scope,
        policy_digest: policy_digest(stored)?,
    })
}

fn ensure_lifetime(stored: &StoredNativeActor, now: i64) -> Result<(), GatewayError> {
    if stored
        .source_valid_until_ms
        .is_some_and(|expiry| expiry <= now)
    {
        return Err(fail());
    }
    let expiry = match &stored.source {
        SourceActor::Session { expires_at_ms, .. } | SourceActor::Kernel { expires_at_ms, .. } => {
            Some(*expires_at_ms)
        }
        SourceActor::Configuration { valid_until_ms, .. } => *valid_until_ms,
        SourceActor::Passport { expires_at_ms, .. } => Some(*expires_at_ms),
        SourceActor::ApiKey { expires_at_ms, .. } => *expires_at_ms,
    };
    if expiry.is_some_and(|expiry| expiry <= now) {
        return Err(fail());
    }
    Ok(())
}
async fn observed<S: AuthorityStore>(
    tx: &mut ReadCache<'_, S>,
    key: &RecordKey,
) -> Result<Option<String>, GatewayError> {
    tx.observe_revision(key).await?;
    Ok(tx.revision(key))
}
fn runtime_key() -> RecordKey {
    TupleKey::from_scalar_parts(&[
        ScalarKeyPart::String("uos_ai"),
        ScalarKeyPart::String("runtime_config"),
        ScalarKeyPart::String("v2"),
    ])
    .to_record_key()
}
fn fail() -> GatewayError {
    GatewayError::unavailable(
        "Original-request native actor authority is unavailable or changed.",
        "native_actor_unavailable",
    )
}

fn configuration_principal(
    stored: &StoredNativeActor,
    config: &AuthConfig,
    method: &ConfigMethod,
    token_hash: &Option<String>,
) -> Result<String, GatewayError> {
    Ok(match method {
        ConfigMethod::AuthAllowlist => {
            if stored.scope == NativeActorScope::Admin
                || !config.auth_tokens.iter().any(|token| {
                    Some(sha256_hex(token.expose().as_bytes())).as_ref() == token_hash.as_ref()
                })
            {
                return Err(fail());
            }
            "auth-method:auth_tokens_allowlist".into()
        }
        ConfigMethod::AdminAllowlist => {
            if !config.admin_tokens.iter().any(|token| {
                Some(sha256_hex(token.expose().as_bytes())).as_ref() == token_hash.as_ref()
            }) {
                return Err(fail());
            }
            "auth-method:admin_allowlist".into()
        }
        ConfigMethod::Deno => {
            if token_hash.is_none() {
                return Err(fail());
            }
            "auth-method:deno_deploy_token".into()
        }
        ConfigMethod::Disabled => token_hash.as_ref().map_or_else(
            || "local-auth-disabled".into(),
            |hash| format!("bearer-sha256:{hash}"),
        ),
    })
}

async fn kernel_principal<S: AuthorityStore>(
    tx: &mut ReadCache<'_, S>,
    stored: &StoredNativeActor,
) -> Result<String, GatewayError> {
    let SourceActor::Kernel {
        owner,
        repo,
        scope,
        repo_revision,
        org_revision,
        defaults_revision,
        cutover_revision,
        keys_revision,
        ..
    } = &stored.source
    else {
        return Err(fail());
    };
    let principal = {
        if stored.scope == NativeActorScope::Admin || !matches!(scope.as_str(), "org" | "repo") {
            return Err(fail());
        }
        if observed(tx, &keys::kernel_policy(owner, Some(repo))).await? != *repo_revision
            || observed(tx, &keys::kernel_policy(owner, None)).await? != *org_revision
            || observed(tx, &keys::kernel_defaults()).await? != *defaults_revision
            || observed(tx, &keys::kernel_cutover()).await? != *cutover_revision
            || observed(tx, &upstream::kernel_public_keys_key()).await? != *keys_revision
        {
            return Err(fail());
        }
        format!(
            "github-repo:{}/{}",
            owner.to_lowercase(),
            repo.to_lowercase()
        )
    };
    Ok(principal)
}
