//! Named catalog input reads and codecs; returned rows are DATA, never effect authority.
use gateway_catalog_input::{
    catalog::{CatalogInput, ProviderCatalog},
    runtime_config::normalize_runtime_config,
};
use gateway_catalog_provider::{
    host::{CatalogProvider, CredentialPublication, ProviderBinding, unavailable},
    providers,
};
use gateway_contracts::model::ModelId;
use gateway_core::{
    query::QueryStore, source_control::decode_source_control_record, state::AuthorityStore,
    state::RevisionCheck, state::StoredRecord,
};
use gateway_kv_codec::{key::RecordKey, tuple_key::ScalarKeyPart, tuple_key::TupleKey};
use gateway_model_selection::{selection::ModelWhitelist, selection::ProviderSelection};
use gateway_model_snapshot::snapshot::ModelSnapshot;
use gateway_schema::catalog_store;
use gateway_source::{
    error::GatewayError, source_json::SourceJson, source_serialization as source,
};
use serde_json::Value;
use std::collections::BTreeMap;
#[derive(Clone, Debug)]
pub struct CatalogState {
    pub input: CatalogInput,
    pub runtime: Option<SourceJson>,
    /// The adapter may schedule these through its existing request-owned wait_until.
    /// No cron, alarm, detached task or timer is created by this crate.
    pub refresh_due: Vec<CatalogProvider>,
}
pub fn source_key(parts: &[&str]) -> RecordKey {
    TupleKey::from_scalar_parts(
        &parts
            .iter()
            .map(|p| ScalarKeyPart::String(p))
            .collect::<Vec<_>>(),
    )
    .to_record_key()
}
pub fn selection_key() -> RecordKey {
    source_key(&["uos_ai", "provider_selection", "v1"])
}
pub fn whitelist_key() -> RecordKey {
    source_key(&["uos_ai", "codex_models_whitelist"])
}
pub fn account_models_key() -> RecordKey {
    source_key(&["uos_ai", "codex_account_models", "v1"])
}
pub async fn read_json<S: AuthorityStore>(
    store: &S,
    key: &RecordKey,
) -> Result<Option<StoredRecord>, GatewayError> {
    let record = store.read(key).await?;
    if let Some(record) = &record {
        record.json_value()?;
    }
    Ok(record)
}
/// Validate the stored full snapshot without normalizing its model records.
/// Callers that need source Get semantics retain their original raw value/revision.
pub fn parse_full_snapshot(value: &SourceJson) -> Option<ModelSnapshot> {
    let models = source::member(value, "models")
        .as_ref()
        .and_then(source::array)?;
    let source_name = source::member(value, "source")
        .as_ref()
        .and_then(source::string)?;
    let updated = source::member(value, "updated_at_ms")
        .as_ref()
        .and_then(source::number)?;
    if models.is_empty()
        || !models
            .iter()
            .all(|v| v.as_str().starts_with('{') || v.as_str().starts_with('['))
        || gateway_schema_values::source_view::js_trim_source(&source_name)
            .as_units()
            .is_empty()
        || !updated.is_finite()
        || updated.fract() != 0.0
        || updated <= 0.0
        || updated > gateway_contracts::identity::MAX_SAFE_INTEGER as f64
    {
        return None;
    }
    Some(ModelSnapshot {
        models,
        source: source_name,
        updated_at_ms: updated as i64,
        client_version: source::member(value, "client_version")
            .as_ref()
            .and_then(source::string)
            .and_then(|s| s.try_utf8().ok()),
    })
}
/// Explicit opt-in for named model/default/whitelist records; generic read_json is unchanged.
pub struct SourceControlRow {
    pub record: StoredRecord,
    pub document: SourceJson,
}
async fn read_source_control<S: AuthorityStore>(
    store: &S,
    key: &RecordKey,
) -> Result<Option<SourceControlRow>, GatewayError> {
    store
        .read(key)
        .await?
        .map(|record| {
            let document = decode_source_control_record(&record)?;
            Ok(SourceControlRow { record, document })
        })
        .transpose()
}
pub async fn checks_current<S: AuthorityStore>(
    store: &S,
    checks: &[RevisionCheck],
) -> Result<bool, GatewayError> {
    for fence in checks {
        if store.read(&fence.key).await?.as_ref().map(|r| &r.revision)
            != fence.expected_revision.as_ref()
        {
            return Ok(false);
        }
    }
    Ok(true)
}
pub async fn load_state<S: QueryStore>(
    store: &S,
    publication: &CredentialPublication,
    observations: BTreeMap<CatalogProvider, ProviderCatalog>,
    now: i64,
    public: bool,
) -> Result<CatalogState, GatewayError> {
    publication.validate()?;
    let runtime = read_source_control(store, &catalog_store::runtime_config_key())
        .await?
        .as_ref()
        .map(|r| normalize_runtime_config(&r.document))
        .transpose()?
        .flatten();
    let snapshot = runtime
        .as_ref()
        .and_then(|r| source::member(r, "codex_models"))
        .as_ref()
        .and_then(|v| v.deserialize::<ModelSnapshot>().ok());
    let full_snapshot = read_source_control(store, &catalog_store::codex_snapshot_key())
        // Source loadFullCodexModelsSnapshot catches this optional read only.
        .await
        .ok()
        .flatten()
        .and_then(|r| parse_full_snapshot(&r.document));
    let selection = read_json(store, &selection_key())
        .await?
        .as_ref()
        .and_then(|r| ProviderSelection::normalize(&r.value));
    let whitelist = read_source_control(store, &whitelist_key())
        .await?
        .as_ref()
        .and_then(|r| ModelWhitelist::normalize(&r.document));
    let mut input = CatalogInput {
        snapshot,
        full_snapshot,
        selection,
        whitelist,
        ..CatalogInput::default()
    };
    let mut refresh_due = Vec::new();
    for provider in [
        CatalogProvider::Openlux,
        CatalogProvider::Surplus,
        CatalogProvider::Lithos,
        CatalogProvider::Openrouter,
        CatalogProvider::ClaudeCode,
        CatalogProvider::Deepseek,
        CatalogProvider::Cerebras,
    ] {
        let configured = publication.providers.contains_key(&provider);
        let mut observation = observations.get(&provider).cloned().unwrap_or_default();
        observation.configured = configured;
        if provider != CatalogProvider::Openrouter
            && !configured
            && !(public && provider == CatalogProvider::Surplus)
        {
            observation = ProviderCatalog::default();
        }
        if provider.discovery()
            && (configured
                || provider == CatalogProvider::Openrouter
                || public && provider == CatalogProvider::Surplus)
            && observation
                .updated_at_ms
                .is_none_or(|at| now.saturating_sub(at) >= 300_000)
        {
            refresh_due.push(provider);
        }
        input.providers.insert(provider.id().into(), observation);
    }
    // Public OpenRouter enrichment is usable even without a routing credential.
    let mut ids = input
        .providers
        .values()
        .flat_map(|p| p.models.iter())
        .chain(input.codex().into_iter().flat_map(|s| s.models.iter()))
        .filter_map(|row| {
            ["slug", "id", "model", "name"]
                .iter()
                .find_map(|key| source::member(row, key).as_ref().and_then(source::string))
        })
        .map(ModelId::from_source)
        .collect::<Vec<_>>();
    for provider in ["deepseek", "lithos", "cerebras", "ubiquity"] {
        ids.extend(
            gateway_schema::provider_catalog::provider_ids(provider)
                .iter()
                .map(|id| ModelId::from_scalar(id)),
        );
    }
    let openrouter = input.rows("openrouter").to_vec();
    for id in ids {
        if let Some(row) = providers::match_openrouter(&openrouter, &id) {
            input.enrichment.insert(
                id,
                gateway_model_metadata::metadata::MetadataHint::from_openrouter(row),
            );
        }
    }
    Ok(CatalogState {
        input,
        runtime,
        refresh_due,
    })
}
/// Existing catalog rows only. Chunks are private until one atomic metadata publication
/// checks the source generation, lease owner/revision/expiry and exact credential rows.
/// Provider observations are durable inputs, not the assembled final body.
/// Keys include the auth generation and credential binding so rotation and
/// key changes cannot reuse stale rows.
pub fn provider_observation_key(
    generation: &str,
    provider: CatalogProvider,
    binding: Option<&ProviderBinding>,
) -> RecordKey {
    let identity = binding
        .map(|binding| gateway_schema_values::scalar::sha256(binding.identity.as_bytes()))
        .unwrap_or_else(|| "anonymous".into());
    source_key(&[
        "ubq_ai",
        "provider_catalog_v1",
        generation,
        provider.id(),
        &identity,
    ])
}
pub async fn auth_generation<S: AuthorityStore>(store: &S) -> Result<Option<String>, GatewayError> {
    Ok(read_json(store, &catalog_store::auth_generation_key())
        .await?
        .and_then(|record| record.value.as_str().map(str::to_owned))
        .filter(|value| !value.is_empty()))
}
#[derive(serde::Deserialize, serde::Serialize)]
struct ProviderObservationWire {
    models: Vec<String>,
    updated_at_ms: Option<i64>,
    configured: bool,
    available: bool,
    stale: bool,
}
fn observation_wire(snapshot: &ProviderCatalog) -> Result<ProviderObservationWire, GatewayError> {
    Ok(ProviderObservationWire {
        models: snapshot
            .models
            .iter()
            .map(|model| {
                gateway_source::source_serialization::canonical(model)
                    .map(|value| value.as_str().to_owned())
                    .map_err(|_| unavailable("Provider catalog observation could not be encoded"))
            })
            .collect::<Result<Vec<_>, _>>()?,
        updated_at_ms: snapshot.updated_at_ms,
        configured: snapshot.configured,
        available: snapshot.available,
        stale: snapshot.stale,
    })
}
pub async fn load_provider_observation<S: AuthorityStore>(
    store: &S,
    generation: &str,
    provider: CatalogProvider,
    binding: Option<&ProviderBinding>,
    now: i64,
) -> Result<Option<ProviderCatalog>, GatewayError> {
    let key = provider_observation_key(generation, provider, binding);
    let Some(record) = store.read(&key).await? else {
        return Ok(None);
    };
    record.validate_version()?;
    let Ok(wire) = serde_json::from_value::<ProviderObservationWire>(record.json_value()?.clone())
    else {
        return Ok(None);
    };
    let mut snapshot = ProviderCatalog {
        models: wire
            .models
            .iter()
            .map(|model| gateway_source::source_json::SourceJson::parse(model))
            .collect::<Result<Vec<_>, _>>()
            .unwrap_or_default(),
        updated_at_ms: wire.updated_at_ms,
        configured: wire.configured,
        available: wire.available,
        stale: wire.stale,
    };
    if snapshot
        .updated_at_ms
        .is_none_or(|updated| updated <= 0 || updated > now.saturating_add(60_000))
    {
        return Ok(None);
    }
    snapshot.stale = snapshot
        .updated_at_ms
        .is_some_and(|updated| now.saturating_sub(updated) >= 300_000);
    Ok(Some(snapshot))
}

/// Read only the fixed named catalog control family; no caller-selected key.
pub async fn read_full_snapshot<S: AuthorityStore>(
    store: &S,
) -> Result<Option<SourceControlRow>, GatewayError> {
    read_source_control(store, &catalog_store::codex_snapshot_key()).await
}

/// Read only the fixed named catalog control family; no caller-selected key.
pub async fn read_runtime_config<S: AuthorityStore>(
    store: &S,
) -> Result<Option<SourceControlRow>, GatewayError> {
    read_source_control(store, &catalog_store::runtime_config_key()).await
}

/// Read only the fixed named catalog control family; no caller-selected key.
pub async fn read_whitelist<S: AuthorityStore>(
    store: &S,
) -> Result<Option<SourceControlRow>, GatewayError> {
    read_source_control(store, &whitelist_key()).await
}

/// Read only the fixed named catalog control family; no caller-selected key.
pub async fn read_account_models<S: AuthorityStore>(
    store: &S,
) -> Result<Option<SourceControlRow>, GatewayError> {
    read_source_control(store, &account_models_key()).await
}

/// Encode observation DATA only; runtime retains all currentness and write ownership.
pub fn encode_provider_observation(snapshot: &ProviderCatalog) -> Result<Value, GatewayError> {
    serde_json::to_value(observation_wire(snapshot)?)
        .map_err(|_| unavailable("Provider catalog observation could not be encoded"))
}
