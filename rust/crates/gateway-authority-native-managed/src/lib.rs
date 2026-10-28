//! Complete source managed publication and prepared-action publication verification.
mod backend;
use backend::Transaction;
use gateway_auth_contracts::types::{AuthConfig, AuthHost};
use gateway_auth_credentials::{
    native_credentials::native_refresh_time_ns,
    provider_credentials::{
        CodexNativeOwner, CodexSourceAccount, CodexSourcePool, MANAGED_NATIVE_SOURCE_VERSION,
        ManagedNativeSource, codex_access_expiry_ms, codex_credential_identity,
        codex_source_pool_key, parse_codex_source_pool, valid_native_generation,
    },
};
use gateway_auth_native_proof::native_auth::{
    AuthenticatedNativeOwner, NativeCapability, NativeTransportIdentity,
};
use gateway_auth_native_wire::{
    control::NATIVE_RENEWAL_MAX_MS, native_read::NATIVE_PUBLICATION_PATH,
};
use gateway_auth_passport_contracts::session::PassportDate;
use gateway_auth_primitives::crypto::{Secret, sha256_hex};
use gateway_authority_actor_currentness::{
    app::ActorService, source_actor::SourceActorCurrentData,
};
use gateway_authority_actor_records::{scope::NativeActorScope, versions};
use gateway_authority_contracts::{
    AuthorityClock, NativeRenewalCancellation, TokenSource, add, text, validate_time,
};
use gateway_authority_keys as keys;
use gateway_authority_native_context_facts::SourceNativeContextFactsReader;
use gateway_authority_native_preparation::SourceNativeRenewalAction;
use gateway_authority_native_records::{
    managed::{NativeManagedPublication, NativeManagedPublicationDisposition},
    owner::RegisteredRenewalOwner,
    rows::{self, GenerationEffect, OperationRow, Phase, TerminalPublication, conflict, fail},
    source_rows::SourceRenewalActionRowV3,
};
use gateway_authority_routing_records::types::{PoolAccount, SubscriptionPool};
use gateway_contracts::identity::PoolVersionstamp;
use gateway_core::{
    source_control::decode_source_control_record,
    state::{AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, StoredRecord},
};
use gateway_kv_codec::key::RecordKey;
use gateway_native_authority_wire::{
    context::NativeRenewalContext, source_fence::SourceNativeRenewalFenceV3,
};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde::Deserialize;
use serde_json::Value;
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Document {
    account_id: String,
    access_token: String,
    #[serde(deserialize_with = "required_nullable")]
    native_refreshed_at: Option<String>,
}
fn required_nullable<'de, D: serde::Deserializer<'de>>(d: D) -> Result<Option<String>, D::Error> {
    Option::<String>::deserialize(d)
}

/// This parser lives below the publication crate to keep its independent effect guard cycle-free.
/// The publication sender/receiver round-trip fixtures bind this exact private v2 contract.
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Wire {
    operation: String,
    v: u8,
    action_ref: String,
    operation_id: String,
    native_owner_id: String,
    owner_revision: String,
    source_revision: String,
    normalized_revision: String,
    publication_revision: String,
    account_id_hash: String,
    previous_native_source_generation: String,
    native_source_generation: String,
    access_generation: String,
    document: Document,
}

pub struct SourceNativeManagedContext<'a, C> {
    identity: &'a NativeTransportIdentity,
    registered: &'a RegisteredRenewalOwner,
    cancellation: &'a C,
}
impl<'a, C> SourceNativeManagedContext<'a, C> {
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
struct Prepared {
    source_key: RecordKey,
    source: CodexSourcePool,
    old_normalized: SubscriptionPool,
    slot: usize,
    access: Secret,
    terminal: TerminalPublication,
    unchanged: bool,
}
pub struct SourceNativeManagedService<'a, S, I, H, C> {
    store: &'a S,
    tokens: &'a I,
    clock: Option<&'a dyn AuthorityClock>,
    actors: ActorService<'a, S, H>,
    context: SourceNativeManagedContext<'a, C>,
}
impl<
    'a,
    S: AuthorityStore,
    I: TokenSource,
    H: AuthHost + PassportDate,
    C: NativeRenewalCancellation,
> SourceNativeManagedService<'a, S, I, H, C>
{
    pub fn new(
        store: &'a S,
        tokens: &'a I,
        clock: Option<&'a dyn AuthorityClock>,
        host: &'a H,
        config: &'a AuthConfig,
        context: SourceNativeManagedContext<'a, C>,
    ) -> Self {
        Self {
            store,
            tokens,
            clock,
            actors: ActorService::new(store, host, config),
            context,
        }
    }
    fn transaction(&self, now: i64) -> Result<Transaction<'_, S>, GatewayError> {
        Transaction::new(self.store, now)
    }
    pub fn now_ms(&self) -> Result<i64, GatewayError> {
        self.time(0)
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
        tx: &mut Transaction<'_, S>,
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
    async fn begin(
        &self,
        reference: &str,
        anchor: i64,
    ) -> Result<
        (
            Transaction<'_, S>,
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
        tx: &mut Transaction<'_, S>,
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
        tx: &mut Transaction<'_, S>,
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
    pub async fn publish_managed(
        &self,
        proof: &AuthenticatedNativeOwner,
        body: &[u8],
    ) -> Result<NativeManagedPublication, GatewayError> {
        let now = self.time(0)?;
        proof.require_request(
            NativeCapability::AccessPublish,
            NATIVE_PUBLICATION_PATH,
            body,
            now,
        )?;
        if body.len() > 65_536
            || proof.owner_id() != self.context.identity.owner_id()
            || proof.owner_epoch() != self.context.identity.owner_epoch()
            || proof.transport_key_fingerprint() != self.context.identity.key_fingerprint()
        {
            return Err(fail());
        }
        let w: Wire = serde_json::from_slice(body).map_err(|_| fail())?;
        if w.operation != "managed_access_publish"
            || w.v != 2
            || [
                &w.account_id_hash,
                &w.previous_native_source_generation,
                &w.native_source_generation,
                &w.access_generation,
            ]
            .iter()
            .any(|v| !valid_native_generation(v))
        {
            return Err(fail());
        }
        text(&w.publication_revision)?;
        let request_digest = sha256_hex(body);
        for _ in 0..5 {
            let (mut tx, action, phase) = self.begin(&w.action_ref, now).await?;
            let operation: OperationRow = tx
                .read(&rows::key("operation", &w.operation_id))
                .await?
                .ok_or_else(fail)?;
            if operation.v != 1
                || operation.action_ref != w.action_ref
                || operation.operation_id != w.operation_id
                || action.operation_id.as_deref() != Some(&w.operation_id)
                || w.native_owner_id != action.fence.native_owner_id
                || w.native_owner_id != self.context.registered.native_owner_id()
                || w.owner_revision != action.fence.owner_revision
                || w.source_revision != action.fence.source_revision
                || w.normalized_revision != action.fence.normalized_revision
                || w.account_id_hash != action.fence.account_id_hash
                || w.previous_native_source_generation != action.fence.source_native_generation
            {
                return Err(conflict());
            }
            if let Some(terminal) = &operation.terminal {
                if terminal.publication_revision != w.publication_revision
                    || terminal.publication_request_digest.as_deref() != Some(&request_digest)
                {
                    return Err(conflict());
                }
                self.terminal(&mut tx, &action, &phase, terminal).await?;
                if tx.commit().await? {
                    return Ok(outcome(
                        &w,
                        terminal,
                        self.context.identity.owner_id(),
                        self.context.identity.owner_epoch(),
                        NativeManagedPublicationDisposition::AlreadyCurrent,
                    ));
                }
                continue;
            }
            self.current(&mut tx, &action, &phase).await?;
            let generation_key = generation_key(&action.fence)?;
            let generation: GenerationEffect = tx.read(&generation_key).await?.ok_or_else(fail)?;
            if generation.v != 1 || generation.terminal.is_some() {
                return Err(conflict());
            }
            let prepared = self
                .candidate(&mut tx, &action, &w, &request_digest)
                .await?;
            let unchanged = prepared.unchanged;
            let (terminal, mutations) =
                self.mutations(&tx, &w, operation, generation, prepared, generation_key)?;
            self.current(&mut tx, &action, &phase).await?;
            let result = self
                .store
                .commit(AtomicTransition {
                    checks: tx.checks(),
                    mutations,
                })
                .await?;
            if result == CommitOutcome::Committed {
                return Ok(outcome(
                    &w,
                    &terminal,
                    self.context.identity.owner_id(),
                    self.context.identity.owner_epoch(),
                    if unchanged {
                        NativeManagedPublicationDisposition::AlreadyCurrent
                    } else {
                        NativeManagedPublicationDisposition::Applied
                    },
                ));
            }
        }
        Err(conflict())
    }
    async fn candidate(
        &self,
        tx: &mut Transaction<'_, S>,
        action: &SourceRenewalActionRowV3,
        w: &Wire,
        request_digest: &str,
    ) -> Result<Prepared, GatewayError> {
        let source_key = codex_source_pool_key();
        let raw: Value = tx.read(&source_key).await?.ok_or_else(fail)?;
        let source = parse_codex_source_pool(&raw)?.ok_or_else(fail)?;
        let old_normalized: SubscriptionPool = tx.read(&keys::pool()).await?.ok_or_else(fail)?;
        let slot = source
            .accounts
            .iter()
            .position(|a| a.identity().routing_account_hash.0 == action.fence.account_id_hash)
            .ok_or_else(fail)?;
        let previous = source.accounts.get(slot).ok_or_else(fail)?;
        let native = previous.native_owner.as_ref().ok_or_else(fail)?;
        let previous_physical = native
            .managed_source
            .as_ref()
            .filter(|m| m.v == MANAGED_NATIVE_SOURCE_VERSION)
            .ok_or_else(fail)?;
        // Full managed credentials are normalized only under separately approved offline native custody.
        if !previous.refresh_token.expose().is_empty()
            || native.external_user_id.is_some()
            || native.codex_home != self.context.registered.registered_home()
            || previous_physical.generation_hash != w.previous_native_source_generation
            || previous.account_id != w.document.account_id
        {
            return Err(fail());
        }
        let access = Secret::new(w.document.access_token.clone());
        let id =
            codex_credential_identity(&w.document.account_id, &access, &Secret::new(String::new()));
        let expiry = codex_access_expiry_ms(&access)
            .filter(|n| n.is_finite() && n.fract() == 0.0 && *n <= 9_007_199_254_740_991.0)
            .ok_or_else(fail)?;
        let old_expiry = action.fence.access_expires_at_ms;
        let changed = w.native_source_generation != w.previous_native_source_generation;
        if id.routing_account_hash.0 != w.account_id_hash
            || id.native_generation_hash != w.access_generation
            || expiry <= self.time(tx.now)?.saturating_add(120_000) as f64
            || old_expiry.is_some_and(|old| expiry < old as f64)
        {
            return Err(fail());
        }
        let old_time = native_refresh_time_ns(native.native_refreshed_at.as_deref());
        let new_time = native_refresh_time_ns(w.document.native_refreshed_at.as_deref());
        if w.document
            .native_refreshed_at
            .as_ref()
            .is_some_and(|_| new_time.is_none())
            || (!changed && w.access_generation != native.generation_hash)
            || (changed
                && (old_expiry.is_some_and(|old| expiry == old as f64)
                    || w.access_generation == native.generation_hash)
                && !matches!((old_time,new_time),(Some(old),Some(new))if new>old))
        {
            return Err(fail());
        }
        let unchanged = !changed
            && w.access_generation == native.generation_hash
            && w.document.native_refreshed_at == native.native_refreshed_at;
        if unchanged && w.publication_revision != w.source_revision {
            return Err(conflict());
        }
        if !unchanged
            && tx
                .checks()
                .iter()
                .any(|c| c.expected_revision.as_ref() == Some(&w.publication_revision))
        {
            return Err(conflict());
        }
        let terminal = terminal_record(w, &id.credential_version.0, request_digest, unchanged);
        Ok(Prepared {
            source_key,
            source,
            old_normalized,
            slot,
            access,
            terminal,
            unchanged,
        })
    }
    fn mutations(
        &self,
        tx: &Transaction<'_, S>,
        w: &Wire,
        mut operation: OperationRow,
        mut generation: GenerationEffect,
        prepared: Prepared,
        generation_key: RecordKey,
    ) -> Result<(TerminalPublication, Vec<RecordMutation>), GatewayError> {
        let Prepared {
            source_key,
            mut source,
            old_normalized,
            slot,
            access,
            terminal,
            unchanged,
        } = prepared;
        let mut mutations = Vec::new();
        if !unchanged {
            source.accounts[slot] = CodexSourceAccount {
                account_id: w.document.account_id.clone(),
                access_token: access,
                refresh_token: Secret::new(String::new()),
                updated_at_ms: tx.now as f64,
                native_owner: Some(CodexNativeOwner {
                    codex_home: self.context.registered.registered_home().to_owned(),
                    generation_hash: w.access_generation.clone(),
                    native_refreshed_at: w.document.native_refreshed_at.clone(),
                    external_user_id: None,
                    managed_source: Some(ManagedNativeSource {
                        v: MANAGED_NATIVE_SOURCE_VERSION,
                        generation_hash: w.native_source_generation.clone(),
                    }),
                }),
            };
            source.updated_at_ms = tx.now as f64;
            let accounts = source
                .accounts
                .iter()
                .zip(&old_normalized.accounts)
                .map(|(a, old)| {
                    let i = a.identity();
                    PoolAccount {
                        account_id_hash: i.routing_account_hash,
                        admin_account_hash: i.administrative_account_hash,
                        credential_version: i.credential_version,
                        enabled: old.enabled,
                    }
                })
                .collect();
            let normalized = SubscriptionPool {
                v: 1,
                pool_versionstamp: PoolVersionstamp(w.publication_revision.clone()),
                accounts,
            };
            mutations.push(RecordMutation::Put {
                key: source_key,
                record: StoredRecord::json(
                    &w.publication_revision,
                    source.private_source_value(),
                    None,
                ),
            });
            mutations.push(RecordMutation::Put {
                key: keys::pool(),
                record: StoredRecord::json(
                    &w.publication_revision,
                    serde_json::to_value(normalized).map_err(|_| fail())?,
                    None,
                ),
            });
        }
        operation.phase = Phase::Published;
        operation.publication_revision = Some(w.publication_revision.clone());
        operation.native_source_generation = Some(w.native_source_generation.clone());
        operation.access_generation = Some(w.access_generation.clone());
        operation.terminal = Some(terminal.clone());
        generation.publication_revision = Some(w.publication_revision.clone());
        generation.terminal = Some(terminal.clone());
        for (key, value) in [
            (
                rows::key("operation", &w.operation_id),
                serde_json::to_value(operation).map_err(|_| fail())?,
            ),
            (
                generation_key,
                serde_json::to_value(generation).map_err(|_| fail())?,
            ),
        ] {
            let revision = self.tokens.next_token()?;
            text(&revision)?;
            if revision == w.publication_revision
                || tx
                    .checks()
                    .iter()
                    .any(|c| c.expected_revision.as_ref() == Some(&revision))
            {
                return Err(conflict());
            }
            mutations.push(RecordMutation::Put {
                key,
                record: StoredRecord::json(revision, value, None),
            });
        }
        Ok((terminal, mutations))
    }
    pub async fn verify_action_publication(
        &self,
        action: &SourceNativeRenewalAction,
        operation_id: &str,
        publication_revision: &str,
    ) -> Result<SourceNativeRenewalFenceV3, GatewayError> {
        for _ in 0..5 {
            let (mut tx, row, phase) = self.begin(action.action_ref(), 0).await?;
            let operation: OperationRow = tx
                .read(&rows::key("operation", operation_id))
                .await?
                .ok_or_else(fail)?;
            if row.fence != *action.fence()
                || row.operation_id.as_deref() != Some(operation_id)
                || operation.v != 1
                || operation.operation_id != operation_id
                || operation.action_ref != row.action_ref
                || operation.phase != Phase::Published
                || operation.publication_revision.as_deref() != Some(publication_revision)
            {
                return Err(fail());
            }
            let terminal = operation.terminal.as_ref().ok_or_else(fail)?;
            let fence = self.terminal(&mut tx, &row, &phase, terminal).await?;
            if tx.commit().await? {
                return Ok(fence);
            }
        }
        Err(conflict())
    }
}
fn terminal_record(
    w: &Wire,
    credential_version: &str,
    request_digest: &str,
    unchanged: bool,
) -> TerminalPublication {
    TerminalPublication {
        publication_revision: w.publication_revision.clone(),
        source_revision: if unchanged {
            w.source_revision.clone()
        } else {
            w.publication_revision.clone()
        },
        normalized_revision: if unchanged {
            w.normalized_revision.clone()
        } else {
            w.publication_revision.clone()
        },
        native_source_generation: w.native_source_generation.clone(),
        access_generation: w.access_generation.clone(),
        credential_version: credential_version.to_owned(),
        publication_request_digest: Some(request_digest.to_owned()),
    }
}
fn outcome(
    w: &Wire,
    t: &TerminalPublication,
    owner: &str,
    epoch: &str,
    disposition: NativeManagedPublicationDisposition,
) -> NativeManagedPublication {
    NativeManagedPublication {
        disposition,
        owner_id: owner.into(),
        owner_epoch: epoch.into(),
        action_ref: w.action_ref.clone(),
        operation_id: w.operation_id.clone(),
        native_owner_id: w.native_owner_id.clone(),
        owner_revision: w.owner_revision.clone(),
        source_revision: w.source_revision.clone(),
        normalized_revision: w.normalized_revision.clone(),
        publication_revision: t.publication_revision.clone(),
        current_source_revision: t.source_revision.clone(),
        current_normalized_revision: t.normalized_revision.clone(),
        previous_native_source_generation: w.previous_native_source_generation.clone(),
        native_source_generation: t.native_source_generation.clone(),
        access_generation: t.access_generation.clone(),
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
