//! Complete actual source admin Import4 preparation and local native-body authentication.
mod backend;
use backend::Transaction;
use gateway_auth_contracts::types::{AuthConfig, AuthHost};
use gateway_auth_native_contracts::native_import::{
    NativeImportKind, NativeImportMetadata, NativeImportTarget,
};
use gateway_auth_native_proof::{
    native_auth::NativeTransportIdentity,
    native_import::{NativeImportAdmission, VerifiedNativeImportBody},
};
use gateway_auth_native_wire::control::NATIVE_RENEWAL_MAX_MS;
use gateway_auth_passport_contracts::session::PassportDate;
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_authority_actor_currentness::{
    app::ActorService,
    source_actor::{SourceActorCurrentData, SourceNativeRenewalActor},
};
use gateway_authority_actor_records::{
    scope::NativeActorScope, source::StoredNativeActorV2, versions,
};
use gateway_authority_contracts::{
    AuthorityClock, NativeRenewalCancellation, TokenSource, add, text, validate_time,
};
use gateway_authority_native_import_facts::{SourceImportFactsData, SourceNativeImportFactsReader};
use gateway_authority_native_records::{
    owner::RegisteredRenewalOwner, rows, source_rows::SourceImportActionRowV4,
};
use gateway_contracts::identity::RequestContext;
use gateway_core::{source_control::decode_source_control_record, state::AuthorityStore};
use gateway_source::{error::GatewayError, source_json::SourceJson};
pub struct SourceNativeImportContext<'a, C> {
    identity: &'a NativeTransportIdentity,
    registered: &'a RegisteredRenewalOwner,
    cancellation: &'a C,
}
impl<'a, C> SourceNativeImportContext<'a, C> {
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
/// Only the complete same-store original Source action CAS creates this result.
pub struct SourcePreparedLocalImport {
    row: SourceImportActionRowV4,
}
impl SourcePreparedLocalImport {
    pub fn metadata(&self) -> &NativeImportMetadata {
        &self.row.metadata
    }
    pub fn target(&self) -> &NativeImportTarget {
        &self.row.target
    }
    pub fn deadline_ms(&self) -> i64 {
        self.row.deadline_ms
    }
}
pub struct SourceAdmittedLocalImport<'a, S> {
    store: &'a S,
    admission: NativeImportAdmission,
    row: SourceImportActionRowV4,
}
/// No body, actor/context, admission or private row can be reconstructed from DATA.
/// ```compile_fail
/// use gateway_authority_native_import::SourceAuthenticatedLocalImport;
/// let _:SourceAuthenticatedLocalImport<'_,()> = serde_json::from_str("{}").unwrap();
/// ```
pub struct SourceAuthenticatedLocalImport<'a, S> {
    store: &'a S,
    body: VerifiedNativeImportBody,
    row: SourceImportActionRowV4,
}
impl<S> SourceAuthenticatedLocalImport<'_, S> {
    pub fn metadata(&self) -> &NativeImportMetadata {
        &self.row.metadata
    }
    pub fn target(&self) -> &NativeImportTarget {
        &self.row.target
    }
    pub fn deadline_ms(&self) -> i64 {
        self.row.deadline_ms
    }
    #[cfg(unix)]
    fn require_body(&self, body: &[u8], now: i64) -> Result<(), GatewayError> {
        if now >= self.deadline_ms() {
            return Err(unavailable());
        }
        self.body.require_body(body, now)
    }
    #[cfg(unix)]
    pub fn require_original_context(
        &self,
        context: &gateway_auth_native_proof::native_context::NativeControlContext<'_>,
        body: &[u8],
    ) -> Result<(), GatewayError> {
        let now = context.now_ms()?;
        self.require_body(body, now)?;
        self.body.require_context(context)
    }
}
pub struct SourceNativeLocalImportService<'a, S, I, H, C> {
    store: &'a S,
    tokens: &'a I,
    clock: Option<&'a dyn AuthorityClock>,
    actors: ActorService<'a, S, H>,
    context: SourceNativeImportContext<'a, C>,
}
impl<
    'a,
    S: AuthorityStore,
    I: TokenSource,
    H: AuthHost + PassportDate,
    C: NativeRenewalCancellation,
> SourceNativeLocalImportService<'a, S, I, H, C>
{
    pub fn new(
        store: &'a S,
        tokens: &'a I,
        clock: Option<&'a dyn AuthorityClock>,
        host: &'a H,
        config: &'a AuthConfig,
        context: SourceNativeImportContext<'a, C>,
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
    fn lifetime(
        &self,
        row: &SourceImportActionRowV4,
        phase: &SourceActorCurrentData,
        now: i64,
    ) -> Result<(), GatewayError> {
        validate_time(now)?;
        if row.v != versions::IMPORT_ACTION_V4
            || row.publication.is_some()
            || row.kind != NativeImportKind::CodexAuth
            || row.kind != row.target.kind()
            || row.deadline_ms != add(row.created_at_ms, NATIVE_RENEWAL_MAX_MS)?
            || now < row.created_at_ms
            || now >= row.deadline_ms
            || row.actor.source_valid_until_ms.is_some_and(|at| at <= now)
            || row.actor.source_expiry().is_some_and(|at| at <= now)
            || phase.minimum_expiry().is_some_and(|at| at <= now)
        {
            return Err(unavailable());
        }
        Ok(())
    }
    async fn facts(
        &self,
        tx: &mut Transaction<'_, S, I>,
        actor: &StoredNativeActorV2,
        original: &RequestContext,
        phase: &SourceActorCurrentData,
        target: &NativeImportTarget,
    ) -> Result<SourceImportFactsData, GatewayError> {
        let now = tx.now;
        let data = {
            let snapshot = tx.routing_read_store();
            SourceNativeImportFactsReader::new(
                &snapshot,
                self.context.identity,
                self.context.registered,
                self.clock,
                self.context.cancellation,
                now,
            )?
            .read(actor, original, phase, target)
            .await?
        };
        for check in data.revision_checks() {
            if !tx.expect(&check.key, &check.expected_revision) {
                return Err(unavailable());
            }
        }
        tx.now = data.observed_at_ms();
        Ok(data)
    }
    pub async fn prepare_import(
        &self,
        actor: &SourceNativeRenewalActor<'_, S>,
        target: NativeImportTarget,
    ) -> Result<SourcePreparedLocalImport, GatewayError> {
        target.validate()?;
        if actor.scope() != NativeActorScope::Admin {
            return Err(unavailable());
        }
        for _ in 0..5 {
            let phase = self
                .actors
                .prepare_source_phase(actor, NativeActorScope::Admin, self.time(0)?)
                .await?;
            let now = self.time(0)?;
            let mut tx = self.transaction(now)?;
            hydrate(&mut tx, phase.current_data().revision_checks()).await?;
            let data = self
                .facts(
                    &mut tx,
                    phase.actor_data(),
                    phase.context(),
                    phase.current_data(),
                    &target,
                )
                .await?;
            let reference = source_reference(
                phase.context(),
                phase.current_data().policy_digest(),
                &target,
                data.facts(),
            )?;
            let key = rows::key("action", &reference);
            let prior: Option<SourceJson> = tx.read_source_control(&key).await?;
            let row = if let Some(raw) = prior {
                let row = SourceImportActionRowV4::from_source_json(&raw)?;
                if row.v != 4
                    || row.publication.is_some()
                    || row.actor != *phase.actor_data()
                    || row.context != *phase.context()
                    || row.target != target
                    || row.facts != *data.facts()
                {
                    return Err(unavailable());
                }
                row
            } else {
                let metadata = NativeImportMetadata {
                    v: 1,
                    operation_id: self.tokens.next_token()?,
                    action_ref: reference.clone(),
                };
                metadata.validate()?;
                let row = SourceImportActionRowV4 {
                    v: 4,
                    kind: target.kind(),
                    metadata,
                    target: target.clone(),
                    actor: phase.actor_data().clone(),
                    context: phase.context().clone(),
                    created_at_ms: now,
                    deadline_ms: add(now, NATIVE_RENEWAL_MAX_MS)?,
                    facts: data.facts().clone(),
                    publication: None,
                };
                tx.put_source_control(&key, &row.to_source_json()?, None)?;
                row
            };
            row.metadata.validate()?;
            if row.metadata.action_ref != reference {
                return Err(unavailable());
            }
            self.current_row(&mut tx, &row, phase.current_data())
                .await?;
            if tx.commit().await? {
                self.lifetime(&row, phase.current_data(), self.time(now)?)?;
                return Ok(SourcePreparedLocalImport { row });
            }
        }
        Err(unavailable())
    }
    async fn row(
        &self,
        tx: &mut Transaction<'_, S, I>,
        metadata: &NativeImportMetadata,
    ) -> Result<SourceImportActionRowV4, GatewayError> {
        let raw: SourceJson = tx
            .read_source_control(&rows::key("action", &metadata.action_ref))
            .await?
            .ok_or_else(unavailable)?;
        let row = SourceImportActionRowV4::from_source_json(&raw)?;
        if row.metadata != *metadata {
            return Err(unavailable());
        }
        Ok(row)
    }
    async fn load(
        &self,
        metadata: &NativeImportMetadata,
        now: i64,
    ) -> Result<SourceImportActionRowV4, GatewayError> {
        let raw = self
            .store
            .read(&rows::key("action", &metadata.action_ref))
            .await?
            .ok_or_else(unavailable)?;
        raw.validate_version()?;
        text(&raw.revision)?;
        if let Some(expiry) = raw.expires_at_ms {
            validate_time(expiry)?;
            if expiry <= now {
                return Err(unavailable());
            }
        }
        let row = SourceImportActionRowV4::from_source_json(&decode_source_control_record(&raw)?)?;
        if row.metadata != *metadata {
            return Err(unavailable());
        }
        Ok(row)
    }
    async fn current_row(
        &self,
        tx: &mut Transaction<'_, S, I>,
        row: &SourceImportActionRowV4,
        phase: &SourceActorCurrentData,
    ) -> Result<(), GatewayError> {
        self.lifetime(row, phase, self.time(row.created_at_ms)?)?;
        let data = self
            .facts(tx, &row.actor, &row.context, phase, &row.target)
            .await?;
        if data.facts() != &row.facts {
            return Err(unavailable());
        }
        self.lifetime(row, phase, self.time(tx.now)?)?;
        Ok(())
    }
    async fn current_import(
        &self,
        metadata: &NativeImportMetadata,
        expected: Option<&SourceImportActionRowV4>,
    ) -> Result<SourceImportActionRowV4, GatewayError> {
        metadata.validate()?;
        for _ in 0..5 {
            let observed = self.time(0)?;
            let before = self.load(metadata, observed).await?;
            let phase = self
                .actors
                .check_stored_source_phase(
                    &before.actor,
                    &before.context,
                    NativeActorScope::Admin,
                    self.time(observed)?,
                )
                .await?;
            let mut tx = self.transaction(self.time(observed)?)?;
            hydrate(&mut tx, phase.revision_checks()).await?;
            let row = self.row(&mut tx, metadata).await?;
            if row.actor != before.actor
                || row.context != before.context
                || expected.is_some_and(|old| {
                    old.created_at_ms != row.created_at_ms
                        || old.deadline_ms != row.deadline_ms
                        || old.metadata != row.metadata
                        || old.target != row.target
                        || old.context != row.context
                        || old.actor != row.actor
                        || old.facts != row.facts
                })
            {
                return Err(unavailable());
            }
            self.current_row(&mut tx, &row, &phase).await?;
            if tx.commit().await? {
                self.lifetime(&row, &phase, self.time(observed)?)?;
                return Ok(row);
            }
        }
        Err(unavailable())
    }
    pub async fn admit_import(
        &self,
        admission: NativeImportAdmission,
    ) -> Result<SourceAdmittedLocalImport<'a, S>, GatewayError> {
        admission.require_owner(self.context.identity, self.time(0)?)?;
        let row = self.current_import(admission.metadata(), None).await?;
        if admission.kind() != row.target.kind() {
            return Err(unavailable());
        }
        admission.require_owner(self.context.identity, self.time(0)?)?;
        Ok(SourceAdmittedLocalImport {
            store: self.store,
            admission,
            row,
        })
    }
    pub async fn finish_import(
        &self,
        admitted: SourceAdmittedLocalImport<'_, S>,
        original_body: &[u8],
    ) -> Result<SourceAuthenticatedLocalImport<'a, S>, GatewayError> {
        if !std::ptr::eq(self.store, admitted.store) {
            return Err(unavailable());
        }
        let body = admitted
            .admission
            .verify_body(original_body, self.time(0)?)?;
        let row = self
            .current_import(body.metadata(), Some(&admitted.row))
            .await?;
        body.require_owner(self.context.identity, self.time(0)?)?;
        body.require_body(original_body, self.time(0)?)?;
        Ok(SourceAuthenticatedLocalImport {
            store: self.store,
            body,
            row,
        })
    }
    pub async fn recheck_import(
        &self,
        proof: &SourceAuthenticatedLocalImport<'_, S>,
    ) -> Result<(), GatewayError> {
        if !std::ptr::eq(self.store, proof.store) {
            return Err(unavailable());
        }
        proof
            .body
            .require_owner(self.context.identity, self.time(0)?)?;
        self.current_import(proof.metadata(), Some(&proof.row))
            .await?;
        proof
            .body
            .require_owner(self.context.identity, self.time(0)?)
    }
}
async fn hydrate<S: AuthorityStore, I: TokenSource>(
    tx: &mut Transaction<'_, S, I>,
    checks: &[gateway_core::state::RevisionCheck],
) -> Result<(), GatewayError> {
    for check in checks {
        tx.observe_revision(&check.key).await?;
        if !tx.expect(&check.key, &check.expected_revision) {
            return Err(unavailable());
        }
    }
    Ok(())
}
fn source_reference(
    context: &RequestContext,
    policy_digest: &str,
    target: &NativeImportTarget,
    facts: &gateway_authority_native_records::import_facts::ImportFacts,
) -> Result<String, GatewayError> {
    let raw = SourceJson::from_serializable(&(
        versions::IMPORT_ACTION_V4_DOMAIN,
        context.to_source_json()?,
        policy_digest,
        target,
        facts,
    ))?;
    Ok(sha256_hex(raw.as_bytes()))
}
fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Native import preparation is unavailable, changed, expired, or unsupported.",
        "native_import_authority_unavailable",
    )
}
