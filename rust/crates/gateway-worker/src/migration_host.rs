//! Bounded migration bindings; full native capture is private, never a public admin body.
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
        crate::authority::RemoteAuthorityStore,
        crate::host::PlatformHost,
    >,
    env: &worker::Env,
    operation: gateway_state_transfer::http::Operation,
) -> Result<worker::Response, GatewayError> {
    use gateway_state_transfer::{http, intent};
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
    let reply = http::handle(
        auth.store,
        operation,
        configured.as_deref(),
        presented.as_deref(),
        &body,
    )
    .await?;
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
