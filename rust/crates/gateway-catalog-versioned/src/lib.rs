//! Exact-version catalog operation with private lease and publication ownership.
mod state;
use gateway_catalog_inputs::{
    codex::{Fetched, degraded, fetch_codex},
    state as inputs,
};
use gateway_catalog_provider::host::{
    CatalogHost, CatalogMaintenance, CatalogObservation, CatalogProvider, unavailable,
};
use gateway_catalog_runtime::CatalogRuntime;
use gateway_core::{error::GatewayError, query::QueryStore, source_json::SourceJson};
use gateway_schema::{
    catalog_cache::{self, CacheState, CatalogResponse, LoadedCatalog},
    catalog_store,
};
use std::{
    future::{Future, poll_fn},
    task::Poll,
};
struct Lease<'a> {
    version: &'a str,
    owner: &'a str,
    generation: &'a str,
}
/// The request-owned heartbeat exists only while the one fetch future is live.
async fn with_lease<
    S: QueryStore,
    H: CatalogHost,
    F: Future<Output = Result<Fetched, GatewayError>>,
>(
    store: &S,
    host: &H,
    lease: &Lease<'_>,
    fetch: F,
) -> Result<Fetched, GatewayError> {
    enum Step<T> {
        Fetch(T),
        Tick(Result<(), GatewayError>),
    }
    let mut fetch = std::pin::pin!(fetch);
    loop {
        let mut wait = std::pin::pin!(host.wait(state::HEARTBEAT_MS));
        let step = poll_fn(|cx| {
            if let Poll::Ready(response) = fetch.as_mut().poll(cx) {
                return Poll::Ready(Step::Fetch(response));
            }
            if let Poll::Ready(tick) = wait.as_mut().poll(cx) {
                return Poll::Ready(Step::Tick(tick));
            }
            Poll::Pending
        })
        .await;
        match step {
            Step::Fetch(result) => return result,
            Step::Tick(tick) => {
                tick?;
                if !state::generation_current(store, lease.generation).await?
                    || !catalog_store::renew_refresh_lease(
                        store,
                        lease.version,
                        lease.owner,
                        host.now_ms(),
                    )
                    .await?
                {
                    return Err(catalog_cache::rotation_unavailable());
                }
            }
        }
    }
}
async fn response<S: QueryStore, H: CatalogHost>(
    runtime: &CatalogRuntime,
    store: &S,
    host: &H,
    cached: &LoadedCatalog,
    conditional: Option<&str>,
    cache_state: CacheState,
) -> Result<CatalogResponse, GatewayError> {
    let publication = host.publication().await?;
    let state = runtime
        .load_state(store, &publication, host.now_ms())
        .await?;
    Ok(catalog_cache::catalog_response(
        cached,
        conditional,
        cache_state,
        &state.input,
    ))
}
async fn fallback<S: QueryStore, H: CatalogHost>(
    runtime: &CatalogRuntime,
    store: &S,
    host: &H,
    message: &str,
) -> Result<CatalogResponse, GatewayError> {
    let publication = host.publication().await?;
    let state = runtime
        .load_state(store, &publication, host.now_ms())
        .await?;
    for provider in [
        CatalogProvider::Openlux,
        CatalogProvider::Surplus,
        CatalogProvider::ClaudeCode,
    ] {
        if state.input.enabled(provider.id()) {
            runtime
                .refresh_provider(
                    store,
                    host,
                    provider,
                    provider != CatalogProvider::ClaudeCode,
                )
                .await?;
        }
    }
    let publication = host.publication().await?;
    let state = runtime
        .load_state(store, &publication, host.now_ms())
        .await?;
    catalog_cache::fallback_response(&state.input).ok_or_else(|| unavailable(message))
}
async fn current<S: QueryStore, H: CatalogHost>(
    store: &S,
    host: &H,
    version: &str,
) -> Result<Option<LoadedCatalog>, GatewayError> {
    let Some(generation) = inputs::read_json(store, &catalog_store::auth_generation_key())
        .await?
        .and_then(|r| r.value.as_str().map(str::to_owned))
    else {
        return Ok(None);
    };
    state::load_exact_catalog(store, version, &generation, host.now_ms()).await
}
async fn recover<S: QueryStore, H: CatalogHost>(
    runtime: &CatalogRuntime,
    store: &S,
    host: &H,
    lease: &Lease<'_>,
    cached: Option<&LoadedCatalog>,
    conditional: Option<&str>,
    strict_rotation: bool,
) -> Result<CatalogResponse, GatewayError> {
    let current_generation = state::generation_current(store, lease.generation)
        .await
        .unwrap_or(false);
    if current_generation && let Some(cached) = cached {
        return response(runtime, store, host, cached, conditional, CacheState::Stale).await;
    }
    if let Some(replacement) = current(store, host, lease.version).await? {
        return response(
            runtime,
            store,
            host,
            &replacement,
            conditional,
            if current_generation {
                CacheState::Miss
            } else {
                CacheState::Rotated
            },
        )
        .await;
    }
    if strict_rotation {
        return Err(catalog_cache::rotation_unavailable());
    }
    fallback(
        runtime,
        store,
        host,
        "Codex upstream model catalog is unavailable",
    )
    .await
}

async fn refresh<S: QueryStore, H: CatalogHost>(
    runtime: &CatalogRuntime,
    store: &S,
    host: &H,
    lease: &Lease<'_>,
    cached: Option<&LoadedCatalog>,
    conditional: Option<&str>,
    started: i64,
) -> Result<CatalogResponse, GatewayError> {
    let publication = host.publication().await?;
    if !inputs::checks_current(store, &publication.source_checks).await? {
        return recover(runtime, store, host, lease, cached, conditional, true).await;
    }
    let etag = cached
        .filter(|c| c.source_body.is_some())
        .and_then(|c| c.metadata.etag.clone());
    let fetched = match with_lease(
        store,
        host,
        lease,
        fetch_codex(host, &publication, lease.version, etag),
    )
    .await
    {
        Ok(fetched) => fetched,
        Err(error) => {
            if degraded(&error) {
                observe_degradation(host, lease.version).await;
            }
            if error.status == 499 {
                return Err(error);
            }
            return recover(runtime, store, host, lease, cached, conditional, false).await;
        }
    };
    if fetched.degraded {
        observe_degradation(host, lease.version).await;
    }
    if !state::generation_current(store, lease.generation).await?
        || !inputs::checks_current(store, &fetched.publication.source_checks).await?
    {
        return recover(runtime, store, host, lease, None, conditional, true).await;
    }
    let context = state::PublicationContext {
        version: lease.version,
        generation: lease.generation,
        publication: &fetched.publication,
        revision: &host.unique_id()?,
        now: host.now_ms(),
    };
    state::publish_account_models(store, &fetched.evidence, &context).await?;
    let revalidated = fetched.response.status == 304 && cached.is_some();
    let body = if revalidated {
        let Some(body) = cached.and_then(|c| c.source_body.as_ref()) else {
            return recover(runtime, store, host, lease, cached, conditional, false).await;
        };
        body.clone()
    } else {
        if !(200..300).contains(&fetched.response.status)
            || !fetched.response.is_json()
            || catalog_cache::parse_catalog(&fetched.response.body).is_none()
        {
            return recover(runtime, store, host, lease, cached, conditional, false).await;
        }
        fetched.response.body.clone()
    };
    let state = runtime
        .load_state(store, &fetched.publication, host.now_ms())
        .await?;
    let revision = host.unique_id()?;
    let (etag, content_type) = match (revalidated, cached) {
        (true, Some(c)) => (
            c.metadata.etag.as_deref(),
            Some(c.metadata.content_type.as_str()),
        ),
        (true, None) => (None, None),
        (false, _) => (
            fetched.response.etag.as_deref(),
            fetched.response.content_type.as_deref(),
        ),
    };
    let prepared = catalog_cache::prepare_catalog(
        catalog_cache::CatalogIdentity {
            client_version: lease.version,
            auth_generation: lease.generation,
            body_generation: &revision,
        },
        &body,
        etag,
        content_type,
        started,
        &state.input.enrichment,
    )?;
    if !state::publish_fenced(
        store,
        &prepared,
        lease.owner,
        &fetched.publication.source_checks,
        host,
    )
    .await?
    {
        return recover(runtime, store, host, lease, None, conditional, true).await;
    }
    let context = state::PublicationContext {
        version: lease.version,
        generation: lease.generation,
        publication: &fetched.publication,
        revision: &revision,
        now: started,
    };
    let body = (!revalidated).then_some(body.as_str());
    finish_refresh(runtime, store, host, lease, conditional, context, body).await
}

async fn finish_refresh<S: QueryStore, H: CatalogHost>(
    runtime: &CatalogRuntime,
    store: &S,
    host: &H,
    lease: &Lease<'_>,
    conditional: Option<&str>,
    context: state::PublicationContext<'_>,
    body: Option<&str>,
) -> Result<CatalogResponse, GatewayError> {
    if let Some(body) = body {
        // The optional full snapshot cannot turn a successfully cached exact-version answer into a guessed schema.
        if let Err(error) = state::publish_snapshot(
            store,
            &SourceJson::parse(body).map_err(|_| unavailable("Catalog parsing failed"))?,
            &context,
        )
        .await
        {
            maintenance(host, CatalogMaintenance::SnapshotPublication, error).await;
        }
    }
    if let Err(error) =
        catalog_store::prune_stored_catalog_versions(store, lease.version, host.now_ms()).await
    {
        maintenance(host, CatalogMaintenance::VersionPruning, error).await;
    }
    if !state::generation_current(store, lease.generation).await? {
        return recover(runtime, store, host, lease, None, conditional, true).await;
    }
    let catalog = state::load_exact_catalog(store, lease.version, lease.generation, host.now_ms())
        .await?
        .ok_or_else(|| unavailable("Codex model catalog could not be read after caching"))?;
    let cache = body.map_or(CacheState::Revalidated, |_| CacheState::Miss);
    response(runtime, store, host, &catalog, conditional, cache).await
}

async fn observe_degradation<H: CatalogHost>(host: &H, version: &str) {
    let _ = host
        .observe(CatalogObservation::ProviderDegradation {
            client_version: version.into(),
        })
        .await;
}

async fn maintenance<H: CatalogHost>(host: &H, operation: CatalogMaintenance, error: GatewayError) {
    let _ = host
        .observe(CatalogObservation::MaintenanceFailure {
            operation,
            code: error.envelope.error.code,
        })
        .await;
}

/// Complete exact-version catalog operation; leases and publication remain private.
pub trait VersionedCatalog {
    fn versioned_models<S: QueryStore, H: CatalogHost>(
        &self,
        store: &S,
        host: &H,
        raw_version: &str,
        conditional: Option<&str>,
    ) -> impl Future<Output = Result<CatalogResponse, GatewayError>>;
}
impl VersionedCatalog for CatalogRuntime {
    async fn versioned_models<S: QueryStore, H: CatalogHost>(
        &self,
        store: &S,
        host: &H,
        raw_version: &str,
        conditional: Option<&str>,
    ) -> Result<CatalogResponse, GatewayError> {
        let runtime = self;
        let version = catalog_cache::validate_client_version(raw_version)?;
        let publication = host.publication().await?;
        publication.validate()?;
        let state = runtime
            .load_state(store, &publication, host.now_ms())
            .await?;
        // Stale paid catalogs return immediately; cold paid catalogs and Claude retain source request ownership.
        for provider in [
            CatalogProvider::Openlux,
            CatalogProvider::Surplus,
            CatalogProvider::ClaudeCode,
        ] {
            if state.input.enabled(provider.id())
                && (provider == CatalogProvider::ClaudeCode
                    || runtime.cached_provider(&publication, provider).is_none())
            {
                runtime
                    .refresh_provider(store, host, provider, false)
                    .await?;
            }
        }
        let generation = match catalog_store::get_auth_generation(store, &host.unique_id()?).await {
            Ok(generation) => generation,
            Err(_) => {
                return fallback(
                    runtime,
                    store,
                    host,
                    "Codex model catalog cache is unavailable",
                )
                .await;
            }
        };
        let now = host.now_ms();
        let cached = state::load_exact_catalog(store, &version, &generation, now).await?;
        if let Some(cached) = &cached
            && cached.metadata.fresh(now)
        {
            return response(runtime, store, host, cached, conditional, CacheState::Hit).await;
        }
        let owner = host.unique_id()?;
        if !catalog_store::acquire_refresh_lease(store, &version, &owner, now).await? {
            if let Some(cached) = cached {
                return response(
                    runtime,
                    store,
                    host,
                    &cached,
                    conditional,
                    CacheState::Stale,
                )
                .await;
            }
            let deadline = now.saturating_add(catalog_cache::COLD_WAIT_MS);
            for _ in 0..51 {
                if let Some(catalog) = current(store, host, &version).await? {
                    return response(
                        runtime,
                        store,
                        host,
                        &catalog,
                        conditional,
                        CacheState::Wait,
                    )
                    .await;
                }
                if host.now_ms() >= deadline {
                    break;
                }
                host.wait(100).await?;
            }
            return fallback(
                runtime,
                store,
                host,
                "Codex model catalog refresh is already in progress",
            )
            .await;
        }
        let lease = Lease {
            version: &version,
            owner: &owner,
            generation: &generation,
        };
        let result = refresh(
            runtime,
            store,
            host,
            &lease,
            cached.as_ref(),
            conditional,
            now,
        )
        .await;
        // A failed release never deletes a successor's lease; publication and response are already settled.
        let released = catalog_store::release_refresh_lease(store, &version, &owner).await;
        if let Err(error) = released
            && result.is_err()
        {
            return Err(error);
        }
        result
    }
}
