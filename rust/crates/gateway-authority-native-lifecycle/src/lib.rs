//! Complete actual source3 native claim/Start/currentness/terminal/unknown lifecycle.
mod backend;
use backend::Transaction;
use gateway_auth_contracts::types::{AuthConfig, AuthHost};
use gateway_auth_native_proof::native_auth::{
    AuthenticatedNativeOwner, NativeCapability, NativeTransportIdentity,
};
use gateway_auth_native_wire::{
    control::{NATIVE_RENEWAL_MAX_MS, NativeControlEnvelope, NativeControlRequest},
    native_read::{NATIVE_CONTROL_PATH, NATIVE_RENEWAL_AUTHORITY_PATH},
};
use gateway_auth_passport_contracts::session::PassportDate;
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_authority_actor_currentness::{
    app::ActorService, source_actor::SourceActorCurrentData,
};
use gateway_authority_actor_records::{scope::NativeActorScope, versions};
use gateway_authority_contracts::{
    AuthorityClock, NativeRenewalCancellation, TokenSource, add, text, validate_time,
};
use gateway_authority_native_context_facts::SourceNativeContextFactsReader;
use gateway_authority_native_records::{
    owner::RegisteredRenewalOwner,
    rows::{self, GenerationEffect, OperationRow, Phase, TerminalPublication, conflict, fail},
    source_rows::SourceRenewalActionRowV3,
};
use gateway_core::{
    source_control::decode_source_control_record,
    state::{AuthorityStore, StoredRecord},
};
use gateway_native_authority_request::NativeRenewalAuthorityRequest;
use gateway_native_authority_wire::{
    context::NativeRenewalContext, renewal::NativeRenewalMode,
    source_fence::SourceNativeRenewalFenceV3,
};
use gateway_source::{error::GatewayError, source_json::SourceJson};
/// Constructor-held trusted capabilities; selectors cannot choose an owner or native home.
pub struct SourceNativeLifecycleContext<'a, C> {
    identity: &'a NativeTransportIdentity,
    registered: &'a RegisteredRenewalOwner,
    cancellation: &'a C,
}
impl<'a, C> SourceNativeLifecycleContext<'a, C> {
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
/// Only the complete same-store claim CAS mints this single-use start capability.
/// ```compile_fail
/// use gateway_authority_native_lifecycle::SourceNativeRenewalClaim;
/// let _:SourceNativeRenewalClaim<'_,()> = serde_json::from_str("{}").unwrap();
/// ```
pub struct SourceNativeRenewalClaim<'a, S> {
    store: &'a S,
    claim_ref: String,
    operation_id: String,
    action_ref: String,
    deadline_ms: i64,
    fence: SourceNativeRenewalFenceV3,
    mode: NativeRenewalMode,
    start_permit: Option<String>,
    publication_revision: Option<String>,
}
impl<S> SourceNativeRenewalClaim<'_, S> {
    pub fn claim_ref(&self) -> &str {
        &self.claim_ref
    }
    pub fn operation_id(&self) -> &str {
        &self.operation_id
    }
    pub fn action_ref(&self) -> &str {
        &self.action_ref
    }
    pub fn deadline_ms(&self) -> i64 {
        self.deadline_ms
    }
    pub fn fence(&self) -> &SourceNativeRenewalFenceV3 {
        &self.fence
    }
    pub fn mode(&self) -> NativeRenewalMode {
        self.mode
    }
    pub fn start_permit(&self) -> Option<&str> {
        self.start_permit.as_deref()
    }
    pub fn publication_revision(&self) -> Option<&str> {
        self.publication_revision.as_deref()
    }
}
pub struct SourceNativeRenewalPublished {
    publication_revision: String,
}
impl SourceNativeRenewalPublished {
    pub fn publication_revision(&self) -> &str {
        &self.publication_revision
    }
}
pub struct SourceNativeLifecycleService<'a, S, I, H, C> {
    store: &'a S,
    tokens: &'a I,
    clock: Option<&'a dyn AuthorityClock>,
    actors: ActorService<'a, S, H>,
    context: SourceNativeLifecycleContext<'a, C>,
}
impl<
    'a,
    S: AuthorityStore,
    I: TokenSource,
    H: AuthHost + PassportDate,
    C: NativeRenewalCancellation,
> SourceNativeLifecycleService<'a, S, I, H, C>
{
    pub fn new(
        store: &'a S,
        tokens: &'a I,
        clock: Option<&'a dyn AuthorityClock>,
        host: &'a H,
        config: &'a AuthConfig,
        context: SourceNativeLifecycleContext<'a, C>,
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
    fn time(&self, anchor: i64) -> Result<i64, GatewayError> {
        if self.context.cancellation.cancelled() {
            return Err(GatewayError::new(
                499,
                "Native renewal was cancelled.",
                Some("request_cancelled"),
            ));
        }
        self.cleanup_time(anchor)
    }
    fn cleanup_time(&self, anchor: i64) -> Result<i64, GatewayError> {
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
    fn row_time(
        &self,
        row: &SourceRenewalActionRowV3,
        phase: &SourceActorCurrentData,
        now: i64,
    ) -> Result<(), GatewayError> {
        validate_time(now)?;
        if row.v != versions::RENEWAL_ACTION_V3
            || now >= row.deadline_ms
            || row.deadline_ms > add(row.created_at_ms, NATIVE_RENEWAL_MAX_MS)?
            || row.actor.source_valid_until_ms.is_some_and(|at| at <= now)
            || row.actor.source_expiry().is_some_and(|at| at <= now)
            || phase.minimum_expiry().is_some_and(|at| at <= now)
        {
            return Err(fail());
        }
        Ok(())
    }
    async fn load_action(
        &self,
        reference: &str,
        now: i64,
    ) -> Result<SourceRenewalActionRowV3, GatewayError> {
        text(reference)?;
        let raw = self
            .store
            .read(&rows::key("action", reference))
            .await?
            .ok_or_else(fail)?;
        metadata(&raw, now)?;
        let row = SourceRenewalActionRowV3::from_source_json(&decode_source_control_record(&raw)?)?;
        if row.action_ref != reference {
            return Err(fail());
        }
        Ok(row)
    }
    async fn action(
        &self,
        tx: &mut Transaction<'_, S, I>,
        reference: &str,
    ) -> Result<SourceRenewalActionRowV3, GatewayError> {
        text(reference)?;
        let value: SourceJson = tx
            .read_source_control(&rows::key("action", reference))
            .await?
            .ok_or_else(fail)?;
        let row = SourceRenewalActionRowV3::from_source_json(&value)?;
        if row.action_ref != reference {
            return Err(fail());
        }
        Ok(row)
    }
    /// The row is ordinary discovery DATA; real C runs outside the private effect transaction on every retry.
    async fn begin(
        &self,
        reference: &str,
        anchor: i64,
    ) -> Result<
        (
            Transaction<'_, S, I>,
            SourceRenewalActionRowV3,
            SourceActorCurrentData,
        ),
        GatewayError,
    > {
        let observed = self.time(anchor)?;
        let before = self.load_action(reference, observed).await?;
        let checked_at = self.time(observed)?;
        let scope = if matches!(
            before.fence.context,
            NativeRenewalContext::CacheExperiment { .. }
        ) {
            NativeActorScope::Admin
        } else {
            NativeActorScope::Client
        };
        let phase = self
            .actors
            .check_stored_source_phase(&before.actor, &before.context, scope, checked_at)
            .await?;
        let mut tx = self.transaction(self.time(checked_at)?)?;
        let row = self.action(&mut tx, reference).await?;
        if row.actor != before.actor || row.context != before.context {
            return Err(fail());
        }
        for check in phase.revision_checks() {
            tx.observe_revision(&check.key).await?;
            if !tx.expect(&check.key, &check.expected_revision) {
                return Err(fail());
            }
        }
        self.row_time(&row, &phase, self.time(tx.now)?)?;
        Ok((tx, row, phase))
    }
    async fn current(
        &self,
        tx: &mut Transaction<'_, S, I>,
        row: &SourceRenewalActionRowV3,
        phase: &SourceActorCurrentData,
    ) -> Result<SourceNativeRenewalFenceV3, GatewayError> {
        self.row_time(row, phase, self.time(row.created_at_ms)?)?;
        let now = tx.now;
        let data = {
            let snapshot = tx.routing_read_store();
            SourceNativeContextFactsReader::new(
                &snapshot,
                self.context.identity,
                self.context.registered,
                self.clock,
                self.context.cancellation,
                now,
            )?
            .read_context(&row.actor, &row.context, phase, &row.fence.context)
            .await?
        };
        for check in data.revision_checks() {
            if !tx.expect(&check.key, &check.expected_revision) {
                return Err(fail());
            }
        }
        tx.now = data.observed_at_ms();
        self.row_time(row, phase, self.time(tx.now)?)?;
        if data.fence() != &row.fence
            || data.checks_digest() != row.checks_digest
            || data.control_checks_digest() != row.control_checks_digest
            || data.account_digests() != row.account_digests
        {
            return Err(fail());
        }
        Ok(data.fence().clone())
    }
    async fn terminal(
        &self,
        tx: &mut Transaction<'_, S, I>,
        row: &SourceRenewalActionRowV3,
        phase: &SourceActorCurrentData,
        terminal: &TerminalPublication,
    ) -> Result<SourceNativeRenewalFenceV3, GatewayError> {
        let now = tx.now;
        let data = {
            let snapshot = tx.routing_read_store();
            SourceNativeContextFactsReader::new(
                &snapshot,
                self.context.identity,
                self.context.registered,
                self.clock,
                self.context.cancellation,
                now,
            )?
            .read_terminal(row, phase, terminal)
            .await?
        };
        for check in data.revision_checks() {
            if !tx.expect(&check.key, &check.expected_revision) {
                return Err(fail());
            }
        }
        tx.now = data.observed_at_ms();
        self.row_time(row, phase, self.time(tx.now)?)?;
        Ok(data.fence().clone())
    }
    async fn operation(
        &self,
        tx: &mut Transaction<'_, S, I>,
        claim: &SourceNativeRenewalClaim<'_, S>,
    ) -> Result<(SourceRenewalActionRowV3, OperationRow), GatewayError> {
        if !std::ptr::eq(self.store, claim.store) {
            return Err(fail());
        }
        let action = self.action(tx, &claim.action_ref).await?;
        let row: OperationRow = tx
            .read(&rows::key("operation", &claim.operation_id))
            .await?
            .ok_or_else(fail)?;
        if row.v != 1
            || row.operation_id != claim.operation_id
            || row.action_ref != action.action_ref
            || row.claim_ref != claim.claim_ref
            || action.operation_id.as_ref() != Some(&claim.operation_id)
            || action.fence != claim.fence
        {
            return Err(conflict());
        }
        Ok((action, row))
    }
    pub async fn claim(
        &self,
        proof: &AuthenticatedNativeOwner,
        reference: &str,
        operation_id: &str,
        exact_body: &[u8],
    ) -> Result<SourceNativeRenewalClaim<'a, S>, GatewayError> {
        let now = self.time(0)?;
        proof.require_request(
            NativeCapability::OwnerRenewal,
            NATIVE_CONTROL_PATH,
            exact_body,
            now,
        )?;
        if proof.owner_id() != self.context.identity.owner_id()
            || proof.owner_epoch() != self.context.identity.owner_epoch()
            || proof.transport_key_fingerprint() != self.context.identity.key_fingerprint()
        {
            return Err(fail());
        }
        let body = NativeControlEnvelope::parse(exact_body)?;
        let NativeControlRequest::CodexRenewAccess { action_ref } = body.request;
        if body.operation_id != operation_id || action_ref != reference {
            return Err(conflict());
        }
        let digest = sha256_hex(exact_body);
        for _ in 0..5 {
            let (mut tx, mut action, phase) = self.begin(reference, now).await?;
            if action
                .operation_id
                .as_ref()
                .is_some_and(|id| id != operation_id)
            {
                return Err(conflict());
            }
            let (operation, mode, start_permit) = self
                .claim_operation(&mut tx, &action, operation_id, &digest, &phase)
                .await?;
            if action.operation_id.is_none() {
                action.operation_id = Some(operation_id.into());
                tx.put_source_control(
                    &rows::key("action", reference),
                    &action.to_source_json()?,
                    None,
                )?;
            }
            if let Some(terminal) = &operation.terminal {
                self.terminal(&mut tx, &action, &phase, terminal).await?;
            } else {
                self.current(&mut tx, &action, &phase).await?;
            }
            if tx.commit().await? {
                return Ok(SourceNativeRenewalClaim {
                    store: self.store,
                    claim_ref: operation.claim_ref,
                    operation_id: operation_id.into(),
                    action_ref: reference.into(),
                    deadline_ms: action.deadline_ms,
                    fence: action.fence,
                    mode,
                    start_permit,
                    publication_revision: operation.publication_revision,
                });
            }
        }
        Err(conflict())
    }

    async fn claim_operation(
        &self,
        tx: &mut Transaction<'_, S, I>,
        action: &SourceRenewalActionRowV3,
        operation_id: &str,
        digest: &str,
        phase: &SourceActorCurrentData,
    ) -> Result<(OperationRow, NativeRenewalMode, Option<String>), GatewayError> {
        let reference = action.action_ref.as_str();
        let key = rows::key("operation", operation_id);
        let existing: Option<OperationRow> = tx.read(&key).await?;
        let generation_key = generation_key(&action.fence)?;
        let generation: Option<GenerationEffect> = tx.read(&generation_key).await?;
        if generation.as_ref().is_some_and(|g| g.v != 1) {
            return Err(fail());
        }
        let result = if let Some(mut row) = existing {
            if row.v != 1
                || row.action_ref != reference
                || row.operation_id != operation_id
                || row.request_digest != digest
                || generation.is_none()
            {
                return Err(conflict());
            }
            let terminal = row
                .terminal
                .clone()
                .or_else(|| generation.as_ref().and_then(|g| g.terminal.clone()));
            if let Some(terminal) = terminal {
                self.terminal(tx, action, phase, &terminal).await?;
                if row.terminal.is_none() {
                    row.phase = Phase::Published;
                    row.publication_revision = Some(terminal.publication_revision.clone());
                    row.terminal = Some(terminal);
                    tx.put(&key, &row, None)?;
                }
            } else {
                self.current(tx, action, phase).await?;
            }
            (row, NativeRenewalMode::ReconcileOnly, None)
        } else {
            self.current(tx, action, phase).await?;
            let permit = self.tokens.next_token()?;
            text(&permit)?;
            let fresh = generation.is_none();
            let row = OperationRow {
                v: 1,
                action_ref: reference.into(),
                operation_id: operation_id.into(),
                request_digest: digest.to_owned(),
                claim_ref: self.tokens.next_token()?,
                start_digest: sha256_hex(permit.as_bytes()),
                phase: if fresh {
                    Phase::Claimed
                } else {
                    Phase::Unknown
                },
                publication_revision: None,
                native_source_generation: None,
                access_generation: None,
                terminal: None,
            };
            tx.put(&key, &row, None)?;
            if fresh {
                tx.put(
                    &generation_key,
                    &GenerationEffect {
                        v: 1,
                        operation_id: operation_id.into(),
                        claim_ref: row.claim_ref.clone(),
                        publication_revision: None,
                        terminal: None,
                    },
                    None,
                )?;
            }
            (
                row,
                if fresh {
                    NativeRenewalMode::RenewOnce
                } else {
                    NativeRenewalMode::ReconcileOnly
                },
                fresh.then_some(permit),
            )
        };
        Ok(result)
    }
    pub async fn resolve_claim(
        &self,
        operation_id: &str,
        claim_ref: &str,
        start_permit: Option<&str>,
    ) -> Result<SourceNativeRenewalClaim<'a, S>, GatewayError> {
        text(operation_id)?;
        text(claim_ref)?;
        for _ in 0..5 {
            let raw = self
                .store
                .read(&rows::key("operation", operation_id))
                .await?
                .ok_or_else(fail)?;
            metadata(&raw, self.time(0)?)?;
            let before: OperationRow =
                serde_json::from_value(raw.json_value()?.clone()).map_err(|_| fail())?;
            let (mut tx, action, phase) = self.begin(&before.action_ref, 0).await?;
            let row: OperationRow = tx
                .read(&rows::key("operation", operation_id))
                .await?
                .ok_or_else(fail)?;
            if row.v != 1 || row.operation_id != operation_id || row.claim_ref != claim_ref {
                return Err(conflict());
            }
            if row.action_ref != action.action_ref {
                return Err(conflict());
            }
            if action.operation_id.as_deref() != Some(operation_id) {
                return Err(conflict());
            }
            if let Some(terminal) = &row.terminal {
                self.terminal(&mut tx, &action, &phase, terminal).await?;
            } else {
                self.current(&mut tx, &action, &phase).await?;
            }
            let fresh = row.phase == Phase::Claimed
                && start_permit.is_some_and(|p| sha256_hex(p.as_bytes()) == row.start_digest);
            if start_permit.is_some() && !fresh {
                return Err(conflict());
            }
            if tx.commit().await? {
                return Ok(SourceNativeRenewalClaim {
                    store: self.store,
                    claim_ref: claim_ref.into(),
                    operation_id: operation_id.into(),
                    action_ref: action.action_ref,
                    deadline_ms: action.deadline_ms,
                    fence: action.fence,
                    mode: if fresh {
                        NativeRenewalMode::RenewOnce
                    } else {
                        NativeRenewalMode::ReconcileOnly
                    },
                    start_permit: start_permit.map(str::to_owned),
                    publication_revision: row.publication_revision,
                });
            }
        }
        Err(conflict())
    }

    pub async fn start(
        &self,
        claim: &mut SourceNativeRenewalClaim<'_, S>,
    ) -> Result<NativeRenewalMode, GatewayError> {
        let can_start = claim.mode == NativeRenewalMode::RenewOnce;
        claim.mode = NativeRenewalMode::ReconcileOnly;
        let permit = claim.start_permit.take();
        for _ in 0..5 {
            let (mut tx, _, phase) = self.begin(&claim.action_ref, 0).await?;
            let (action, mut operation) = self.operation(&mut tx, claim).await?;
            self.current(&mut tx, &action, &phase).await?;
            let generation: GenerationEffect = tx
                .read(&generation_key(&action.fence)?)
                .await?
                .ok_or_else(fail)?;
            if !can_start
                || operation.phase != Phase::Claimed
                || generation.v != 1
                || generation.operation_id != claim.operation_id
                || generation.claim_ref != claim.claim_ref
                || permit
                    .as_ref()
                    .is_none_or(|p| sha256_hex(p.as_bytes()) != operation.start_digest)
            {
                return Ok(NativeRenewalMode::ReconcileOnly);
            }
            operation.phase = Phase::Started;
            tx.put(
                &rows::key("operation", &claim.operation_id),
                &operation,
                None,
            )?;
            self.current(&mut tx, &action, &phase).await?;
            if tx.commit().await? {
                return Ok(NativeRenewalMode::RenewOnce);
            }
        }
        Err(conflict())
    }

    pub async fn recheck(
        &self,
        claim: &SourceNativeRenewalClaim<'_, S>,
    ) -> Result<SourceNativeRenewalFenceV3, GatewayError> {
        for _ in 0..5 {
            let (mut tx, _, phase) = self.begin(&claim.action_ref, 0).await?;
            let (action, operation) = self.operation(&mut tx, claim).await?;
            let fence = if let Some(terminal) = &operation.terminal {
                self.terminal(&mut tx, &action, &phase, terminal).await?
            } else {
                self.current(&mut tx, &action, &phase).await?
            };
            if tx.commit().await? {
                return Ok(fence);
            }
        }
        Err(conflict())
    }

    pub async fn published(
        &self,
        claim: &SourceNativeRenewalClaim<'_, S>,
        revision: &str,
    ) -> Result<SourceNativeRenewalPublished, GatewayError> {
        text(revision)?;
        // Managed publication atomically records its terminal proof. A bare RPC success cannot do so.
        let (mut tx, _, phase) = self.begin(&claim.action_ref, 0).await?;
        let (action, operation) = self.operation(&mut tx, claim).await?;
        if operation.phase != Phase::Published
            || operation.publication_revision.as_deref() != Some(revision)
        {
            return Err(fail());
        }
        let terminal = operation.terminal.as_ref().ok_or_else(fail)?;
        self.terminal(&mut tx, &action, &phase, terminal).await?;
        if !tx.commit().await? {
            return Err(conflict());
        }
        Ok(SourceNativeRenewalPublished {
            publication_revision: revision.into(),
        })
    }

    pub async fn unknown(
        &self,
        claim: &SourceNativeRenewalClaim<'_, S>,
    ) -> Result<(), GatewayError> {
        for _ in 0..5 {
            // Conservative uncertainty can be recorded after cancellation or deadline loss.
            let mut tx = self.transaction(self.cleanup_time(0)?)?;
            let (_, mut operation) = self.operation(&mut tx, claim).await?;
            if operation.phase == Phase::Published {
                return Ok(());
            }
            operation.phase = Phase::Unknown;
            tx.put(
                &rows::key("operation", &claim.operation_id),
                &operation,
                None,
            )?;
            if tx.commit().await? {
                return Ok(());
            }
        }
        Err(conflict())
    }

    /// Native-authenticated retained ownership cleanup; actor revocation cannot erase an owned unknown effect.
    pub async fn record_unknown(
        &self,
        proof: &AuthenticatedNativeOwner,
        exact_body: &[u8],
    ) -> Result<(), GatewayError> {
        let now = self.cleanup_time(0)?;
        proof.require_request(
            NativeCapability::OwnerRenewal,
            NATIVE_RENEWAL_AUTHORITY_PATH,
            exact_body,
            now,
        )?;
        if proof.owner_id() != self.context.identity.owner_id()
            || proof.owner_epoch() != self.context.identity.owner_epoch()
            || proof.transport_key_fingerprint() != self.context.identity.key_fingerprint()
        {
            return Err(fail());
        }
        let NativeRenewalAuthorityRequest::Unknown {
            operation_id,
            claim_ref,
            ..
        } = NativeRenewalAuthorityRequest::parse(exact_body)?
        else {
            return Err(fail());
        };
        for _ in 0..5 {
            let mut tx = self.transaction(self.cleanup_time(0)?)?;
            let key = rows::key("operation", &operation_id);
            let mut operation: OperationRow = tx.read(&key).await?.ok_or_else(fail)?;
            let action = self.action(&mut tx, &operation.action_ref).await?;
            let generation: GenerationEffect = tx
                .read(&generation_key(&action.fence)?)
                .await?
                .ok_or_else(fail)?;
            if operation.v != 1
                || operation.operation_id != operation_id
                || operation.claim_ref != claim_ref
                || action.operation_id.as_deref() != Some(operation_id.as_str())
                || action.fence.owner_id != self.context.identity.owner_id()
                || action.fence.owner_epoch != self.context.identity.owner_epoch()
                || action.fence.native_owner_id != self.context.registered.native_owner_id()
                || generation.v != 1
                || generation.operation_id != operation_id
                || generation.claim_ref != claim_ref
            {
                return Err(conflict());
            }
            let terminal = operation.phase == Phase::Published
                || operation.terminal.is_some()
                || operation.publication_revision.is_some()
                || generation.terminal.is_some()
                || generation.publication_revision.is_some();
            if !terminal {
                operation.phase = Phase::Unknown;
                tx.put(&key, &operation, None)?;
            }
            if tx.commit().await? {
                return Ok(());
            }
        }
        Err(conflict())
    }
}
fn generation_key(
    fence: &SourceNativeRenewalFenceV3,
) -> Result<gateway_kv_codec::key::RecordKey, GatewayError> {
    rows::generation_key_parts(
        &fence.native_owner_id,
        &fence.account_id_hash,
        &fence.source_native_generation,
    )
}
fn metadata(row: &StoredRecord, now: i64) -> Result<(), GatewayError> {
    row.validate_version()?;
    text(&row.revision)?;
    if let Some(expiry) = row.expires_at_ms {
        validate_time(expiry)?;
        if expiry <= now {
            return Err(fail());
        }
    }
    Ok(())
}
