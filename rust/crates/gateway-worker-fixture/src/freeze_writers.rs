//! Explicit test-only injection of the real Rust Writer destructor/complete path.
//! Durable Finish is owned exclusively by unmodified WriterGroup::retain.
use gateway_auth::crypto::sha256_hex;
use gateway_authority_provisioning::model::{Acquire, LeaseKind};
use gateway_core::error::GatewayError;
use gateway_worker_authority::client::RemoteAuthorityStore;
use worker::{Context, Env, Method, Request, Response};

fn invalid() -> GatewayError {
    GatewayError::unavailable(
        "Writer fixture is unavailable",
        "freeze_writer_fixture_refused",
    )
}

pub(super) async fn fetch(
    request: Request,
    env: Env,
    context: Context,
) -> worker::Result<gateway_worker::response::GatewayResponse> {
    let result = async {
        let mode = match (request.method(), request.path().as_str()) {
            (Method::Post, "/drop") => "drop",
            (Method::Post, "/complete") => "complete",
            _ => return Err(invalid()),
        };
        let capability = sha256_hex(gateway_worker::transport::random::opaque_token()?.as_bytes());
        let store = RemoteAuthorityStore::from_env(&env)?
            .operator_read()
            .acquire(
                Acquire {
                    id: sha256_hex(format!("freeze-writer-{mode}-{capability}").as_bytes()),
                    capability_sha256: sha256_hex(capability.as_bytes()),
                    kind: LeaseKind::Root {
                        request_id: format!("freeze-writer-{mode}"),
                    },
                    parent: None,
                },
                capability,
            )
            .await?;
        let permit = store.permit()?;
        let (store, root) = store.track_root(&context)?;
        let child = store.writer()?.ok_or_else(invalid)?;
        if mode == "drop" {
            drop(child);
        } else {
            child.complete();
        }
        root.complete();
        Ok(serde_json::json!({
            "operation":"actual_rust_writer", "mode":mode,
            "writer_id":permit.id, "serving_epoch":permit.serving_epoch,
            "capability_sha256":sha256_hex(permit.capability.as_bytes())
        }))
    }
    .await;
    let value = result
        .map_err(|error: GatewayError| worker::Error::RustError(error.envelope.error.message))?;
    Response::from_json(&value).map(Into::into)
}
