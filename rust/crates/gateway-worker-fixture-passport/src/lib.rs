//! Fixed test-only Passport HTTP/date host; no actor, proof or store API.
use gateway_auth_contracts::{
    types::AuthHost,
    upstream::{AuthUpstreamRequest, AuthUpstreamResponse},
    webauthn::{CosePublicKey, SignatureInput, WebAuthnCrypto},
};
use gateway_auth_passport_contracts::session::PassportDate;
use gateway_authority_contracts::{AuthorityClock, NativeRenewalCancellation, TokenSource};
use gateway_source::{error::GatewayError, js_string::JsString16};
use worker::{Env, Method, Request, RequestInit};
fn invalid() -> GatewayError {
    GatewayError::unavailable(
        "Controlled Rust fixture setup failed",
        "matrix_fixture_setup_failed",
    )
}
pub struct Host {
    env: Env,
}
impl Host {
    pub fn new(env: Env) -> Self {
        Self { env }
    }
}
impl WebAuthnCrypto for Host {
    async fn validate_key(&self, _: &CosePublicKey) -> Result<(), GatewayError> {
        Err(invalid())
    }
    async fn verify_signature(&self, _: SignatureInput) -> Result<bool, GatewayError> {
        Err(invalid())
    }
}
impl PassportDate for Host {
    fn parse_date_ms(&self, value: &JsString16) -> f64 {
        use worker::{
            js_sys::{Function, JsString, Reflect},
            wasm_bindgen::{JsCast, JsValue},
        };
        let result = (|| {
            let date = Reflect::get(&worker::js_sys::global(), &JsValue::from_str("Date")).ok()?;
            let parse = Reflect::get(&date, &JsValue::from_str("parse"))
                .ok()?
                .dyn_into::<Function>()
                .ok()?;
            parse
                .call1(&date, &JsString::from_char_code(value.as_units()))
                .ok()?
                .as_f64()
        })();
        result.unwrap_or(f64::NAN)
    }
}
impl AuthHost for Host {
    fn now_ms(&self) -> i64 {
        gateway_worker_storage::store::now_ms().unwrap_or(i64::MAX)
    }
    fn random_bytes(&self, n: usize) -> Result<Vec<u8>, GatewayError> {
        gateway_worker_platform::random::random_bytes(n)
    }
    async fn kernel_principal(
        &self,
        _: &str,
        _: &str,
        _: &str,
        _: &str,
    ) -> Result<String, GatewayError> {
        Err(invalid())
    }
    async fn fetch_auth(
        &self,
        input: AuthUpstreamRequest,
    ) -> Result<AuthUpstreamResponse, GatewayError> {
        if !matches!(input, AuthUpstreamRequest::PassportSession { .. }) {
            return Err(invalid());
        }
        let body = input.body()?;
        let mut init = RequestInit::new();
        init.with_method(Method::Post);
        init.with_body(Some(worker::wasm_bindgen::JsValue::from_str(
            std::str::from_utf8(&body).map_err(|_| invalid())?,
        )));
        let mut request = Request::new_with_init(&input.url(), &init).map_err(|_| invalid())?;
        for (k, v) in input.headers() {
            request.headers_mut().set(&k, &v).map_err(|_| invalid())?;
        }
        let mut response = self
            .env
            .service("PASSPORT_FIXTURE")
            .map_err(|_| invalid())?
            .fetch_request(request)
            .await
            .map_err(|_| invalid())?;
        Ok(AuthUpstreamResponse {
            status: response.status_code(),
            body: response.bytes().await.map_err(|_| invalid())?,
        })
    }
}

pub struct Clock;
impl AuthorityClock for Clock {
    fn now_ms(&self) -> Result<i64, GatewayError> {
        gateway_worker_storage::store::now_ms()
    }
}
pub struct Tokens;
impl TokenSource for Tokens {
    fn next_token(&self) -> Result<String, GatewayError> {
        gateway_worker_platform::random::opaque_token()
    }
}
pub struct Cancel;
impl NativeRenewalCancellation for Cancel {
    fn cancelled(&self) -> bool {
        false
    }
}
