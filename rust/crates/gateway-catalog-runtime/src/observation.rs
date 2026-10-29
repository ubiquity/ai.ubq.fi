//! Current provider observation writes remain co-owned with the private runtime memo.
use gateway_catalog_input::catalog::ProviderCatalog;
use gateway_catalog_inputs::state::{encode_provider_observation, provider_observation_key};
use gateway_catalog_provider::host::{CatalogProvider, ProviderBinding};
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, RevisionCheck, StoredRecord,
};
use gateway_kv_codec::key::RecordKey;
use gateway_source::error::GatewayError;
use serde_json::Value;
pub(crate) fn check(key: RecordKey, record: Option<&StoredRecord>) -> RevisionCheck {
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
pub(crate) async fn persist_provider_observation<S: AuthorityStore>(
    store: &S,
    generation: &str,
    provider: CatalogProvider,
    binding: Option<&ProviderBinding>,
    snapshot: &ProviderCatalog,
    revision: &str,
    now: i64,
) -> Result<(), GatewayError> {
    let key = provider_observation_key(generation, provider, binding);
    let value = encode_provider_observation(snapshot)?;
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
