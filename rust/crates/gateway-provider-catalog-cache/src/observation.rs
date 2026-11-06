//! Existing observation codec and CAS storage with explicit provider cache scope.
use gateway_core::{
    error::GatewayError,
    state::{
        AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, RecordMutation, RevisionCheck,
        StoredRecord,
    },
    tuple_key::{ScalarKeyPart, TupleKey},
};
use gateway_schema::catalog::ProviderCatalog;
use serde_json::Value;

pub struct Scope<'a> {
    pub generation: &'a str,
    pub provider: &'a str,
    pub binding: Option<&'a str>,
}
pub fn scope<'a>(
    provider: &'a str,
    generation: Option<&'a str>,
    binding: Option<&'a str>,
) -> Option<Scope<'a>> {
    Some(Scope {
        generation: if provider == "openrouter" {
            "public-openrouter-models"
        } else {
            generation?
        },
        provider,
        binding,
    })
}
fn unavailable(message: &str) -> GatewayError {
    GatewayError::new(502, message, Some("codex_catalog_unavailable"))
}
fn key(scope: &Scope<'_>) -> RecordKey {
    let identity = scope
        .binding
        .map(|binding| gateway_schema::util::sha256(binding.as_bytes()))
        .unwrap_or_else(|| "anonymous".into());
    TupleKey::from_scalar_parts(
        &[
            "ubq_ai",
            "provider_catalog_v1",
            scope.generation,
            scope.provider,
            &identity,
        ]
        .map(ScalarKeyPart::String),
    )
    .to_record_key()
}
fn check(key: RecordKey, record: Option<&StoredRecord>) -> RevisionCheck {
    RevisionCheck {
        key,
        expected_revision: record.map(|r| r.revision.clone()),
    }
}
fn mutation(key: RecordKey, value: Value, revision: &str, expires: Option<i64>) -> RecordMutation {
    RecordMutation::Put {
        key,
        record: StoredRecord::json(revision, value, expires),
    }
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
                gateway_core::source_serialization::canonical(model)
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
pub async fn load<S: AuthorityStore>(
    store: &S,
    scope: &Scope<'_>,
    now: i64,
) -> Result<Option<ProviderCatalog>, GatewayError> {
    let key = key(scope);
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
            .map(|model| gateway_core::source_json::SourceJson::parse(model))
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
pub async fn persist<S: AuthorityStore>(
    store: &S,
    scope: &Scope<'_>,
    snapshot: &ProviderCatalog,
    revision: &str,
    now: i64,
) -> Result<(), GatewayError> {
    let key = key(scope);
    let value = serde_json::to_value(observation_wire(snapshot)?)
        .map_err(|_| unavailable("Provider catalog observation could not be encoded"))?;
    for _ in 0..3 {
        let row = store.read(&key).await?;
        if store
            .commit(AtomicTransition {
                checks: vec![check(key.clone(), row.as_ref())],
                mutations: vec![mutation(
                    key.clone(),
                    value.clone(),
                    revision,
                    Some(now.saturating_add(gateway_schema::catalog_cache::RETENTION_MS)),
                )],
            })
            .await?
            == CommitOutcome::Committed
        {
            return Ok(());
        }
    }
    Ok(())
}
