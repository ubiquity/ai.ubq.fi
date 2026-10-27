//! Private exact-version lease/currentness publication; no public prepared or write factory.
use gateway_catalog_inputs::state::{
    account_models_key, checks_current, read_account_models, read_full_snapshot, read_json,
    read_runtime_config,
};
use gateway_catalog_provider::host::{CatalogHost, CredentialPublication, unavailable};
use gateway_core::{
    error::GatewayError,
    model::ModelId,
    source_control::encode_source_control_record,
    source_json::SourceJson,
    state::{
        AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, RecordMutation, RevisionCheck,
        StoredRecord,
    },
};
use gateway_schema::{
    catalog_cache::{PreparedCatalog, REFRESH_LEASE_MS},
    catalog_store::{self, RefreshLease},
    selection::{AccountModelCatalog, AccountModelsStore},
    snapshot::ModelSnapshot,
};
use serde_json::{Value, json};
pub(crate) async fn load_exact_catalog<S: AuthorityStore>(
    store: &S,
    version: &str,
    generation: &str,
    now: i64,
) -> Result<Option<gateway_schema::catalog_cache::LoadedCatalog>, GatewayError> {
    let loaded = catalog_store::load_catalog(store, version, generation, now).await?;
    if loaded
        .as_ref()
        .is_some_and(|c| c.metadata.client_version != version)
    {
        return Err(unavailable(
            "Stored Codex catalog belongs to another exact client version",
        ));
    }
    Ok(loaded)
}
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
fn source_mutation(
    key: RecordKey,
    value: &SourceJson,
    revision: &str,
    expires: Option<i64>,
) -> Result<RecordMutation, GatewayError> {
    Ok(RecordMutation::Put {
        key,
        record: encode_source_control_record(revision, value, expires)?,
    })
}
pub(crate) async fn generation_current<S: AuthorityStore>(
    store: &S,
    generation: &str,
) -> Result<bool, GatewayError> {
    Ok(read_json(store, &catalog_store::auth_generation_key())
        .await?
        .as_ref()
        .and_then(|r| r.value.as_str())
        == Some(generation))
}
pub(crate) async fn publish_fenced<S: AuthorityStore, H: CatalogHost>(
    store: &S,
    prepared: &PreparedCatalog,
    owner: &str,
    source_checks: &[RevisionCheck],
    host: &H,
) -> Result<bool, GatewayError> {
    let m = &prepared.metadata;
    let generation = read_json(store, &catalog_store::auth_generation_key()).await?;
    let lease = read_json(store, &catalog_store::lease_key(&m.client_version)).await?;
    let current_lease = lease
        .as_ref()
        .and_then(|r| serde_json::from_value::<RefreshLease>(r.value.clone()).ok());
    if generation.as_ref().and_then(|r| r.value.as_str()) != Some(&m.auth_generation)
        || !current_lease.is_some_and(|l| l.owner == owner && l.lease_until_ms > host.now_ms())
        || !checks_current(store, source_checks).await?
    {
        return Ok(false);
    }
    let mut chunk_checks = Vec::new();
    let mut chunk_mutations = Vec::new();
    for (source, chunks) in [(false, &prepared.chunks), (true, &prepared.source_chunks)] {
        for (index, bytes) in chunks.iter().enumerate() {
            let key =
                catalog_store::chunk_key(&m.client_version, &m.body_generation, index, source);
            chunk_checks.push(check(key.clone(), None));
            chunk_mutations.push(mutation(
                key,
                json!(bytes),
                &m.body_generation,
                Some(prepared.expires_at_ms),
            ));
        }
    }
    if store
        .commit(AtomicTransition {
            checks: chunk_checks,
            mutations: chunk_mutations,
        })
        .await?
        != CommitOutcome::Committed
    {
        return Ok(false);
    }
    if !current_lease_matches(store, &m.client_version, owner, host.now_ms()).await? {
        catalog_store::delete_catalog_chunks(store, m).await?;
        return Ok(false);
    }
    let key = catalog_store::metadata_key(&m.client_version);
    let previous = read_json(store, &key).await?;
    let mut checks = source_checks.to_vec();
    checks.extend([
        check(catalog_store::auth_generation_key(), generation.as_ref()),
        check(catalog_store::lease_key(&m.client_version), lease.as_ref()),
        check(key.clone(), previous.as_ref()),
    ]);
    let committed = store
        .commit(AtomicTransition {
            checks,
            mutations: vec![mutation(
                key,
                serde_json::to_value(m)
                    .map_err(|_| unavailable("Catalog metadata could not be encoded"))?,
                &m.body_generation,
                Some(prepared.expires_at_ms),
            )],
        })
        .await?
        == CommitOutcome::Committed;
    if !committed {
        catalog_store::delete_catalog_chunks(store, m).await?;
        return Ok(false);
    }
    if let Some(old) = previous
        .and_then(|r| {
            serde_json::from_value::<gateway_schema::catalog_cache::CatalogMetadata>(r.value).ok()
        })
        .filter(|old| old.body_generation != m.body_generation)
    {
        catalog_store::delete_catalog_chunks(store, &old).await?;
    }
    Ok(true)
}
pub(crate) struct PublicationContext<'a> {
    pub version: &'a str,
    pub publication: &'a CredentialPublication,
    pub generation: &'a str,
    pub revision: &'a str,
    pub now: i64,
}
pub(crate) async fn current_lease_matches<S: AuthorityStore>(
    store: &S,
    version: &str,
    owner: &str,
    now: i64,
) -> Result<bool, GatewayError> {
    let lease = read_json(store, &catalog_store::lease_key(version)).await?;
    Ok(lease
        .and_then(|r| serde_json::from_value::<RefreshLease>(r.value).ok())
        .is_some_and(|l| l.owner == owner && l.lease_until_ms > now))
}
pub(crate) async fn publish_account_models<S: AuthorityStore>(
    store: &S,
    entries: &[(String, Vec<ModelId>)],
    context: &PublicationContext<'_>,
) -> Result<(), GatewayError> {
    if entries.is_empty() {
        return Ok(());
    }
    let PublicationContext {
        version,
        publication,
        generation,
        revision,
        now,
    } = context;
    for _ in 0..3 {
        let auth = read_json(store, &catalog_store::auth_generation_key()).await?;
        if auth.as_ref().and_then(|r| r.value.as_str()) != Some(*generation) {
            return Ok(());
        }
        let old = read_account_models(store).await?;
        let data = old
            .as_ref()
            .and_then(|r| AccountModelsStore::parse(&r.document))
            .unwrap_or_default();
        let entries = entries
            .iter()
            .map(|(id, slugs)| {
                (
                    id.clone(),
                    AccountModelCatalog {
                        slugs: slugs.clone(),
                        client_version: (*version).into(),
                        updated_at_ms: *now,
                    },
                )
            })
            .collect::<Vec<_>>();
        let next = data.record_catalogs(&entries);
        let mut checks = publication.source_checks.clone();
        checks.extend([
            check(catalog_store::auth_generation_key(), auth.as_ref()),
            check(account_models_key(), old.as_ref().map(|r| &r.record)),
        ]);
        if store
            .commit(AtomicTransition {
                checks,
                mutations: vec![source_mutation(
                    account_models_key(),
                    &next.to_source_json(),
                    revision,
                    None,
                )?],
            })
            .await?
            == CommitOutcome::Committed
        {
            break;
        }
    }
    Ok(())
}
pub(crate) async fn publish_snapshot<S: AuthorityStore>(
    store: &S,
    parsed: &SourceJson,
    context: &PublicationContext<'_>,
) -> Result<(), GatewayError> {
    let PublicationContext {
        version,
        publication,
        generation,
        revision,
        now,
    } = context;
    for _ in 0..3 {
        let auth = read_json(store, &catalog_store::auth_generation_key()).await?;
        if auth.as_ref().and_then(|r| r.value.as_str()) != Some(*generation) {
            return Ok(());
        }
        let snapshot = read_full_snapshot(store).await?;
        let runtime = read_runtime_config(store).await?;
        let current = snapshot
            .as_ref()
            .map(|r| {
                r.document
                    .deserialize::<ModelSnapshot>()
                    .map_err(|_| unavailable("Stored Codex model snapshot is corrupt"))
            })
            .transpose()?;
        let Some((next, next_runtime)) = catalog_store::normalized_snapshot_plan(
            parsed,
            version,
            current.as_ref(),
            runtime
                .as_ref()
                .map(|r| &r.document)
                .unwrap_or(&SourceJson::parse("null")?),
            *now,
        ) else {
            return Ok(());
        };
        let mut checks = publication.source_checks.clone();
        checks.extend([
            check(catalog_store::auth_generation_key(), auth.as_ref()),
            check(
                catalog_store::codex_snapshot_key(),
                snapshot.as_ref().map(|r| &r.record),
            ),
            check(
                catalog_store::runtime_config_key(),
                runtime.as_ref().map(|r| &r.record),
            ),
        ]);
        if store
            .commit(AtomicTransition {
                checks,
                mutations: vec![
                    source_mutation(
                        catalog_store::codex_snapshot_key(),
                        &SourceJson::from_serializable(&next)?,
                        revision,
                        None,
                    )?,
                    source_mutation(
                        catalog_store::runtime_config_key(),
                        &next_runtime,
                        revision,
                        None,
                    )?,
                ],
            })
            .await?
            == CommitOutcome::Committed
        {
            return Ok(());
        }
    }
    Ok(())
}
/// Renew only request-owned leases; no independent periodic maintenance exists.
pub(crate) const HEARTBEAT_MS: u32 = (REFRESH_LEASE_MS / 3) as u32;
