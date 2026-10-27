use crate::{invalid, text, validate_time};
use gateway_authority_contracts::{AuthorityClock, TokenSource};
use gateway_core::state::StoredRecord;
use gateway_core::{error::GatewayError, source_control::decode_source_control_record};
use gateway_core::{source_control::encode_source_control_record, state::RecordKey};
use gateway_core::{source_json::SourceJson, state::AtomicTransition, state::RecordMutation};
use gateway_core::{state::AuthorityStore, state::CommitOutcome, state::RevisionCheck};
use gateway_schema::aliases::SourceStringCollator;
use serde::{Serialize, de::DeserializeOwned};
use std::{collections::BTreeMap, collections::BTreeSet, sync::Arc};

pub struct Authority<S, I> {
    pub(crate) store: S,
    pub(crate) tokens: I,
    clock: Option<Box<dyn AuthorityClock>>,
    source_string_collator: Option<Arc<dyn SourceStringCollator>>,
}

struct MissingSourceStringCollator;
static MISSING_SOURCE_STRING_COLLATOR: MissingSourceStringCollator = MissingSourceStringCollator;
impl SourceStringCollator for MissingSourceStringCollator {
    fn compare(
        &self,
        _left: &gateway_core::js_string::JsString16,
        _right: &gateway_core::js_string::JsString16,
    ) -> Result<std::cmp::Ordering, GatewayError> {
        Err(GatewayError::unavailable(
            "Source string comparison requires the trusted runtime capability.",
            "source_string_collation_unavailable",
        ))
    }
}

impl<S: AuthorityStore, I: TokenSource> Authority<S, I> {
    pub fn new(store: S, tokens: I) -> Self {
        Self {
            store,
            tokens,
            clock: None,
            source_string_collator: None,
        }
    }

    pub fn with_clock(store: S, tokens: I, clock: impl AuthorityClock + 'static) -> Self {
        Self {
            store,
            tokens,
            clock: Some(Box::new(clock)),
            source_string_collator: None,
        }
    }

    /// Trusted source-runtime comparison capability, never a request/token field.
    pub fn with_source_string_collator(mut self, collator: Arc<dyn SourceStringCollator>) -> Self {
        self.source_string_collator = Some(collator);
        self
    }

    pub(crate) fn source_string_collator(&self) -> &dyn SourceStringCollator {
        self.source_string_collator
            .as_deref()
            .unwrap_or(&MISSING_SOURCE_STRING_COLLATOR)
    }

    pub(crate) fn fresh_time(&self, started_at: i64) -> Result<i64, GatewayError> {
        let now = self
            .clock
            .as_ref()
            .ok_or_else(|| {
                crate::unavailable(
                    "authority_clock_unavailable",
                    "Time-sensitive authority actions require a trusted current clock.",
                )
            })?
            .now_ms()?;
        validate_time(now)?;
        if now < started_at {
            return Err(crate::unavailable(
                "invalid_clock",
                "Authority clock moved backwards.",
            ));
        }
        Ok(now)
    }

    pub fn store(&self) -> &S {
        &self.store
    }

    pub(crate) fn transaction(&self, now: i64) -> Result<Transaction<'_, S, I>, GatewayError> {
        validate_time(now)?;
        Ok(Transaction {
            store: &self.store,
            tokens: &self.tokens,
            now,
            reads: BTreeMap::new(),
            writes: BTreeMap::new(),
        })
    }
}

/// Only the named local release adapter observes this private read origin.
pub(crate) enum ApiReleaseReadError {
    Backend(GatewayError),
    Record(GatewayError),
}
impl ApiReleaseReadError {
    fn into_error(self) -> GatewayError {
        match self {
            Self::Backend(error) | Self::Record(error) => error,
        }
    }
}
fn decode_json_record<T: DeserializeOwned>(row: &StoredRecord) -> Result<T, GatewayError> {
    serde_json::from_value(row.json_value()?.clone())
        .map_err(|_| invalid("Malformed authority record."))
}

pub(crate) struct Transaction<'a, S, I> {
    store: &'a S,
    tokens: &'a I,
    pub now: i64,
    reads: BTreeMap<String, Option<StoredRecord>>,
    writes: BTreeMap<String, RecordMutation>,
}

impl<S: AuthorityStore, I: TokenSource> Transaction<'_, S, I> {
    async fn cached_record(
        &mut self,
        key: &RecordKey,
    ) -> Result<Option<&StoredRecord>, GatewayError> {
        self.cached_record_classified(key)
            .await
            .map_err(ApiReleaseReadError::into_error)
    }
    async fn cached_record_classified(
        &mut self,
        key: &RecordKey,
    ) -> Result<Option<&StoredRecord>, ApiReleaseReadError> {
        if !self.reads.contains_key(&key.0) {
            let record = self
                .store
                .read(key)
                .await
                .map_err(ApiReleaseReadError::Backend)?;
            if let Some(value) = &record {
                value
                    .validate_version()
                    .map_err(ApiReleaseReadError::Record)?;
                text(&value.revision).map_err(ApiReleaseReadError::Record)?;
                if let Some(expiry) = value.expires_at_ms {
                    validate_time(expiry).map_err(ApiReleaseReadError::Record)?;
                }
            }
            self.reads.insert(key.0.clone(), record);
        }
        let raw = self.reads.get(&key.0).and_then(Option::as_ref);
        if raw.is_some_and(|row| row.expires_at_ms.is_some_and(|expiry| expiry <= self.now)) {
            return Ok(None);
        }
        Ok(raw)
    }

    /// Establish only the existing cached metadata/revision precondition. This
    /// observes no payload and cannot replace a named family's validation.
    pub(crate) async fn observe_revision(&mut self, key: &RecordKey) -> Result<(), GatewayError> {
        self.cached_record(key).await?;
        Ok(())
    }

    pub async fn read<T: DeserializeOwned>(
        &mut self,
        key: &RecordKey,
    ) -> Result<Option<T>, GatewayError> {
        self.cached_record(key)
            .await?
            .map(decode_json_record)
            .transpose()
    }

    pub(crate) fn routing_read_store(&mut self) -> RoutingReadStore<'_, S> {
        RoutingReadStore {
            store: self.store,
            reads: std::cell::RefCell::new(&mut self.reads),
        }
    }

    /// Explicit lossless opt-in for named control families. The caller still owns
    /// the exact key, DTO/version and currentness checks; data grants no authority.
    pub(crate) async fn read_source_control<T: DeserializeOwned>(
        &mut self,
        key: &RecordKey,
    ) -> Result<Option<T>, GatewayError> {
        self.cached_record(key)
            .await?
            .map(|row| {
                decode_source_control_record(row)?
                    .deserialize()
                    .map_err(|_| invalid("Malformed authority record."))
            })
            .transpose()
    }

    /// Preserve the full source value and custody for this one audit family.
    pub(crate) async fn read_full_pool_reset_audit(
        &mut self,
        episode_hash: &str,
    ) -> Result<Option<StoredRecord>, GatewayError> {
        let key = gateway_authority_keys::reset_shadow(episode_hash);
        Ok(self.cached_record(&key).await?.cloned())
    }

    pub fn revision(&self, key: &RecordKey) -> Option<String> {
        self.reads
            .get(&key.0)
            .and_then(Option::as_ref)
            .map(|row| row.revision.clone())
    }

    pub fn expiry(&self, key: &RecordKey) -> Option<i64> {
        self.reads
            .get(&key.0)
            .and_then(Option::as_ref)
            .and_then(|row| row.expires_at_ms)
    }

    pub fn expect(&self, key: &RecordKey, revision: &Option<String>) -> bool {
        self.reads.contains_key(&key.0) && &self.revision(key) == revision
    }

    pub fn checks(&self) -> Vec<RevisionCheck> {
        self.reads
            .iter()
            .map(|(key, record)| RevisionCheck {
                key: RecordKey(key.clone()),
                expected_revision: record.as_ref().map(|row| row.revision.clone()),
            })
            .collect()
    }

    pub fn post_checks(&self) -> Vec<RevisionCheck> {
        self.checks()
            .into_iter()
            .map(|mut check| {
                if let Some(mutation) = self.writes.get(&check.key.0) {
                    check.expected_revision = match mutation {
                        RecordMutation::Put { record, .. } => Some(record.revision.clone()),
                        RecordMutation::Delete { .. } => None,
                    };
                }
                check
            })
            .collect()
    }

    pub fn put<T: Serialize>(
        &mut self,
        key: &RecordKey,
        value: &T,
        expiry: Option<i64>,
    ) -> Result<(), GatewayError> {
        if !self.reads.contains_key(&key.0) {
            return Err(invalid(
                "A mutation is missing its strong-read precondition.",
            ));
        }
        if let Some(expiry) = expiry {
            validate_time(expiry)?;
        }
        let revision = self.tokens.next_token()?;
        text(&revision)?;
        if self
            .reads
            .values()
            .flatten()
            .any(|row| row.revision == revision)
        {
            return Err(invalid("Token source reused an existing revision."));
        }
        let record = StoredRecord::json(
            revision,
            serde_json::to_value(value).map_err(|_| invalid("Authority serialization failed."))?,
            expiry,
        );
        self.writes.insert(
            key.0.clone(),
            RecordMutation::Put {
                key: key.clone(),
                record,
            },
        );
        Ok(())
    }

    /// Same strong-read, revision and expiry contract as `put`, but only explicit
    /// named-family callers choose the finite Json/ProfileV2 source tree codec.
    pub(crate) fn put_source_control<T: Serialize + ?Sized>(
        &mut self,
        key: &RecordKey,
        value: &T,
        expiry: Option<i64>,
    ) -> Result<(), GatewayError> {
        if !self.reads.contains_key(&key.0) {
            return Err(invalid(
                "A mutation is missing its strong-read precondition.",
            ));
        }
        if let Some(expiry) = expiry {
            validate_time(expiry)?;
        }
        let revision = self.tokens.next_token()?;
        text(&revision)?;
        if self
            .reads
            .values()
            .flatten()
            .any(|row| row.revision == revision)
        {
            return Err(invalid("Token source reused an existing revision."));
        }
        let document = SourceJson::from_serializable(value)?;
        let record = encode_source_control_record(revision, &document, expiry)?;
        self.writes.insert(
            key.0.clone(),
            RecordMutation::Put {
                key: key.clone(),
                record,
            },
        );
        Ok(())
    }

    pub fn delete(&mut self, key: &RecordKey) -> Result<(), GatewayError> {
        if !self.reads.contains_key(&key.0) {
            return Err(invalid(
                "A deletion is missing its strong-read precondition.",
            ));
        }
        self.writes
            .insert(key.0.clone(), RecordMutation::Delete { key: key.clone() });
        Ok(())
    }

    pub async fn commit(self) -> Result<bool, GatewayError> {
        let checks = self.checks();
        let mut revisions = BTreeSet::new();
        for mutation in self.writes.values() {
            if let RecordMutation::Put { record, .. } = mutation
                && !revisions.insert(record.revision.clone())
            {
                return Err(invalid("Token source duplicated a mutation revision."));
            }
        }
        Ok(self
            .store
            .commit(AtomicTransition {
                checks,
                mutations: self.writes.into_values().collect(),
            })
            .await?
            == CommitOutcome::Committed)
    }
}

#[cfg(test)]
mod source_control_tests;

impl<S: AuthorityStore, I: TokenSource> gateway_authority_api_quota::ApiQuotaFactory
    for Authority<S, I>
{
    type Store = S;
    type Tokens = I;
    fn api_quota(&self) -> gateway_authority_api_quota::ApiQuotaService<'_, S, I> {
        gateway_authority_api_quota::ApiQuotaService::new(&self.store, &self.tokens)
    }
}

impl<S: AuthorityStore, I: TokenSource> gateway_authority_api_release::ApiReleaseFactory
    for Authority<S, I>
{
    type Store = S;
    type Tokens = I;
    fn api_release(&self) -> gateway_authority_api_release::ApiReleaseService<'_, S, I> {
        gateway_authority_api_release::ApiReleaseService::new(&self.store, &self.tokens)
    }
}

impl<S: AuthorityStore, I: TokenSource> gateway_authority_kernel::KernelFactory
    for Authority<S, I>
{
    type Store = S;
    type Tokens = I;
    fn kernel(&self) -> gateway_authority_kernel::KernelService<'_, S, I> {
        gateway_authority_kernel::KernelService::new(&self.store, &self.tokens)
    }
}

impl<S: AuthorityStore, I: TokenSource> gateway_authority_local_completion::LocalCompletionFactory
    for Authority<S, I>
{
    type Store = S;
    type Tokens = I;
    fn local_completion(
        &self,
    ) -> gateway_authority_local_completion::LocalCompletionService<'_, S, I> {
        gateway_authority_local_completion::LocalCompletionService::new(&self.store, &self.tokens)
    }
}

/// Private fixed-reader bridge. No mutation or raw store reference crosses this boundary.
pub(crate) struct RoutingReadStore<'a, S> {
    store: &'a S,
    reads: std::cell::RefCell<&'a mut BTreeMap<String, Option<StoredRecord>>>,
}
impl<S: AuthorityStore> AuthorityStore for RoutingReadStore<'_, S> {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        let missing = !self.reads.borrow().contains_key(&key.0);
        if missing {
            let record = self.store.read(key).await?;
            if let Some(row) = &record {
                row.validate_version()?;
                text(&row.revision)?;
                if let Some(expiry) = row.expires_at_ms {
                    validate_time(expiry)?;
                }
            }
            self.reads.borrow_mut().insert(key.0.clone(), record);
        }
        Ok(self.reads.borrow().get(&key.0).cloned().flatten())
    }
    async fn commit(&self, _transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        Err(invalid(
            "Routing evidence reader has no mutation authority.",
        ))
    }
}

impl<S: AuthorityStore, I: TokenSource> gateway_authority_capacity::CapacityFactory
    for Authority<S, I>
{
    type Store = S;
    type Tokens = I;
    fn capacity(&self) -> gateway_authority_capacity::CapacityService<'_, S, I> {
        gateway_authority_capacity::CapacityService::new(
            &self.store,
            &self.tokens,
            self.clock.as_deref(),
        )
    }
}

impl<S: AuthorityStore, I: TokenSource> gateway_authority_claude_proof::ClaudeProofFactory
    for Authority<S, I>
{
    type Store = S;
    fn claude_proof(&self) -> gateway_authority_claude_proof::ClaudeProofService<'_, S> {
        gateway_authority_claude_proof::ClaudeProofService::new(&self.store, self.clock.as_deref())
    }
}

impl<S: AuthorityStore, I: TokenSource> gateway_authority_routing_selection::SelectionFactory
    for Authority<S, I>
{
    type Store = S;
    type Tokens = I;
    fn selection(&self) -> gateway_authority_routing_selection::SelectionService<'_, S, I> {
        gateway_authority_routing_selection::SelectionService::new(
            &self.store,
            &self.tokens,
            self.clock.as_deref(),
        )
    }
}

impl<S: AuthorityStore, I: TokenSource> gateway_authority_paid_route::PaidRouteFactory
    for Authority<S, I>
{
    type Store = S;
    fn paid_route(&self) -> gateway_authority_paid_route::PaidRouteService<'_, S> {
        gateway_authority_paid_route::PaidRouteService::new(
            &self.store,
            self.clock.as_deref(),
            self.source_string_collator(),
        )
    }
}

impl<S: AuthorityStore, I: TokenSource> gateway_authority_paid_admission::PaidAdmissionFactory
    for Authority<S, I>
{
    type Store = S;
    type Tokens = I;
    fn paid_admission(&self) -> gateway_authority_paid_admission::PaidAdmissionService<'_, S, I> {
        gateway_authority_paid_admission::PaidAdmissionService::new(
            &self.store,
            &self.tokens,
            self.clock.as_deref(),
            self.source_string_collator(),
        )
    }
}

impl<S: AuthorityStore, I: TokenSource> gateway_authority_paid_ledger::PaidLedgerFactory
    for Authority<S, I>
{
    type Store = S;
    type Tokens = I;
    fn paid_ledger(&self) -> gateway_authority_paid_ledger::PaidLedgerService<'_, S, I> {
        gateway_authority_paid_ledger::PaidLedgerService::new(&self.store, &self.tokens)
    }
}

impl<S: gateway_core::query::QueryStore, I: TokenSource>
    gateway_authority_paid_backfill::PaidBackfillFactory for Authority<S, I>
{
    type Store = S;
    type Tokens = I;
    fn paid_backfill(&self) -> gateway_authority_paid_backfill::PaidBackfillService<'_, S, I> {
        gateway_authority_paid_backfill::PaidBackfillService::new(&self.store, &self.tokens)
    }
}

impl<S: AuthorityStore, I: TokenSource> gateway_authority_reset_permit::ResetPermitFactory
    for Authority<S, I>
{
    type Store = S;
    type Tokens = I;
    fn reset_permit(&self) -> gateway_authority_reset_permit::ResetPermitService<'_, S, I> {
        gateway_authority_reset_permit::ResetPermitService::new(
            &self.store,
            &self.tokens,
            self.clock.as_deref(),
        )
    }
}

impl<S: AuthorityStore, I: TokenSource> gateway_authority_reset_claim::ResetClaimFactory
    for Authority<S, I>
{
    type Store = S;
    type Tokens = I;
    fn reset_claim(&self) -> gateway_authority_reset_claim::ResetClaimService<'_, S, I> {
        gateway_authority_reset_claim::ResetClaimService::new(
            &self.store,
            &self.tokens,
            self.clock.as_deref(),
        )
    }
}

impl<S: AuthorityStore, I: TokenSource> gateway_authority_reset_spend::ResetSpendFactory
    for Authority<S, I>
{
    type Store = S;
    type Tokens = I;
    fn reset_spend(&self) -> gateway_authority_reset_spend::ResetSpendService<'_, S, I> {
        gateway_authority_reset_spend::ResetSpendService::new(
            &self.store,
            &self.tokens,
            self.clock.as_deref(),
        )
    }
}

impl<S: AuthorityStore, I: TokenSource> gateway_authority_reset_recovery::ResetRecoveryFactory
    for Authority<S, I>
{
    type Store = S;
    type Tokens = I;
    fn reset_recovery(&self) -> gateway_authority_reset_recovery::ResetRecoveryService<'_, S, I> {
        gateway_authority_reset_recovery::ResetRecoveryService::new(
            &self.store,
            &self.tokens,
            self.clock.as_deref(),
        )
    }
}
