//! Bounded migration bindings; full native capture is private, never a public admin body.
use gateway_authority_lifecycle::{
    model::{
        Access, Acquire, Authorization, Blocker, ControlEnvelope, ControlRequest, ImportState,
        LeaseKind, Operation, Phase, Reply, Status,
    },
    policy,
};
use gateway_core::{error::GatewayError, query::QueryStore};
use gateway_migration::{
    error::MigrationError,
    operational::{NativeMigrationRequest, NativeMigrationResponse},
};

pub async fn execute_profile<S: QueryStore>(
    store: &S,
    request: gateway_admin_runtime::capabilities::MigrationRequest,
    context: gateway_admin_runtime::capabilities::Context,
) -> Result<gateway_auth::types::AuthResponse, GatewayError> {
    gateway_migration::profile_runtime::execute_profile(store, &ProfileHost, request, context).await
}

struct ProfileHost;
impl gateway_admin_data::profile::SourceDateHost for ProfileHost {
    fn date_value(&self, input: Option<&serde_json::Value>) -> Result<Option<i64>, GatewayError> {
        let raw = input
            .map(gateway_core::source_json::SourceJson::from_serializable)
            .transpose()?;
        self.source_date_value(raw.as_ref())
    }
    fn source_date_value(
        &self,
        input: Option<&gateway_core::source_json::SourceJson>,
    ) -> Result<Option<i64>, GatewayError> {
        use worker::wasm_bindgen::JsCast;
        let value = input
            .map(|v| worker::js_sys::JSON::parse(v.as_str()))
            .transpose()
            .map_err(|_| {
                GatewayError::unavailable(
                    "Source Date input is invalid",
                    "source_date_host_invalid",
                )
            })?
            .unwrap_or(worker::wasm_bindgen::JsValue::UNDEFINED);
        // A throwing source Date coercion is an error for this line, not the request.
        let invalid =
            || GatewayError::unavailable("Source Date coercion failed", "source_date_host_invalid");
        let constructor = worker::js_sys::Reflect::get(
            &worker::js_sys::global(),
            &worker::wasm_bindgen::JsValue::from_str("Date"),
        )
        .map_err(|_| invalid())?
        .dyn_into::<worker::js_sys::Function>()
        .map_err(|_| invalid())?;
        let arguments = worker::js_sys::Array::new();
        arguments.push(&value);
        let date = worker::js_sys::Reflect::construct(&constructor, &arguments)
            .map_err(|_| invalid())?
            .dyn_into::<worker::js_sys::Date>()
            .map_err(|_| invalid())?;
        let time = date.get_time();
        Ok(time.is_finite().then_some(time as i64))
    }
}
impl gateway_migration::profile_runtime::ProfileHost for ProfileHost {
    fn next_revision(&self) -> Result<String, GatewayError> {
        crate::transport::random::opaque_token()
    }
}

pub async fn execute_native<S: QueryStore>(
    store: &S,
    request: NativeMigrationRequest,
) -> Result<NativeMigrationResponse, MigrationError> {
    gateway_migration::operational::execute_native(store, request).await
}

/// Authenticated operator state transfer (DECISIONS 2026-11-05). Only the
/// configured admin token reaches it, and only while the one-time operator
/// intent digest is configured; every write is one existing staged frame.
pub async fn state_transfer(
    request: &mut worker::Request,
    auth_request: &gateway_auth::types::AuthRequest,
    auth: &gateway_auth::resolve::AuthService<
        '_,
        gateway_worker_authority::client::RemoteAuthorityStore,
        crate::host::PlatformHost,
    >,
    env: &worker::Env,
    operation: gateway_state_transfer::http::Operation,
) -> Result<worker::Response, GatewayError> {
    use gateway_authority_provisioning::intent;
    use gateway_state_transfer::http;
    let principal = auth.authenticate_admin(auth_request).await?;
    if !matches!(
        principal.method,
        gateway_auth::types::AuthMethod::AdminAllowlist
    ) {
        return Err(GatewayError::new(403, "Forbidden", Some("forbidden")));
    }
    let body = match operation {
        http::Operation::ImportStatus => Vec::new(),
        _ => crate::transport::body::request_bytes(request, http::REQUEST_MAX_BYTES).await?,
    };
    let presented = request.headers().get(intent::INTENT_HEADER).ok().flatten();
    let configured = crate::host::setting(env, intent::INTENT_SETTING);
    let intent = intent::Intent::verify(presented.as_deref(), configured.as_deref())?;
    if operation == http::Operation::Control {
        return control_response(auth.store, &intent, &body).await;
    }
    let reply = if operation == http::Operation::ImportFrame {
        import_reply(
            auth.store,
            &intent,
            configured.as_deref(),
            presented.as_deref(),
            &body,
        )
        .await?
    } else {
        let scoped = if operation == http::Operation::ExportPage {
            Some(export_store(auth.store, &body).await?)
        } else {
            None
        };
        http::handle(
            scoped.as_ref().unwrap_or(auth.store),
            operation,
            configured.as_deref(),
            presented.as_deref(),
            &body,
        )
        .await?
    };
    let failed =
        || GatewayError::unavailable("State-transfer reply failed", "state_transfer_reply_failed");
    let mut response = worker::Response::from_bytes(reply.body)
        .map_err(|_| failed())?
        .with_status(reply.status);
    for (name, value) in reply.headers {
        response
            .headers_mut()
            .set(name, &value)
            .map_err(|_| failed())?;
    }
    Ok(response)
}

async fn authority_status(
    store: &gateway_worker_authority::client::RemoteAuthorityStore,
) -> Result<Status, GatewayError> {
    match store.coordination(Operation::Status).await? {
        Reply::Status { status } => Ok(status),
        _ => Err(policy::refused("Authority status is invalid")),
    }
}
async fn import_store(
    store: &gateway_worker_authority::client::RemoteAuthorityStore,
    intent: &gateway_authority_provisioning::intent::Intent,
    body: &[u8],
    presented: &str,
) -> Result<gateway_worker_authority::client::RemoteAuthorityStore, GatewayError> {
    let frame = gateway_migration::staged::decode_stage_request(body)?;
    let capture = intent.import_capture()?;
    let value =
        serde_json::to_value(&frame).map_err(|_| policy::refused("Import frame is invalid"))?;
    let status = authority_status(store).await?;
    let import = if value.get("stage").and_then(serde_json::Value::as_str) == Some("begin") {
        let data = value
            .get("data")
            .ok_or_else(|| policy::refused("Import header missing"))?;
        if data
            .pointer("/manifest/capture_id")
            .and_then(serde_json::Value::as_str)
            != Some(capture)
        {
            return Err(policy::refused("Import capture differs"));
        }
        ImportState {
            capture_id: capture.into(),
            intent_digest: intent.configured_digest().into(),
            header_digest: gateway_auth::crypto::sha256_hex(
                &serde_json::to_vec(data).map_err(|_| policy::refused("Import header invalid"))?,
            ),
            records: data
                .get("records")
                .and_then(serde_json::Value::as_u64)
                .ok_or_else(|| policy::refused("Import count absent"))?,
            chain_digest: data
                .get("digest")
                .and_then(serde_json::Value::as_str)
                .ok_or_else(|| policy::refused("Import chain absent"))?
                .into(),
        }
    } else {
        status
            .gate
            .import
            .ok_or_else(|| policy::refused("No exclusive import exists"))?
    };
    if import.capture_id != capture || import.intent_digest != intent.configured_digest() {
        return Err(policy::refused("Import owner differs"));
    }
    let frame_digest = gateway_auth::crypto::sha256_hex(body);
    let capability = gateway_auth::crypto::sha256_hex(
        format!("uos-import-frame-cap-v1\0{presented}\0{frame_digest}").as_bytes(),
    );
    let request = Acquire {
        id: gateway_auth::crypto::sha256_hex(
            format!(
                "uos-import-frame-v1\0{}\0{frame_digest}",
                intent.configured_digest()
            )
            .as_bytes(),
        ),
        capability_sha256: gateway_auth::crypto::sha256_hex(capability.as_bytes()),
        kind: LeaseKind::ImportFrame(import),
        parent: None,
    };
    store.acquire(request, capability).await
}
async fn export_store(
    store: &gateway_worker_authority::client::RemoteAuthorityStore,
    body: &[u8],
) -> Result<gateway_worker_authority::client::RemoteAuthorityStore, GatewayError> {
    let request: gateway_state_transfer::export::ExportRequest = serde_json::from_slice(body)
        .map_err(|_| policy::refused("Export requires its frozen epoch"))?;
    let status = authority_status(store).await?;
    let reference = status
        .gate
        .frozen
        .filter(|value| {
            status.gate.phase == Phase::Frozen
                && value.freeze_id == request.freeze_id
                && value.freeze_revision == request.freeze_revision
        })
        .ok_or_else(|| policy::refused("Frozen export identity differs"))?;
    Ok(store.scoped(Access::Frozen(reference)))
}
async fn control_response(
    store: &gateway_worker_authority::client::RemoteAuthorityStore,
    intent: &gateway_authority_provisioning::intent::Intent,
    body: &[u8],
) -> Result<worker::Response, GatewayError> {
    if body.len() > 65_536 {
        return Err(policy::refused("Control body exceeds its bound"));
    }
    let envelope: ControlEnvelope =
        serde_json::from_slice(body).map_err(|_| policy::refused("Control request is invalid"))?;
    if envelope.v != 2 {
        return Err(policy::refused("Control version unsupported"));
    }
    let operation = match envelope.request {
        ControlRequest::Status => {
            if !matches!(
                intent.scope(),
                gateway_authority_provisioning::intent::Scope::Import { .. }
                    | gateway_authority_provisioning::intent::Scope::Export
            ) {
                return Err(policy::refused("Status requires an operator read scope"));
            }
            Operation::Status
        }
        ControlRequest::FinalizeImport { plan } => {
            let authorization = Authorization::FinalizeImport(plan);
            let digest = intent.require_control(&authorization)?.into();
            Operation::Control {
                authorization,
                intent_digest: digest,
            }
        }
        ControlRequest::Drain { plan } => {
            let authorization = Authorization::Drain(plan);
            let digest = intent.require_control(&authorization)?.into();
            Operation::Control {
                authorization,
                intent_digest: digest,
            }
        }
        ControlRequest::CancelDrain { plan } => {
            let authorization = Authorization::CancelDrain(plan);
            let digest = intent.require_control(&authorization)?.into();
            Operation::Control {
                authorization,
                intent_digest: digest,
            }
        }
        ControlRequest::Activate { reference } => {
            let authorization = Authorization::Activate(reference);
            let digest = intent.require_control(&authorization)?.into();
            Operation::Control {
                authorization,
                intent_digest: digest,
            }
        }
        ControlRequest::AwaitFrozen { plan } => {
            let authorization = Authorization::Drain(plan.clone());
            Operation::AwaitFrozen {
                reference_id: intent.require_control(&authorization)?.into(),
                plan,
            }
        }
    };
    let reply = store.coordination(operation).await?;
    if let gateway_authority_provisioning::intent::Scope::Import { capture_id } = intent.scope()
        && let Reply::Status { status } = &reply
        && status
            .gate
            .import
            .as_ref()
            .is_some_and(|value| &value.capture_id != capture_id)
    {
        return Err(policy::refused("Status names another import capture"));
    }
    let status = if matches!(reply, Reply::DrainTimeout { .. }) {
        504
    } else if matches!(
        reply,
        Reply::DrainCancelled { .. } | Reply::DrainStale { .. }
    ) {
        409
    } else {
        200
    };
    let mut response = worker::Response::from_json(&reply)
        .map_err(|_| policy::refused("Control reply cannot be encoded"))?
        .with_status(status);
    response
        .headers_mut()
        .set("Cache-Control", "no-store")
        .map_err(|_| policy::refused("Control reply headers failed"))?;
    Ok(response)
}

async fn import_reply(
    store: &gateway_worker_authority::client::RemoteAuthorityStore,
    intent: &gateway_authority_provisioning::intent::Intent,
    configured: Option<&str>,
    presented: Option<&str>,
    body: &[u8],
) -> Result<gateway_state_transfer::http::Reply, GatewayError> {
    use gateway_state_transfer::http::{Operation::ImportFrame, handle};
    let presented = presented.ok_or_else(|| policy::refused("Import intent missing"))?;
    let status = authority_status(store).await?;
    if status.gate.phase == Phase::Frozen || status.counts.import_frames != 0 {
        // Read-only replay proves that THIS frame committed; no lease is stolen by age.
        let proof = handle(
            &store.operator_read(),
            ImportFrame,
            configured,
            Some(presented),
            body,
        )
        .await?;
        if status.gate.phase != Phase::Frozen {
            let original = import_store(store, intent, body, presented).await?;
            original.finish_scope(None).await?;
        }
        return Ok(proof);
    }
    let scoped = import_store(store, intent, body, presented).await?;
    match handle(&scoped, ImportFrame, configured, Some(presented), body).await {
        Ok(reply) => {
            scoped.finish_scope(None).await?;
            Ok(reply)
        }
        Err(error) => {
            let uncertain = error
                .envelope
                .error
                .code
                .as_deref()
                .is_some_and(|code| code.starts_with("authority_"));
            if uncertain
                && let Ok(proof) = handle(
                    &store.operator_read(),
                    ImportFrame,
                    configured,
                    Some(presented),
                    body,
                )
                .await
            {
                scoped.finish_scope(None).await?;
                return Ok(proof);
            }
            scoped
                .finish_scope(uncertain.then_some(Blocker::UncertainCommit))
                .await?;
            Err(error)
        }
    }
}
