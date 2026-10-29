//! Complete same-store source Live2 preparation, authorization, recheck and owned retirement.
mod backend;
use backend::Transaction;
use gateway_auth_contracts::types::{AuthConfig, AuthHost};
use gateway_auth_credentials::provider_credentials::{
    codex_access_expiry_ms, codex_source_pool_key, parse_codex_source_pool,
};
use gateway_auth_native_proof::{
    native_auth::NativeTransportIdentity, native_live::AuthenticatedNativeLive,
};
use gateway_auth_passport_contracts::session::PassportDate;
use gateway_authority_actor_currentness::{
    app::ActorService,
    source_actor::{SourceActorCurrentData, SourceNativeRenewalActor},
};
use gateway_authority_actor_records::scope::NativeActorScope;
use gateway_authority_contracts::{AuthorityClock, TokenSource, text, validate_time};
use gateway_authority_keys as keys;
use gateway_authority_native_records::{
    live::{ConnectionRow, custody_key, mapping_key},
    source_rows::SourceLiveActionRowV2,
};
use gateway_authority_paid_control_records::PaidControlOwner;
use gateway_authority_routing_records::types::SubscriptionPool;
use gateway_core::{
    source_control::decode_source_control_record,
    state::{AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation},
};
use gateway_native_authority_request::live::{self as live_request, NativeLiveAuthorityRequest};
use gateway_native_authority_wire::{
    live::NativeLiveRetirement, source_fence::SourceNativeLiveFenceV2,
    source_response::SourceNativeLiveAuthorityResponseV2,
};
use gateway_source::{
    error::GatewayError, source_json::SourceJson, source_serialization as source,
};
use serde::Serialize;
use serde_json::Value;
/// Ephemeral access-only response; only this complete checked operation can issue it.
/// ```compile_fail
/// use gateway_authority_native_live::SourceNativeLiveAccess;
/// let _:SourceNativeLiveAccess=serde_json::from_str("{}").unwrap();
/// ```
#[derive(Serialize)]
#[serde(deny_unknown_fields)]
pub struct SourceNativeLiveAccess {
    access_token: String,
}
impl SourceNativeLiveAccess {
    fn current(access_token: String) -> Self {
        Self { access_token }
    }
    pub fn expose(&self) -> &str {
        &self.access_token
    }
}
/// Original server-created selector; no DATA constructor, Clone or deserialization.
pub struct SourceNativeLiveAction {
    action_ref: String,
}
impl SourceNativeLiveAction {
    pub fn action_ref(&self) -> &str {
        &self.action_ref
    }
}
pub struct SourceNativeLiveService<'a, S, I, H> {
    store: &'a S,
    tokens: &'a I,
    clock: Option<&'a dyn AuthorityClock>,
    identity: &'a NativeTransportIdentity,
    actors: ActorService<'a, S, H>,
}
impl<'a, S: AuthorityStore, I: TokenSource, H: AuthHost + PassportDate>
    SourceNativeLiveService<'a, S, I, H>
{
    pub fn new(
        store: &'a S,
        tokens: &'a I,
        clock: Option<&'a dyn AuthorityClock>,
        identity: &'a NativeTransportIdentity,
        host: &'a H,
        config: &'a AuthConfig,
    ) -> Self {
        Self {
            store,
            tokens,
            clock,
            identity,
            actors: ActorService::new(store, host, config),
        }
    }
    fn time(&self, anchor: i64) -> Result<i64, GatewayError> {
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
    fn transaction(&self, now: i64) -> Result<Transaction<'_, S, I>, GatewayError> {
        Transaction::new(self.store, self.tokens, now)
    }
    fn lifetime(
        &self,
        row: &SourceLiveActionRowV2,
        phase: &SourceActorCurrentData,
        now: i64,
    ) -> Result<(), GatewayError> {
        if row.v != 2
            || row.actor.scope != NativeActorScope::Client
            || phase.scope() != NativeActorScope::Client
            || &row.actor.principal != phase.principal()
            || row.actor.principal != row.context.principal
            || row.actor.policy_digest()? != phase.policy_digest()
            || row.actor.source_valid_until_ms.is_some_and(|at| at <= now)
            || row.actor.source_expiry().is_some_and(|at| at <= now)
            || phase.minimum_expiry().is_some_and(|at| at <= now)
        {
            return Err(fail());
        }
        Ok(())
    }
    async fn load(&self, reference: &str, now: i64) -> Result<SourceLiveActionRowV2, GatewayError> {
        let record = self
            .store
            .read(&custody_key("action", reference))
            .await?
            .ok_or_else(fail)?;
        record.validate_version()?;
        text(&record.revision)?;
        if let Some(expiry) = record.expires_at_ms {
            validate_time(expiry)?;
            if expiry <= now {
                return Err(fail());
            }
        }
        let row = SourceLiveActionRowV2::from_source_json(&decode_source_control_record(&record)?)?;
        if row.action_ref != reference {
            return Err(fail());
        }
        Ok(row)
    }
    /// Discover DATA, then obtain a genuine fresh C phase before constructing the private effect snapshot.
    async fn begin(
        &self,
        reference: &str,
    ) -> Result<
        (
            Transaction<'_, S, I>,
            SourceLiveActionRowV2,
            SourceActorCurrentData,
        ),
        GatewayError,
    > {
        let now = self.time(0)?;
        let before = self.load(reference, now).await?;
        let phase = self
            .actors
            .check_stored_source_phase(
                &before.actor,
                &before.context,
                NativeActorScope::Client,
                self.time(now)?,
            )
            .await?;
        let mut tx = self.transaction(self.time(now)?)?;
        hydrate(&mut tx, &phase).await?;
        let raw: SourceJson = tx
            .read_source_control(&custody_key("action", reference))
            .await?
            .ok_or_else(fail)?;
        let row = SourceLiveActionRowV2::from_source_json(&raw)?;
        if row.action_ref != reference || row.to_source_json()? != before.to_source_json()? {
            return Err(fail());
        }
        Ok((tx, row, phase))
    }
    pub async fn prepare_live_sideband(
        &self,
        actor: &SourceNativeRenewalActor<'_, S>,
        call_id: &str,
    ) -> Result<SourceNativeLiveAction, GatewayError> {
        if actor.scope() != NativeActorScope::Client || call_id.is_empty() || call_id.len() > 4096 {
            return Err(fail());
        }
        for _ in 0..5 {
            let phase = self
                .actors
                .prepare_source_phase(actor, NativeActorScope::Client, self.time(0)?)
                .await?;
            let now = self.time(0)?;
            let mut tx = self.transaction(now)?;
            hydrate(&mut tx, phase.current_data()).await?;
            let reference = self.tokens.next_token()?;
            if !live_request::reference(&reference) {
                return Err(fail());
            }
            let key = custody_key("action", &reference);
            let prior: Option<SourceJson> = tx.read_source_control(&key).await?;
            if prior.is_some() {
                return Err(fail());
            }
            let mapping: SourceJson = tx
                .read_source_control(&mapping_key(call_id, false))
                .await?
                .ok_or_else(missing)?;
            let row = SourceLiveActionRowV2 {
                v: 2,
                action_ref: reference.clone(),
                request_id: phase.context().request_id.0.clone(),
                actor: phase.actor_data().clone(),
                context: phase.context().clone(),
                call_id: call_id.into(),
                account_id: source::member(&mapping, "account_id")
                    .ok_or_else(missing)?
                    .deserialize()
                    .map_err(|_| missing())?,
                mapping_revision: tx
                    .revision(&mapping_key(call_id, false))
                    .ok_or_else(missing)?,
                created_at_ms: now,
            };
            let _ = self.current(&mut tx, &row, phase.current_data()).await?;
            tx.put_source_control(&key, &row.to_source_json()?, None)?;
            if tx.commit().await? {
                return Ok(SourceNativeLiveAction {
                    action_ref: reference,
                });
            }
        }
        Err(conflict())
    }
    async fn current(
        &self,
        tx: &mut Transaction<'_, S, I>,
        row: &SourceLiveActionRowV2,
        phase: &SourceActorCurrentData,
    ) -> Result<(SourceNativeLiveFenceV2, SourceNativeLiveAccess), GatewayError> {
        if row.v != 2 {
            return Err(fail());
        }
        self.lifetime(row, phase, tx.now)?;
        let actor = phase;
        if actor.scope() != NativeActorScope::Client {
            return Err(fail());
        }
        let pending: Option<Value> = tx.read(&mapping_key(&row.call_id, true)).await?;
        if pending.is_some() {
            return Err(missing());
        }
        let map_key = mapping_key(&row.call_id, false);
        let mapping: SourceJson = tx
            .read_source_control(&map_key)
            .await?
            .ok_or_else(missing)?;
        if source::member(&mapping, "account_id")
            .and_then(|value| value.deserialize::<String>().ok())
            .as_deref()
            != Some(row.account_id.as_str())
            || source::member(&mapping, "principal_id")
                .as_ref()
                .and_then(source::string)
                .as_ref()
                != Some(actor.principal().as_source())
            || tx.revision(&map_key).as_deref() != Some(&row.mapping_revision)
        {
            return Err(missing());
        }
        let owner: PaidControlOwner = tx
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
        let source_key = codex_source_pool_key();
        let raw: Value = tx.read(&source_key).await?.ok_or_else(fail)?;
        let pool = parse_codex_source_pool(&raw)?.ok_or_else(fail)?;
        let normalized: SubscriptionPool = tx.read(&keys::pool()).await?.ok_or_else(fail)?;
        gateway_authority_routing_evidence::validation::validate_pool(&normalized)?;
        let (slot, account) = pool
            .accounts
            .iter()
            .enumerate()
            .find(|(_, a)| a.account_id == row.account_id)
            .ok_or_else(missing)?;
        let identity = account.identity();
        let projected = normalized.accounts.get(slot).ok_or_else(fail)?;
        if normalized.pool_versionstamp.0 != tx.revision(&source_key).ok_or_else(fail)?
            || projected.account_id_hash != identity.routing_account_hash
            || projected.admin_account_hash != identity.administrative_account_hash
            || projected.credential_version != identity.credential_version
        {
            return Err(fail());
        }
        let expiry = codex_access_expiry_ms(&account.access_token)
            .filter(|v| {
                v.is_finite() && v.fract() == 0.0 && *v >= 0.0 && *v <= 9_007_199_254_740_991.0
            })
            .map(|v| v as i64);
        let now = self.time(tx.now)?;
        self.lifetime(row, phase, now)?;
        if tx.expiry(&map_key).is_some_and(|at| at <= now) {
            return Err(missing());
        }
        if expiry.is_some_and(|at| at <= now) {
            return Err(GatewayError::new(
                503,
                "Current live access requires separate credential preparation.",
                Some("native_live_access_expired"),
            ));
        }
        tx.now = now;
        Ok((
            SourceNativeLiveFenceV2 {
                v: 2,
                call_id: row.call_id.clone(),
                account_id: row.account_id.clone(),
                account_id_hash: identity.routing_account_hash.0,
                credential_version: identity.credential_version.0,
                source_revision: tx.revision(&source_key).ok_or_else(fail)?,
                normalized_revision: tx.revision(&keys::pool()).ok_or_else(fail)?,
                mapping_revision: row.mapping_revision.clone(),
                owner_id: owner.owner_id,
                owner_epoch: owner.owner_epoch,
                owner_revision: tx.revision(&keys::paid_control_owner()).ok_or_else(fail)?,
                principal: actor.principal().as_source().clone(),
                principal_policy_digest: actor.policy_digest().to_owned(),
                access_expires_at_ms: expiry,
            },
            SourceNativeLiveAccess::current(account.access_token.expose().to_owned()),
        ))
    }
    fn proof(&self, proof: &AuthenticatedNativeLive, body: &[u8]) -> Result<(), GatewayError> {
        proof.require_authority_request(body, self.time(0)?)?;
        if proof.owner_id() != self.identity.owner_id()
            || proof.owner_epoch() != self.identity.owner_epoch()
            || proof.transport_key_fingerprint() != self.identity.key_fingerprint()
        {
            return Err(fail());
        }
        Ok(())
    }
    pub async fn execute(
        &self,
        proof: &AuthenticatedNativeLive,
        body: &[u8],
    ) -> Result<SourceNativeLiveAuthorityResponseV2<SourceNativeLiveAccess>, GatewayError> {
        self.proof(proof, body)?;
        match NativeLiveAuthorityRequest::parse(body)? {
            NativeLiveAuthorityRequest::Authorize {
                action_ref,
                connection_id,
                ..
            } => {
                self.authorize(proof, body, &action_ref, &connection_id)
                    .await
            }
            NativeLiveAuthorityRequest::Recheck {
                action_ref,
                connection_id,
                authorization_ref,
                ..
            } => {
                self.recheck(proof, body, &action_ref, &connection_id, &authorization_ref)
                    .await
            }
            NativeLiveAuthorityRequest::Retire {
                action_ref,
                connection_id,
                authorization_ref,
                upstream_status,
                ..
            } => {
                let outcome = self
                    .retire(
                        proof,
                        body,
                        &action_ref,
                        &connection_id,
                        &authorization_ref,
                        upstream_status,
                    )
                    .await?;
                Ok(SourceNativeLiveAuthorityResponseV2::Retire { v: 2, outcome })
            }
        }
    }
    async fn authorize(
        &self,
        proof: &AuthenticatedNativeLive,
        body: &[u8],
        action_ref: &str,
        connection_id: &str,
    ) -> Result<SourceNativeLiveAuthorityResponseV2<SourceNativeLiveAccess>, GatewayError> {
        for _ in 0..5 {
            self.proof(proof, body)?;
            let (mut tx, action, phase) = self.begin(action_ref).await?;
            let (fence, access) = self.current(&mut tx, &action, &phase).await?;
            let key = custody_key("connection", connection_id);
            let existing: Option<ConnectionRow> = tx.read(&key).await?;
            let connection = if let Some(connection) = existing {
                self.connection(&connection, action_ref, connection_id, None)?;
                if connection.upstream_status.is_some() || connection.retired {
                    return Err(fail());
                }
                connection
            } else {
                ConnectionRow {
                    v: 1,
                    action_ref: action_ref.into(),
                    connection_id: connection_id.into(),
                    authorization_ref: self.tokens.next_token()?,
                    owner_id: self.identity.owner_id().into(),
                    owner_epoch: self.identity.owner_epoch().into(),
                    key_fingerprint: self.identity.key_fingerprint().into(),
                    call_id: action.call_id,
                    created_at_ms: tx.now,
                    upstream_status: None,
                    retired: false,
                }
            };
            tx.put(&key, &connection, None)?;
            self.proof(proof, body)?;
            if tx.commit().await? {
                return Ok(SourceNativeLiveAuthorityResponseV2::Authorize {
                    v: 2,
                    authorization_ref: connection.authorization_ref,
                    fence,
                    access,
                });
            }
        }
        Err(conflict())
    }
    fn connection(
        &self,
        row: &ConnectionRow,
        action: &str,
        connection: &str,
        authorization: Option<&str>,
    ) -> Result<(), GatewayError> {
        if row.v != 1
            || row.action_ref != action
            || row.connection_id != connection
            || authorization.is_some_and(|r| r != row.authorization_ref)
            || row.owner_id != self.identity.owner_id()
            || row.owner_epoch != self.identity.owner_epoch()
            || row.key_fingerprint != self.identity.key_fingerprint()
        {
            return Err(fail());
        }
        Ok(())
    }
    async fn recheck(
        &self,
        proof: &AuthenticatedNativeLive,
        body: &[u8],
        action_ref: &str,
        connection_id: &str,
        authorization_ref: &str,
    ) -> Result<SourceNativeLiveAuthorityResponseV2<SourceNativeLiveAccess>, GatewayError> {
        for _ in 0..5 {
            // Keep the original connection predicates before central authentication.
            let mut discovery = self.transaction(self.time(0)?)?;
            let connection: ConnectionRow = discovery
                .read(&custody_key("connection", connection_id))
                .await?
                .ok_or_else(fail)?;
            self.connection(
                &connection,
                action_ref,
                connection_id,
                Some(authorization_ref),
            )?;
            if connection.upstream_status.is_some() || connection.retired {
                return Err(fail());
            }
            drop(discovery);
            let (mut tx, action, phase) = self.begin(action_ref).await?;
            let connection: ConnectionRow = tx
                .read(&custody_key("connection", connection_id))
                .await?
                .ok_or_else(fail)?;
            self.connection(
                &connection,
                action_ref,
                connection_id,
                Some(authorization_ref),
            )?;
            if connection.upstream_status.is_some() || connection.retired {
                return Err(fail());
            }
            if action.action_ref != action_ref || action.call_id != connection.call_id {
                return Err(fail());
            }
            let (fence, access) = self.current(&mut tx, &action, &phase).await?;
            self.proof(proof, body)?;
            if tx.commit().await? {
                return Ok(SourceNativeLiveAuthorityResponseV2::Recheck {
                    v: 2,
                    fence,
                    access,
                });
            }
        }
        Err(conflict())
    }
    async fn retire(
        &self,
        proof: &AuthenticatedNativeLive,
        body: &[u8],
        action_ref: &str,
        connection_id: &str,
        authorization_ref: &str,
        status: u16,
    ) -> Result<NativeLiveRetirement, GatewayError> {
        let connection_key = custody_key("connection", connection_id);
        let mut call = None;
        for _ in 0..5 {
            self.proof(proof, body)?;
            let mut tx = self.transaction(self.time(0)?)?;
            let mut row: ConnectionRow = tx.read(&connection_key).await?.ok_or_else(fail)?;
            self.connection(&row, action_ref, connection_id, Some(authorization_ref))?;
            if row.upstream_status.is_some_and(|s| s != status) {
                return Err(fail());
            }
            if row.retired {
                return Ok(NativeLiveRetirement::Retired);
            }
            // Genuine prior-effect cleanup does not need current actor, mapping or credentials.
            let marker = mapping_key(&row.call_id, true);
            let prior: Option<Value> = tx.read(&marker).await?;
            if prior.as_ref().is_some_and(|v| {
                v["schema_version"] != 1
                    || v["call_id"] != row.call_id
                    || !matches!(v["upstream_status"].as_u64(), Some(404 | 410))
            }) {
                return Ok(NativeLiveRetirement::Unknown);
            }
            row.upstream_status = Some(status);
            tx.put(&connection_key, &row, None)?;
            if prior.is_none() {
                tx.put(&marker,&serde_json::json!({"schema_version":1,"call_id":row.call_id,"upstream_status":status,"created_at_ms":tx.now}),None)?;
            }
            match tx.commit().await {
                Ok(true) => {
                    call = Some(row.call_id);
                    break;
                }
                Ok(false) => {}
                Err(_) => return Ok(NativeLiveRetirement::Unknown),
            }
        }
        let Some(call) = call else {
            return Ok(NativeLiveRetirement::Pending);
        };
        // Source deletion is unconditional, including a newer mapping of the same call ID.
        if self
            .store
            .commit(AtomicTransition {
                checks: vec![],
                mutations: vec![RecordMutation::Delete {
                    key: mapping_key(&call, false),
                }],
            })
            .await
            != Ok(CommitOutcome::Committed)
        {
            return Ok(NativeLiveRetirement::Pending);
        }
        let mut tx = self.transaction(self.time(0)?)?;
        let marker = mapping_key(&call, true);
        let _: Option<Value> = tx.read(&marker).await?;
        let mut row: ConnectionRow = tx.read(&connection_key).await?.ok_or_else(fail)?;
        self.connection(&row, action_ref, connection_id, Some(authorization_ref))?;
        row.retired = true;
        tx.put(&connection_key, &row, None)?;
        tx.delete(&marker)?;
        match tx.commit().await {
            Ok(true) => Ok(NativeLiveRetirement::Retired),
            _ => Ok(NativeLiveRetirement::Pending),
        }
    }
}
async fn hydrate<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    phase: &SourceActorCurrentData,
) -> Result<(), GatewayError> {
    for check in phase.revision_checks() {
        tx.observe_revision(&check.key).await?;
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(fail());
        }
    }
    Ok(())
}
fn fail() -> GatewayError {
    GatewayError::new(
        403,
        "Current native live authority is unavailable.",
        Some("native_live_authority_stale"),
    )
}
fn missing() -> GatewayError {
    GatewayError::new(
        404,
        "Live call mapping is unavailable.",
        Some("live_call_not_found"),
    )
}
fn conflict() -> GatewayError {
    GatewayError::new(
        409,
        "Live authority changed concurrently.",
        Some("native_live_conflict"),
    )
}
