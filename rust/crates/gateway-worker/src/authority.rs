//! Trusted SDK adapter; private authority logic has one separate defining crate.
use gateway_authority_lifecycle::model::Identity;
use gateway_core::error::GatewayError;
use gateway_worker_authority::wire::{ProfileMigration, StoreResponse};
use gateway_worker_authority_host::authority::{Authority, Host};
use gateway_worker_storage::sqlite::SqliteAuthorityStore;
use worker::{DurableObject, Env, Request, Response, State, durable_object};
struct Bridge;
impl Host for Bridge {
    async fn request_bytes(request: &mut Request, max: usize) -> Result<Vec<u8>, GatewayError> {
        crate::transport::body::request_bytes(request, max).await
    }
    async fn profile_migration(
        store: &SqliteAuthorityStore,
        request: ProfileMigration,
        request_id: String,
        now_ms: i64,
    ) -> Result<StoreResponse, GatewayError> {
        let result = crate::migration_host::execute_profile(
            store,
            request.into(),
            gateway_admin_runtime::capabilities::Context { request_id, now_ms },
        )
        .await?;
        if result.stream.is_some() {
            return Err(GatewayError::unavailable(
                "The shared coordination authority is unavailable.",
                "authority_wire_invalid",
            ));
        }
        Ok(StoreResponse::ProfileMigration {
            status: result.status,
            headers: result.headers,
            body: gateway_core::source_json::SourceJson::parse_bytes(&result.body.to_bytes()?)?,
        })
    }
}
#[durable_object]
pub struct GatewayAuthority {
    inner: Authority<Bridge>,
}
impl DurableObject for GatewayAuthority {
    fn new(state: State, env: Env) -> Self {
        let identity = Identity {
            object_id: state.id().to_string(),
            source_sha: env!("UOS_RUST_BUILD_GIT_SHA").into(),
            source_tree: env!("UOS_RUST_BUILD_GIT_TREE").into(),
        };
        Self {
            inner: Authority::new(
                state,
                identity,
                !crate::host::auth_config(&env).auth_tokens.is_empty(),
            ),
        }
    }
    async fn fetch(&self, request: Request) -> worker::Result<Response> {
        self.inner.fetch(request).await
    }
}
