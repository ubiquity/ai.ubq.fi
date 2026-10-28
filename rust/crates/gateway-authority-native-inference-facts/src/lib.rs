//! Complete original inference journal/control/API/routing/source observations. DATA is never an effect grant.
mod cache;
use cache::ReadCache;
use gateway_auth_credentials::provider_credentials::{
    codex_access_expiry_ms, codex_source_pool_key, parse_codex_source_pool,
};
use gateway_auth_native_proof::native_auth::NativeTransportIdentity;
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_authority_actor_currentness::source_actor::SourceActorCurrentData;
use gateway_authority_actor_records::{
    scope::NativeActorScope, source::StoredNativeActorV2, versions,
};
use gateway_authority_api_protocol::{
    ApiKeyPolicy, ApiReservation, ApiUsageRequest, ApiUsageState,
};
use gateway_authority_contracts::validate_time;
use gateway_authority_keys as keys;
use gateway_authority_native_records::{
    owner::RegisteredRenewalOwner,
    rows::{fail, session_key},
};
use gateway_authority_paid_control_records::{
    PAID_CONTROL_CONFIG_VERSION, PaidControlConfig, PaidControlOwner,
};
use gateway_authority_routing_evidence::reader::{RoutingEvidenceReader, RoutingReadMode};
use gateway_authority_routing_policy::RoutingPolicyData;
use gateway_authority_routing_records::types::SubscriptionPool;
use gateway_authority_routing_selection::types::AdmittedAccount;
use gateway_contracts::{
    identity::{PrincipalId, RequestContext},
    model::ModelId,
    provider::UpstreamProvider,
};
use gateway_core::state::{AuthorityStore, RevisionCheck};
use gateway_local_journal::{context::decode_journal_context, local_work::journal_local_work};
use gateway_native_authority_wire::{
    context::NativeRenewalContext, source_fence::SourceNativeRenewalFenceV3,
};
use gateway_schema::catalog_store::runtime_config_key;
use gateway_source::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use serde::{Deserialize, Serialize};
use serde_json::Value;
#[derive(Deserialize)]
struct SessionFields {
    input_digest: String,
    api: Option<ApiReservation>,
    attempts: Vec<Attempt>,
    active: Option<String>,
    refreshed_versions: Vec<String>,
    finished: bool,
    semantic_committed: bool,
    recovering: bool,
    lease_expires_at_ms: i64,
}
struct Session {
    context: RequestContext,
    fields: SessionFields,
}
#[derive(Deserialize)]
struct Attempt {
    id: String,
    provider: UpstreamProvider,
    model: ModelId,
    fence: SourceJson,
    phase: String,
}

pub struct SourceInferenceControlData {
    principal: PrincipalId,
    policy_digest: String,
    admission: AdmittedAccount,
    attempt_id: String,
    model: ModelId,
    native_generation: String,
    original_admission_digest: String,
    checks: Vec<RevisionCheck>,
    observed_at_ms: i64,
}
impl SourceInferenceControlData {
    pub fn principal(&self) -> &PrincipalId {
        &self.principal
    }
    pub fn policy_digest(&self) -> &str {
        &self.policy_digest
    }
    pub fn account_hash(&self) -> &str {
        &self.admission.binding.account_id_hash.0
    }
    pub fn context(&self) -> NativeRenewalContext {
        NativeRenewalContext::Inference {
            attempt_id: self.attempt_id.clone(),
            original_admission_digest: self.original_admission_digest.clone(),
            model: self.model.clone(),
            slot: self.admission.binding.slot,
        }
    }
    pub fn revision_checks(&self) -> &[RevisionCheck] {
        &self.checks
    }
    pub fn observed_at_ms(&self) -> i64 {
        self.observed_at_ms
    }
}
pub struct SourceInferenceFactsData {
    fence: SourceNativeRenewalFenceV3,
    control_checks: Vec<RevisionCheck>,
    account_digests: Vec<String>,
    checks: Vec<RevisionCheck>,
    observed_at_ms: i64,
}
impl SourceInferenceFactsData {
    pub fn fence(&self) -> &SourceNativeRenewalFenceV3 {
        &self.fence
    }
    pub fn control_revision_checks(&self) -> &[RevisionCheck] {
        &self.control_checks
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
pub struct SourceNativeInferenceFactsReader<'a, S> {
    cache: ReadCache<'a, S>,
    identity: &'a NativeTransportIdentity,
    registered: &'a RegisteredRenewalOwner,
}
impl<'a, S: AuthorityStore> SourceNativeInferenceFactsReader<'a, S> {
    pub fn new(
        snapshot: &'a S,
        identity: &'a NativeTransportIdentity,
        registered: &'a RegisteredRenewalOwner,
        now: i64,
    ) -> Result<Self, GatewayError> {
        Ok(Self {
            cache: ReadCache::new(snapshot, now)?,
            identity,
            registered,
        })
    }
    async fn session(&mut self, request_id: &str) -> Result<Session, GatewayError> {
        let raw: SourceJson = self
            .cache
            .read_source_control(&session_key(request_id))
            .await?
            .ok_or_else(fail)?;
        journal_local_work(&raw)?;
        let context = decode_journal_context(&raw).map_err(|_| fail())?;
        let fields = raw.deserialize().map_err(|_| fail())?;
        Ok(Session { context, fields })
    }
    async fn require_phase(
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
        validate_time(self.cache.now)?;
        if actor
            .source_valid_until_ms
            .is_some_and(|at| at <= self.cache.now)
            || actor.source_expiry().is_some_and(|at| at <= self.cache.now)
            || phase
                .minimum_expiry()
                .is_some_and(|at| at <= self.cache.now)
        {
            return Err(fail());
        }
        for check in phase.revision_checks() {
            self.cache.observe_revision(&check.key).await?;
            if !self.cache.expect(&check.key, &check.expected_revision) {
                return Err(fail());
            }
        }
        Ok(())
    }
    pub async fn read_control(
        mut self,
        actor: &StoredNativeActorV2,
        original: &RequestContext,
        phase: &SourceActorCurrentData,
        expected: Option<&NativeRenewalContext>,
    ) -> Result<SourceInferenceControlData, GatewayError> {
        self.control(actor, original, phase, expected).await
    }
    async fn control(
        &mut self,
        actor: &StoredNativeActorV2,
        original: &RequestContext,
        phase: &SourceActorCurrentData,
        expected: Option<&NativeRenewalContext>,
    ) -> Result<SourceInferenceControlData, GatewayError> {
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
        if config.v != PAID_CONTROL_CONFIG_VERSION
            || config.owner_id != self.identity.owner_id()
            || config.owner_epoch != self.identity.owner_epoch()
        {
            return Err(fail());
        }
        let runtime_key = runtime_config_key();
        self.cache.observe_revision(&runtime_key).await?;
        if config.source_checks.len() != 1
            || config.source_checks[0].key != runtime_key.0
            || config.source_checks[0].expected_revision != self.cache.revision(&runtime_key)
        {
            return Err(fail());
        }
        self.require_phase(actor, original, phase).await?;
        if phase.scope() != NativeActorScope::Client {
            return Err(fail());
        }
        let policy_digest = phase.policy_digest().to_owned();
        let request_id = original.request_id.0.as_str();
        let session = self.session(request_id).await?;
        if session.context.request_id.0 != request_id
            || session.context != *original
            || session.fields.finished
            || session.fields.semantic_committed
            || session.fields.recovering
            || session.fields.lease_expires_at_ms <= self.cache.now
        {
            return Err(fail());
        }
        let expected_attempt = match expected {
            Some(NativeRenewalContext::Inference { attempt_id, .. }) => Some(attempt_id.as_str()),
            None => None,
            _ => return Err(fail()),
        };
        let attempt_id = expected_attempt
            .or(session.fields.active.as_deref())
            .ok_or_else(fail)?;
        let attempt = session
            .fields
            .attempts
            .iter()
            .find(|a| a.id == attempt_id)
            .ok_or_else(fail)?;
        if attempt.provider != UpstreamProvider::ChatgptCodex
            || !matches!(attempt.phase.as_str(), "Prepared" | "Intent" | "Started")
        {
            return Err(fail());
        }
        let fence = source::member(&attempt.fence, "Codex").ok_or_else(fail)?;
        let admission: AdmittedAccount = source::member(&fence, "admission")
            .ok_or_else(fail)?
            .deserialize()
            .map_err(|_| fail())?;
        let source_key = codex_source_pool_key();
        let scalar = |field| {
            source::member(&fence, field).and_then(|value| value.deserialize::<String>().ok())
        };
        if scalar("source_key").as_deref() != Some(source_key.0.as_str())
            || scalar("source_revision").as_deref()
                != Some(admission.binding.pool_versionstamp.0.as_str())
            || !session
                .fields
                .refreshed_versions
                .contains(&admission.binding.credential_version.0)
        {
            return Err(fail());
        }
        self.validate_session_api(&session, phase.principal())
            .await?;
        let data = SourceInferenceControlData {
            principal: phase.principal().clone(),
            policy_digest,
            admission,
            attempt_id: attempt_id.into(),
            model: attempt.model.clone(),
            native_generation: scalar("native_generation").ok_or_else(fail)?,
            original_admission_digest: source_admission_digest(
                &session.context,
                &session.fields.input_digest,
                &session.fields.api,
                attempt_id,
            )?,
            checks: self.cache.checks(),
            observed_at_ms: self.cache.now,
        };
        if expected.is_some_and(|context| context != &data.context()) {
            return Err(fail());
        }
        Ok(data)
    }
    async fn validate_session_api(
        &mut self,
        session: &Session,
        principal: &PrincipalId,
    ) -> Result<(), GatewayError> {
        let Some(key_id) = session.context.key_id.as_ref().map(|k| &k.0) else {
            return Ok(());
        };
        if principal.as_source() != &JsString16::from_utf8(format!("api-key:{key_id}")) {
            return Err(fail());
        }
        let api = session.fields.api.as_ref().ok_or_else(fail)?;
        if api.key_id.0 != *key_id
            || api.request_id != session.context.request_id
            || api.route != session.context.route
        {
            return Err(fail());
        }
        let _: ApiKeyPolicy = self
            .cache
            .read(&keys::api_policy(key_id))
            .await?
            .ok_or_else(fail)?;
        if !api.deferred {
            let request: ApiUsageRequest = self
                .cache
                .read(&keys::api_request(
                    key_id,
                    &api.policy_version,
                    api.window_start_ms,
                    &api.request_id.0,
                ))
                .await?
                .ok_or_else(fail)?;
            request.validate()?;
            if request.key_id != api.key_id
                || request.request_id != api.request_id
                || request.route != api.route
                || request.state == ApiUsageState::Released
            {
                return Err(fail());
            }
        }
        Ok(())
    }
    pub async fn read(
        mut self,
        actor: &StoredNativeActorV2,
        original: &RequestContext,
        phase: &SourceActorCurrentData,
        expected: Option<&NativeRenewalContext>,
    ) -> Result<SourceInferenceFactsData, GatewayError> {
        let control = self.control(actor, original, phase, expected).await?;
        let admission = &control.admission;
        self.require_phase(actor, original, phase).await?;
        let source_key = codex_source_pool_key();
        let owner_id = self.identity.owner_id();
        let owner_epoch = self.identity.owner_epoch();
        let native_owner_id = self.registered.native_owner_id();
        let home = self.registered.registered_home();
        let now = self.cache.now;
        let evidence = {
            let snapshot = self.cache.fixed_store();
            RoutingEvidenceReader::new(&snapshot, now)?
                .read(RoutingReadMode::GlobalSelection)
                .await?
        };
        for check in evidence.revision_checks() {
            if !self.cache.expect(&check.key, &check.expected_revision) {
                return Err(gateway_authority_contracts::invalid(
                    "Routing evidence conflicts with an existing transaction fence.",
                ));
            }
        }
        let view = RoutingPolicyData::from_evidence(&evidence);
        if !view.matches_admission(&admission.binding, admission.active_generation) {
            return Err(fail());
        }
        let normalized: SubscriptionPool =
            self.cache.read(&keys::pool()).await?.ok_or_else(fail)?;
        let raw: Value = self.cache.read(&source_key).await?.ok_or_else(fail)?;
        let source = parse_codex_source_pool(&raw)?.ok_or_else(fail)?;
        let slot = usize::try_from(admission.binding.slot).map_err(|_| fail())?;
        let account = source.accounts.get(slot).ok_or_else(fail)?;
        let identity = account.identity();
        let native = account.native_owner.as_ref().ok_or_else(fail)?;
        if native.codex_home != home
            || native.generation_hash != identity.native_generation_hash
            || control.native_generation != identity.native_generation_hash
            || normalized.pool_versionstamp.0 != admission.binding.pool_versionstamp.0
        {
            return Err(fail());
        }
        if native.external_user_id.is_none()
            && (native.managed_source.is_none() || !account.refresh_token.expose().is_empty())
        {
            return Err(fail());
        }
        let physical = native
            .managed_source
            .as_ref()
            .map(|s| s.generation_hash.as_str())
            .unwrap_or(&native.generation_hash);
        let expiry = codex_access_expiry_ms(&account.access_token)
            .filter(|v| v.is_finite() && v.fract() == 0.0 && v.abs() <= 9_007_199_254_740_991.0)
            .map(|v| v as i64);
        let fence = SourceNativeRenewalFenceV3 {
            v: versions::RENEWAL_FENCE_V3,
            owner_id: owner_id.into(),
            owner_epoch: owner_epoch.into(),
            owner_revision: self
                .cache
                .revision(&keys::paid_control_owner())
                .ok_or_else(fail)?,
            native_owner_id: native_owner_id.into(),
            source_revision: self.cache.revision(&source_key).ok_or_else(fail)?,
            normalized_revision: self.cache.revision(&keys::pool()).ok_or_else(fail)?,
            account_id_hash: identity.routing_account_hash.0,
            credential_version: identity.credential_version.0,
            source_native_generation: physical.into(),
            access_generation: native.generation_hash.clone(),
            access_expires_at_ms: expiry,
            native_refreshed_at: native.native_refreshed_at.clone(),
            principal: phase.principal().as_source().clone(),
            principal_policy_digest: control.policy_digest.clone(),
            original_request_id: original.request_id.0.clone(),
            context: gateway_native_authority_wire::context::NativeRenewalContext::Inference {
                attempt_id: control.attempt_id,
                original_admission_digest: control.original_admission_digest,
                model: control.model,
                slot: admission.binding.slot,
            },
        };
        Ok(SourceInferenceFactsData {
            fence,
            control_checks: control.checks,
            checks: self.cache.checks(),
            observed_at_ms: self.cache.now,
            account_digests: source
                .accounts
                .iter()
                .map(|account| legacy_digest(&account.private_source_value()))
                .collect::<Result<_, _>>()?,
        })
    }
}
fn source_admission_digest(
    context: &RequestContext,
    input_digest: &str,
    api: &Option<ApiReservation>,
    attempt_id: &str,
) -> Result<String, GatewayError> {
    let bytes =
        SourceJson::from_serializable(&(context.to_source_json()?, input_digest, api, attempt_id))?;
    Ok(sha256_hex(bytes.as_bytes()))
}
fn legacy_digest<T: Serialize>(value: &T) -> Result<String, GatewayError> {
    Ok(sha256_hex(&serde_json::to_vec(value).map_err(|_| fail())?))
}

#[cfg(test)]
mod tests {
    use super::source_admission_digest;
    use gateway_auth_primitives::crypto::sha256_hex;
    use gateway_contracts::identity::{PrincipalId, RequestContext, RequestId};
    use gateway_source::js_string::JsString16;
    #[test]
    fn actual_source_admission_digest_keeps_the_original_scalar_tuple_bytes()
    -> Result<(), Box<dyn std::error::Error>> {
        let context = RequestContext {
            request_id: RequestId("scalar".into()),
            principal: PrincipalId::from_scalar("passport-user:scalar".into()),
            key_id: None,
            route: "responses".into(),
            started_at_ms: 1_800_000_000_000,
        };
        let api = None;
        let original = serde_json::to_vec(&(&context, "input", &api, "attempt"))?;
        assert_eq!(
            source_admission_digest(&context, "input", &api, "attempt")?,
            sha256_hex(&original)
        );
        Ok(())
    }
    #[test]
    fn actual_source_admission_digest_keeps_lone_units_distinct_from_replacement()
    -> Result<(), Box<dyn std::error::Error>> {
        let context = RequestContext {
            request_id: RequestId("source".into()),
            principal: PrincipalId::from_source(JsString16::from_units(vec![0xd800, 0])),
            key_id: None,
            route: "responses".into(),
            started_at_ms: 1_800_000_000_000,
        };
        let mut replacement = context.clone();
        replacement.principal = PrincipalId::from_source(JsString16::from_units(vec![0xfffd, 0]));
        assert!(serde_json::to_vec(&context).is_err());
        assert_ne!(
            source_admission_digest(&context, "input", &None, "attempt")?,
            source_admission_digest(&replacement, "input", &None, "attempt")?
        );
        Ok(())
    }
}
