//! Source catalog runtime over one strong authority and credential-owner publication.
use crate::{
    authority::RemoteAuthorityStore, catalog_host::Host, host::PlatformHost, storage::now_ms,
};
use gateway_catalog_runtime::{CatalogRuntime, admin::AdminCatalogResponse, host::CatalogHost};
use gateway_core::{error::GatewayError, model::ModelId, source_json::SourceJson};
use gateway_schema::{catalog::CatalogInput, source_request::error::SourceRequestError};
use std::rc::Rc;
use worker::{Context, Env};

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
) -> Result<gateway_auth::types::AuthResponse, GatewayError> {
    let host = Host {
        store: store.clone(),
        env: env.clone(),
        platform: platform.clone(),
        context,
    };
    Ok(into_auth_response(
        service().public_catalog(store, &host).await,
    ))
}

pub(crate) fn into_auth_response(
    response: Result<AdminCatalogResponse, SourceRequestError>,
) -> gateway_auth::types::AuthResponse {
    match response {
        Ok(response) => {
            let mut wire =
                gateway_auth::types::AuthResponse::source_json(response.status, response.body);
            wire.headers.extend(response.headers);
            wire
        }
        Err(error) => {
            gateway_auth::types::AuthResponse::source_json(error.status, error.body().clone())
        }
    }
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
    let host = Host {
        store: store.clone(),
        env: env.clone(),
        platform: platform.clone(),
        context,
    };
    let (service, version) = (service(), version.to_owned());
    host.background("Catalog background refresh", |host| async move {
        let response = service.versioned_models(&host.store, &host, &version, None);
        response.await.map(drop)
    });
}

#[derive(Clone)]
pub struct CatalogState {
    pub input: CatalogInput,
    pub runtime: Option<SourceJson>,
    pub(crate) service: CatalogRuntime,
    pub(crate) host: Host,
}

impl CatalogState {
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
            host.background("Catalog refresh", move |host| async move {
                service
                    .refresh_provider(&host.store, &host, provider, false)
                    .await
                    .map(drop)
            });
        }
        Ok(Self {
            input: state.input,
            runtime: state.runtime,
            service,
            host,
        })
    }

    pub async fn resolve_model(
        &mut self,
        model: &ModelId,
        responses: bool,
    ) -> Result<(), GatewayError> {
        if let Some(state) = self
            .service
            .resolve_request_model(&self.host.store, &self.host, &self.input, model, responses)
            .await?
        {
            (self.input, self.runtime) = (state.input, state.runtime);
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use gateway_core::js_string::JsString16;
    use std::collections::BTreeMap;

    #[test]
    fn public_catalog_rendering_retains_raw_models_and_source_error_fields() {
        let body = SourceJson::parse(r#"{"models":[{"id":"model-\ud800"}]}"#).unwrap();
        let response = into_auth_response(Ok(AdminCatalogResponse {
            status: 200,
            body: body.clone(),
            headers: BTreeMap::from([("x-fixture".into(), "source".into())]),
        }));
        assert_eq!(response.status, 200);
        assert_eq!(response.body.to_bytes().unwrap(), body.as_bytes());
        assert_eq!(response.headers["x-fixture"], "source");
        assert_eq!(response.headers["cache-control"], "no-store");
        let error = SourceRequestError::new(
            400,
            JsString16::from_units(vec![0xdc00]),
            "invalid_request_error",
            Some("invalid_request_error"),
            Some(Some(JsString16::from_units(vec![0xd800]))),
        );
        let expected = error.body().as_bytes().to_vec();
        let response = into_auth_response(Err(error));
        assert_eq!(response.status, 400);
        assert_eq!(response.body.to_bytes().unwrap(), expected);
        assert_eq!(response.headers["content-type"], "application/json");
    }
}
