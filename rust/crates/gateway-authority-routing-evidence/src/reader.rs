//! Closed routing read set. Every observed physical revision must join the real final CAS.
use crate::{
    capacity::{CapacityStore, parse_capacity_store},
    models::{AccountCatalog, ModelAvailability, UnsupportedModel, unsupported_detail},
    validation::{validate_binding, validate_pool},
};
use gateway_auth::provider_credentials::{codex_source_pool_key, parse_codex_source_pool};
use gateway_authority_contracts::{generation, invalid, text, unavailable, validate_time};
use gateway_authority_keys as keys;
use gateway_authority_routing_records::{
    codec::{normalize, parse_routing_state},
    types::{RoutingState, SubscriptionPool},
};
use gateway_core::{
    error::GatewayError,
    identity::ActiveSelection,
    provider::OveragePermission,
    source_control::decode_source_control_record,
    source_json::SourceJson,
    state::{AuthorityStore, RecordKey, RevisionCheck, StoredRecord},
};
use gateway_schema::selection::{AccountModelsStore, ProviderSelection};
use serde::de::DeserializeOwned;
use serde_json::Value;
use std::collections::BTreeMap;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RoutingReadMode {
    GlobalSelection,
    PinnedObservation,
}

/// Read-only data. Construction neither elects an account nor establishes currentness.
/// Raw auth rows remain private in the reader cache and are dropped after hydration.
///
/// External data cannot construct an observed read set:
/// ```compile_fail
/// use gateway_authority_routing_evidence::reader::RoutingEvidence;
/// let _ = serde_json::from_str::<RoutingEvidence>("{}");
/// ```
///
/// An empty read set cannot be substituted for actual fixed-family reads:
/// ```compile_fail
/// use gateway_authority_routing_evidence::reader::RoutingEvidence;
/// let _ = RoutingEvidence::default();
/// ```
pub struct RoutingEvidence {
    mode: RoutingReadMode,
    observed_at_ms: i64,
    pool: SubscriptionPool,
    state: RoutingState,
    active: Option<ActiveSelection>,
    capacity: CapacityStore,
    models: ModelAvailability,
    models_source: SourceJson,
    source_ids: BTreeMap<String, String>,
    overage: BTreeMap<String, bool>,
    checks: Vec<RevisionCheck>,
}

impl RoutingEvidence {
    pub fn mode(&self) -> RoutingReadMode {
        self.mode
    }
    pub fn observed_at_ms(&self) -> i64 {
        self.observed_at_ms
    }
    pub fn pool(&self) -> &SubscriptionPool {
        &self.pool
    }
    pub fn state(&self) -> &RoutingState {
        &self.state
    }
    pub fn active(&self) -> Option<&ActiveSelection> {
        self.active.as_ref()
    }
    pub fn capacity(&self) -> &CapacityStore {
        &self.capacity
    }
    pub fn models(&self) -> &ModelAvailability {
        &self.models
    }
    pub fn models_source(&self) -> &SourceJson {
        &self.models_source
    }
    pub fn source_ids(&self) -> &BTreeMap<String, String> {
        &self.source_ids
    }
    pub fn overage(&self) -> &BTreeMap<String, bool> {
        &self.overage
    }

    /// Ordinary fences only: no verification CAS is performed. Consumers must include
    /// this complete slice in their existing atomic effect's final revision checks.
    pub fn revision_checks(&self) -> &[RevisionCheck] {
        &self.checks
    }
}

/// Actual store reads only, with no raw-store accessor, callback, arbitrary key API,
/// mutation, token source, proof constructor, lease or standalone currentness grant.
pub struct RoutingEvidenceReader<'a, S> {
    store: &'a S,
    now: i64,
    reads: BTreeMap<String, Option<StoredRecord>>,
}

impl<'a, S: AuthorityStore> RoutingEvidenceReader<'a, S> {
    pub fn new(store: &'a S, now: i64) -> Result<Self, GatewayError> {
        validate_time(now)?;
        Ok(Self {
            store,
            now,
            reads: BTreeMap::new(),
        })
    }

    pub async fn read(mut self, mode: RoutingReadMode) -> Result<RoutingEvidence, GatewayError> {
        let mut pool: SubscriptionPool = self.read_json(&keys::pool()).await?.ok_or_else(|| {
            unavailable("codex_routing_unavailable", "Codex pool is unavailable.")
        })?;
        validate_pool(&pool)?;
        let state = normalize(
            &pool,
            self.read_json::<Value>(&keys::routing())
                .await?
                .as_ref()
                .map(parse_routing_state)
                .transpose()?,
            self.now,
        )?;
        let active: Option<ActiveSelection> = match mode {
            RoutingReadMode::GlobalSelection => self.read_json(&keys::active()).await?,
            RoutingReadMode::PinnedObservation => None,
        };
        if let Some(active) = &active {
            validate_binding(&active.binding)?;
            if active.v != 1 || active.generation == 0 {
                return Err(invalid("Malformed active Codex selection."));
            }
            generation(active.generation)?;
        }
        let capacity = self
            .read_json::<Value>(&keys::capacity())
            .await?
            .as_ref()
            .map(parse_capacity_store)
            .transpose()?
            .unwrap_or_default();
        let (models, models_source, source_ids) = self.load_models(&pool).await?;
        let selection: Option<Value> = self.read_json(&keys::provider_selection()).await?;
        let selection = selection
            .as_ref()
            .filter(|value| !value.is_null())
            .map(|value| {
                ProviderSelection::normalize(value)
                    .ok_or_else(|| invalid("Current source provider selection is malformed."))
            })
            .transpose()?;
        for account in &mut pool.accounts {
            account.enabled = selection
                .as_ref()
                .is_none_or(|selection| selection.account_eligible(&account.admin_account_hash.0));
        }
        let mut overage = BTreeMap::new();
        for account in &pool.accounts {
            let setting: Option<Value> = self
                .read_json(&keys::overage(&account.admin_account_hash.0))
                .await?;
            overage.insert(
                account.account_id_hash.0.clone(),
                OveragePermission::from_stored_record(setting.as_ref()).allows_dispatch(),
            );
        }
        Ok(RoutingEvidence {
            mode,
            observed_at_ms: self.now,
            pool,
            state,
            active,
            capacity,
            models,
            models_source,
            source_ids,
            overage,
            checks: self.checks(),
        })
    }

    async fn cached_record(
        &mut self,
        key: &RecordKey,
    ) -> Result<Option<&StoredRecord>, GatewayError> {
        if !self.reads.contains_key(&key.0) {
            let record = self.store.read(key).await?;
            if let Some(row) = &record {
                row.validate_version()?;
                text(&row.revision)?;
                if let Some(expiry) = row.expires_at_ms {
                    validate_time(expiry)?;
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

    async fn read_json<T: DeserializeOwned>(
        &mut self,
        key: &RecordKey,
    ) -> Result<Option<T>, GatewayError> {
        self.cached_record(key)
            .await?
            .map(|row| {
                serde_json::from_value(row.json_value()?.clone())
                    .map_err(|_| invalid("Malformed authority record."))
            })
            .transpose()
    }

    async fn load_models(
        &mut self,
        pool: &SubscriptionPool,
    ) -> Result<(ModelAvailability, SourceJson, BTreeMap<String, String>), GatewayError> {
        let source_key = codex_source_pool_key();
        let source: Value = self.read_json(&source_key).await?.ok_or_else(|| {
            unavailable(
                "codex_routing_unavailable",
                "Current raw Codex source pool is unavailable.",
            )
        })?;
        let source = parse_codex_source_pool(&source)?
            .ok_or_else(|| invalid("Current raw Codex source pool is malformed."))?;
        if self.revision(&source_key).as_deref() != Some(pool.pool_versionstamp.0.as_str())
            || source.accounts.len() != pool.accounts.len()
        {
            return Err(unavailable(
                "codex_routing_unavailable",
                "Normalized Codex pool is not bound to the current source revision.",
            ));
        }
        let mut ids = BTreeMap::new();
        for (raw, normalized) in source.accounts.iter().zip(&pool.accounts) {
            let identity = raw.identity();
            if identity.routing_account_hash != normalized.account_id_hash
                || identity.credential_version != normalized.credential_version
                || identity.administrative_account_hash != normalized.admin_account_hash
            {
                return Err(unavailable(
                    "codex_routing_unavailable",
                    "Normalized Codex account identity differs from the current raw credential.",
                ));
            }
            ids.insert(normalized.account_id_hash.0.clone(), raw.account_id.clone());
        }
        let raw = self
            .cached_record(&keys::models())
            .await?
            .map(|row| {
                decode_source_control_record(row)?
                    .deserialize::<SourceJson>()
                    .map_err(|_| invalid("Malformed authority record."))
            })
            .transpose()?
            .map_or_else(
                || SourceJson::parse("{\"accounts\":{},\"unsupported\":{}}"),
                Ok,
            )?;
        let source = AccountModelsStore::parse(&raw).unwrap_or_default();
        let mut models = ModelAvailability::default();
        for (hash, id) in &ids {
            if let Some(catalog) = source.accounts.get(id) {
                models.catalogs.insert(
                    hash.clone(),
                    AccountCatalog {
                        client_version: catalog.client_version.clone(),
                        models: catalog.slugs.clone(),
                    },
                );
            }
            if let Some(unsupported) = source.unsupported.get(id) {
                models.unsupported.insert(
                    hash.clone(),
                    unsupported
                        .iter()
                        .map(|(model, at)| {
                            (
                                model.clone(),
                                UnsupportedModel {
                                    observed_at_ms: *at,
                                    detail: unsupported_detail(model),
                                },
                            )
                        })
                        .collect(),
                );
            }
        }
        Ok((models, raw, ids))
    }

    fn revision(&self, key: &RecordKey) -> Option<String> {
        self.reads
            .get(&key.0)
            .and_then(Option::as_ref)
            .map(|row| row.revision.clone())
    }

    fn checks(&self) -> Vec<RevisionCheck> {
        self.reads
            .iter()
            .map(|(key, record)| RevisionCheck {
                key: RecordKey(key.clone()),
                expected_revision: record.as_ref().map(|row| row.revision.clone()),
            })
            .collect()
    }
}
