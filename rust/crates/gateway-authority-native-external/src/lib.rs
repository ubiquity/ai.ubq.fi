//! Complete Source external publication and atomic raw/normalized/operation/generation terminal.
mod backend;
use backend::Transaction;
use gateway_auth_contracts::types::{AuthConfig, AuthHost};
use gateway_auth_credentials::{
    native_credentials::{NativeCredentials, parse_native_document, verify_generation},
    provider_credentials::{
        CodexNativeOwner, CodexSourceAccount, CodexSourcePool, codex_source_pool_key,
        parse_codex_source_pool,
    },
};
use gateway_auth_native_contracts::transport::NativeCapability;
use gateway_auth_native_proof::native_auth::{AuthenticatedNativeOwner, NativeTransportIdentity};
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
use gateway_authority_native_records::{
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
use gateway_native_external_publication_wire::{
    receipt::{SourceExternalPublicationDisposition, SourceExternalPublicationReceipt},
    request::SourceExternalAccessRequest as Wire,
};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde_json::Value;

pub struct SourceNativeExternalContext<'a, C> {
    identity: &'a NativeTransportIdentity,
    registered: &'a RegisteredRenewalOwner,
    cancellation: &'a C,
}
impl<'a, C> SourceNativeExternalContext<'a, C> {
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
    observed: NativeCredentials,
    terminal: TerminalPublication,
    unchanged: bool,
}
pub struct SourceNativeExternalService<'a, S, I, H, C> {
    store: &'a S,
    tokens: &'a I,
    clock: Option<&'a dyn AuthorityClock>,
    actors: ActorService<'a, S, H>,
    context: SourceNativeExternalContext<'a, C>,
}
impl<
    'a,
    S: AuthorityStore,
    I: TokenSource,
    H: AuthHost + PassportDate,
    C: NativeRenewalCancellation,
> SourceNativeExternalService<'a, S, I, H, C>
{
    pub fn new(
        store: &'a S,
        tokens: &'a I,
        clock: Option<&'a dyn AuthorityClock>,
        host: &'a H,
        config: &'a AuthConfig,
        context: SourceNativeExternalContext<'a, C>,
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
    pub async fn publish_external(
        &self,
        proof: &AuthenticatedNativeOwner,
        body: &[u8],
    ) -> Result<SourceExternalPublicationReceipt, GatewayError> {
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
        let w = Wire::parse(body)?;
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
                || operation.claim_ref != w.claim_ref
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
                if operation.phase != Phase::Published
                    || operation.publication_revision.as_deref() != Some(&w.publication_revision)
                    || operation.native_source_generation.as_deref()
                        != Some(&terminal.native_source_generation)
                    || operation.access_generation.as_deref() != Some(&terminal.access_generation)
                    || terminal.publication_revision != w.publication_revision
                    || terminal.publication_request_digest.as_deref() != Some(&request_digest)
                {
                    return Err(conflict());
                }
                let marker: GenerationEffect = tx
                    .read(&generation_key(&action.fence)?)
                    .await?
                    .ok_or_else(fail)?;
                if marker.v != 1
                    || marker.publication_revision.as_deref() != Some(&w.publication_revision)
                    || marker.terminal.as_ref().is_none_or(|observed| {
                        observed.publication_revision != terminal.publication_revision
                            || observed.source_revision != terminal.source_revision
                            || observed.normalized_revision != terminal.normalized_revision
                            || observed.native_source_generation
                                != terminal.native_source_generation
                            || observed.access_generation != terminal.access_generation
                            || observed.credential_version != terminal.credential_version
                            || observed.publication_request_digest
                                != terminal.publication_request_digest
                    })
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
                        SourceExternalPublicationDisposition::AlreadyCurrent,
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
                        SourceExternalPublicationDisposition::AlreadyCurrent
                    } else {
                        SourceExternalPublicationDisposition::Applied
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
        if native.managed_source.is_some()
            || native.external_user_id.is_none()
            || !previous.refresh_token.expose().is_empty()
            || native.codex_home != self.context.registered.registered_home()
            || native.generation_hash != previous.identity().native_generation_hash
            || native.generation_hash != w.previous_native_source_generation
            || old_normalized.v != 1
            || tx.revision(&source_key).as_deref()
                != Some(old_normalized.pool_versionstamp.0.as_str())
            || old_normalized.accounts.len() != source.accounts.len()
            || source
                .accounts
                .iter()
                .zip(&old_normalized.accounts)
                .any(|(account, projected)| {
                    let identity = account.identity();
                    identity.routing_account_hash != projected.account_id_hash
                        || identity.administrative_account_hash != projected.admin_account_hash
                        || identity.credential_version != projected.credential_version
                })
        {
            return Err(fail());
        }
        let document = &w.document;
        let observed = parse_native_document(
            &serde_json::json!({"auth_mode":"chatgptAuthTokens","OPENAI_API_KEY":null,"tokens":{"account_id":document.account_id,"access_token":document.access_token,"id_token":document.access_token,"refresh_token":""},"last_refresh":document.native_refreshed_at}),
        )?;
        if observed.account_id != previous.account_id
            || observed.external_user_id != native.external_user_id
            || observed.external_user_id.as_deref() != Some(document.user_id.as_str())
            || !observed.refresh_token.expose().is_empty()
            || observed.routing_account_hash() != w.account_id_hash
            || observed.routing_credential_version() != w.credential_version
            || observed.generation_hash() != w.native_generation
        {
            return Err(conflict());
        }
        let old = NativeCredentials {
            account_id: previous.account_id.clone(),
            access_token: previous.access_token.clone(),
            refresh_token: previous.refresh_token.clone(),
            email: None,
            native_refreshed_at: native.native_refreshed_at.clone(),
            external_user_id: native.external_user_id.clone(),
        };
        let unchanged = old.same_credentials(&observed);
        let now = self.time(tx.now)?;
        if unchanged {
            if observed
                .access_expires_at_ms()
                .is_none_or(|expiry| expiry <= now.saturating_add(120_000))
            {
                return Err(fail());
            }
        } else {
            verify_generation(&old, &observed, now, true)?;
            if tx
                .checks()
                .iter()
                .any(|check| check.expected_revision.as_ref() == Some(&w.publication_revision))
            {
                return Err(conflict());
            }
        }
        let terminal = TerminalPublication {
            publication_revision: w.publication_revision.clone(),
            source_revision: if unchanged {
                tx.revision(&source_key).ok_or_else(fail)?
            } else {
                w.publication_revision.clone()
            },
            normalized_revision: if unchanged {
                tx.revision(&keys::pool()).ok_or_else(fail)?
            } else {
                w.publication_revision.clone()
            },
            native_source_generation: w.native_generation.clone(),
            access_generation: w.native_generation.clone(),
            credential_version: observed.routing_credential_version(),
            publication_request_digest: Some(request_digest.into()),
        };
        Ok(Prepared {
            source_key,
            source,
            old_normalized,
            slot,
            observed,
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
            observed,
            terminal,
            unchanged,
        } = prepared;
        let mut mutations = Vec::new();
        if !unchanged {
            source.accounts[slot] = CodexSourceAccount {
                account_id: observed.account_id.clone(),
                access_token: observed.access_token,
                refresh_token: Secret::new(String::new()),
                updated_at_ms: tx.now as f64,
                native_owner: Some(CodexNativeOwner {
                    codex_home: self.context.registered.registered_home().to_owned(),
                    generation_hash: w.native_generation.clone(),
                    native_refreshed_at: w.document.native_refreshed_at.clone(),
                    external_user_id: observed.external_user_id,
                    managed_source: None,
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
        operation.native_source_generation = Some(w.native_generation.clone());
        operation.access_generation = Some(w.native_generation.clone());
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
}

fn outcome(
    w: &Wire,
    t: &TerminalPublication,
    owner: &str,
    epoch: &str,
    disposition: SourceExternalPublicationDisposition,
) -> SourceExternalPublicationReceipt {
    SourceExternalPublicationReceipt {
        v: 2,
        operation: "source_external_access_publish".into(),
        disposition,
        owner_id: owner.into(),
        owner_epoch: epoch.into(),
        action_ref: w.action_ref.clone(),
        operation_id: w.operation_id.clone(),
        claim_ref: w.claim_ref.clone(),
        native_owner_id: w.native_owner_id.clone(),
        owner_revision: w.owner_revision.clone(),
        source_revision: w.source_revision.clone(),
        normalized_revision: w.normalized_revision.clone(),
        publication_revision: t.publication_revision.clone(),
        current_source_revision: t.source_revision.clone(),
        current_normalized_revision: t.normalized_revision.clone(),
        account_id_hash: w.account_id_hash.clone(),
        credential_version: t.credential_version.clone(),
        previous_native_source_generation: w.previous_native_source_generation.clone(),
        native_generation: t.native_source_generation.clone(),
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
