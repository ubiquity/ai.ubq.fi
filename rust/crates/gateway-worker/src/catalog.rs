//! Source catalog runtime over one strong authority and credential-owner publication.
use crate::{catalog_host::Host, host::PlatformHost};
use gateway_catalog_admin::admin::CatalogAdmin;
use gateway_catalog_versioned::VersionedCatalog;
use gateway_schema::{catalog::CatalogInput, catalog_cache::CatalogResponse};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use gateway_worker_authority_binding::authority::RemoteAuthorityStore;
use gateway_worker_storage::store::now_ms;
use std::rc::Rc;
use worker::{Context, Env, Request};
use {gateway_catalog_provider::host::CatalogHost, gateway_catalog_runtime::CatalogRuntime};

pub const DIRECT_PROVIDERS: &[(&str, &str)] = &[
    ("deepseek", "DEEPSEEK_API_KEY"),
    ("cerebras", "CEREBRAS_API_KEY"),
    ("lithos", "LITHOSAI_API_KEY"),
    ("openrouter", "OPENROUTER_API_KEY"),
    ("surplus", "SURPLUS_API_KEY"),
    ("openlux", "OPENLUX_API_KEY"),
];

thread_local! {
    static RUNTIME: CatalogRuntime = CatalogRuntime::new();
}

pub fn service() -> CatalogRuntime {
    RUNTIME.with(Clone::clone)
}

pub async fn public_response(
    store: &RemoteAuthorityStore,
    env: &Env,
    platform: &PlatformHost,
    context: Rc<Context>,
) -> Result<gateway_auth_contracts::types::AuthResponse, GatewayError> {
    let host = Host {
        store: store.clone(),
        env: env.clone(),
        platform: platform.clone(),
        context,
    };
    Ok(gateway_catalog_admin::response::into_auth_response(
        service().public_catalog(store, &host).await,
    ))
}

/// The existing versioned refresh path, scheduled after a stale response on a
/// background cancellation. It creates no cron, alarm, or detached timer.
pub fn schedule_warm(
    store: &RemoteAuthorityStore,
    env: &Env,
    platform: &PlatformHost,
    context: Rc<Context>,
    version: &str,
) {
    let mut host = Host {
        store: store.clone(),
        env: env.clone(),
        platform: platform.clone(),
        context: context.clone(),
    };
    let service = service();
    let store = store.clone();
    let version = version.to_owned();
    context.wait_until(async move {
        let cancellation = match gateway_worker_platform::cancellation::Cancellation::background() {
            Ok(cancellation) => cancellation,
            Err(error) => {
                worker::console_error!(
                    "Catalog background refresh remains unavailable: {}",
                    error.envelope.error.message
                );
                return;
            }
        };
        host.platform.cancellation = Rc::new(cancellation);
        if let Err(error) = service
            .versioned_models(&store, &host, &version, None)
            .await
        {
            worker::console_error!(
                "Catalog background refresh remains pending: {}",
                error.envelope.error.message
            );
        }
    });
}

#[derive(Clone)]
pub struct CatalogState {
    pub input: CatalogInput,
    pub runtime: Option<SourceJson>,
    service: CatalogRuntime,
    host: Host,
}

impl CatalogState {
    pub async fn resolve_inference(
        mut self,
        document: &gateway_request_normalization::document::SourceRequestDocument,
    ) -> Result<Self, GatewayError> {
        self.service
            .resolve_inference_catalog(&self.host.store, &self.host, &mut self.input, document)
            .await?;
        Ok(self)
    }

    pub async fn load(
        store: &RemoteAuthorityStore,
        env: &Env,
        platform: &PlatformHost,
        context: Rc<Context>,
    ) -> Result<Self, GatewayError> {
        let host = Host {
            store: store.clone(),
            env: env.clone(),
            platform: platform.clone(),
            context: context.clone(),
        };
        let publication = host.publication().await?;
        let service = service();
        let state = service.load_state(store, &publication, now_ms()?).await?;
        for provider in state.refresh_due {
            let service = service.clone();
            let host = host.clone();
            let store = store.clone();
            context.wait_until(async move {
                if let Err(error) = service
                    .refresh_provider(&store, &host, provider, false)
                    .await
                {
                    worker::console_error!(
                        "Catalog refresh remains unavailable: {}",
                        error.envelope.error.message
                    );
                }
            });
        }
        Ok(Self {
            input: state.input,
            runtime: state.runtime,
            service,
            host,
        })
    }

    pub async fn decision_models(
        &self,
        store: &RemoteAuthorityStore,
    ) -> Result<Vec<gateway_contracts::model::ModelId>, GatewayError> {
        let publication = self.host.publication().await?;
        let state = self
            .service
            .load_state(store, &publication, now_ms()?)
            .await?;
        if state.input.configured("openrouter")
            && state
                .input
                .providers
                .get("openrouter")
                .is_none_or(|row| row.updated_at_ms.is_none())
        {
            self.service
                .refresh_provider(
                    store,
                    &self.host,
                    gateway_catalog_provider::host::CatalogProvider::Openrouter,
                    false,
                )
                .await?;
        }
        let publication = self.host.publication().await?;
        let snapshot = self.service.cached_provider(
            &publication,
            gateway_catalog_provider::host::CatalogProvider::Openrouter,
        );
        Ok(snapshot
            .into_iter()
            .flat_map(|snapshot| snapshot.models)
            .filter(gateway_decision_protocol::catalog::decision_model)
            .filter_map(|row| {
                gateway_source::source_serialization::member(&row, "id")
                    .as_ref()
                    .and_then(gateway_source::source_serialization::string)
            })
            .map(gateway_contracts::model::ModelId::from_source)
            .collect())
    }
    pub async fn native_models(
        &self,
        store: &RemoteAuthorityStore,
        request: &Request,
        version: &str,
        _now: i64,
    ) -> Result<CatalogResponse, GatewayError> {
        let conditional = request.headers().get("if-none-match").map_err(|_| {
            GatewayError::new(
                400,
                "Invalid conditional catalog header.",
                Some("invalid_request_error"),
            )
        })?;
        self.service
            .versioned_models(store, &self.host, version, conditional.as_deref())
            .await
    }
}
