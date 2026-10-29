//! Exact historical scalar actor-v1 DTOs and byte recipes; DATA is not a live actor.
use crate::scope::NativeActorScope;
use gateway_auth_contracts::types::AuthConfig;
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_source::error::GatewayError;
use serde::{Deserialize, Serialize};
use serde_json::json;
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct StoredNativeActor {
    pub v: u8,
    pub principal: String,
    pub scope: NativeActorScope,
    pub config_digest: String,
    pub owner_revision: Option<String>,
    pub config_revision: Option<String>,
    pub runtime_revision: Option<String>,
    #[serde(deserialize_with = "required_expiry")]
    pub source_valid_until_ms: Option<i64>,
    pub source: SourceActor,
}
impl StoredNativeActor {
    pub fn principal(&self) -> &str {
        &self.principal
    }
    pub fn scope(&self) -> NativeActorScope {
        self.scope
    }
}
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
pub enum SourceActor {
    ApiKey {
        key_id: String,
        token_hash: String,
        hash_revision: String,
        policy_revision: Option<String>,
        #[serde(deserialize_with = "required_expiry")]
        expires_at_ms: Option<i64>,
    },
    Session {
        digest: String,
        index_revision: String,
        session_revision: String,
        user_id: String,
        user_revision: String,
        expires_at_ms: i64,
    },
    Configuration {
        method: ConfigMethod,
        token_hash: Option<String>,
        valid_until_ms: Option<i64>,
    },
    Passport {
        sub: String,
        jti: String,
        expires_at_ms: i64,
    },
    Kernel {
        owner: String,
        repo: String,
        state_id: String,
        scope: String,
        expires_at_ms: i64,
        repo_revision: Option<String>,
        org_revision: Option<String>,
        defaults_revision: Option<String>,
        cutover_revision: Option<String>,
        keys_revision: Option<String>,
    },
}
#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ConfigMethod {
    AuthAllowlist,
    AdminAllowlist,
    Deno,
    Disabled,
}

fn required_expiry<'de, D: serde::Deserializer<'de>>(
    deserializer: D,
) -> Result<Option<i64>, D::Error> {
    Option::<i64>::deserialize(deserializer)
}
fn digest(value: &impl Serialize) -> Result<String, GatewayError> {
    Ok(sha256_hex(&serde_json::to_vec(value).map_err(|_| fail())?))
}
pub fn config_digest(config: &AuthConfig) -> Result<String, GatewayError> {
    digest(
        &json!({"v":1,"auth":config.auth_tokens.iter().map(|secret|sha256_hex(secret.expose().as_bytes())).collect::<Vec<_>>(),"admin":config.admin_tokens.iter().map(|secret|sha256_hex(secret.expose().as_bytes())).collect::<Vec<_>>(),"production":config.production,"hosted":config.hosted,"git":config.git_sha_known,"candidate":config.candidate_passkeys,"relay":config.extra_relay_origins,"kernel":config.kernel_public_keys_pem.iter().map(|pem|sha256_hex(pem.as_bytes())).collect::<Vec<_>>(),"app":config.deno_app_slug,"org":config.deno_org_slug,"deployment":config.deno_deployment_id}),
    )
}
pub fn policy_digest(value: &StoredNativeActor) -> Result<String, GatewayError> {
    digest(value)
}
fn fail() -> GatewayError {
    GatewayError::unavailable(
        "Original-request native actor authority is unavailable or changed.",
        "native_actor_unavailable",
    )
}
/// Existing source expiry DATA only; no clock or currentness is consulted.
pub fn source_expiry(value: &SourceActor) -> Option<i64> {
    match value {
        SourceActor::Session { expires_at_ms, .. }
        | SourceActor::Kernel { expires_at_ms, .. }
        | SourceActor::Passport { expires_at_ms, .. } => Some(*expires_at_ms),
        SourceActor::Configuration { valid_until_ms, .. } => *valid_until_ms,
        SourceActor::ApiKey { expires_at_ms, .. } => *expires_at_ms,
    }
}

/// Historical scalar JWT provenance DATA only. Actual AuthService authentication remains mandatory.
pub fn passport_source(
    identity: &gateway_auth_passport_contracts::actor_data::PassportSessionData,
) -> Result<SourceActor, GatewayError> {
    use gateway_auth_passport_contracts::actor_data::PassportSessionData;
    let PassportSessionData::Jwt(claims) = identity else {
        return Err(fail());
    };
    if !claims.is_admin || claims.agent_token_id.is_some() {
        return Err(fail());
    }
    let legacy = claims.try_legacy_scalar().map_err(|_| fail())?;
    Ok(SourceActor::Passport {
        sub: legacy.sub,
        jti: legacy.jti,
        expires_at_ms: legacy.exp.checked_mul(1000).ok_or_else(fail)?,
    })
}
