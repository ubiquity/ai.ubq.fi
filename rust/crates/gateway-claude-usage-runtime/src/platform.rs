//! Fixed native Worker usage transport. No caller-selected URL, method or inference capability.
use crate::fetch::{self, GetFailure, Response};
use gateway_admin_runtime::claude::AccountBinding;
use gateway_core::{error::GatewayError, js_string::JsString16};
use worker::{
    Fetch, Headers, Method, Request, RequestInit, RequestRedirect,
    js_sys::{Array, Function, Reflect, JSON},
    wasm_bindgen::{JsCast, JsValue},
};

fn setup<E>(_: E) -> GetFailure {
    GetFailure::Setup(GatewayError::new(500, "Internal server error", Some("server_error")).with_type("server_error"))
}
fn signal(caller: &worker::web_sys::AbortSignal) -> Result<worker::AbortSignal, GetFailure> {
    let constructor = Reflect::get(&worker::js_sys::global(), &JsValue::from_str("AbortSignal")).map_err(setup)?;
    let timeout = Reflect::get(&constructor, &JsValue::from_str("timeout")).map_err(setup)?.dyn_into::<Function>().map_err(setup)?;
    let timeout = timeout.call1(&constructor, &JsValue::from(fetch::TIMEOUT_MS)).map_err(setup)?;
    let signals = Array::new();
    signals.push(caller.as_ref());
    signals.push(&timeout);
    let any = Reflect::get(&constructor, &JsValue::from_str("any")).map_err(setup)?.dyn_into::<Function>().map_err(setup)?;
    let signal = any.call1(&constructor, &signals).map_err(setup)?;
    Ok(signal.dyn_into::<worker::web_sys::AbortSignal>().map_err(setup)?.into())
}
fn message(error: worker::Error) -> Option<JsString16> {
    let value = error.into_js_value();
    if !value.is_instance_of::<worker::js_sys::Error>() { return None; }
    let message = Reflect::get(&value, &JsValue::from_str("message")).ok()?;
    if !message.is_string() { return None; }
    gateway_core::source_json::SourceJson::parse(&JSON::stringify(&message).ok()?.as_string()?).ok()?.deserialize().ok()
}
/// Construction precedes the fetch failure handler. Platform text is read fully;
/// the classifier applies the source's 4096 UTF16 error slice afterwards.
pub async fn get(account: &AccountBinding, caller: &worker::web_sys::AbortSignal, clock: &impl Fn() -> i64) -> Result<Response, GetFailure> {
    let secret = account.credential.secret.try_scalar_secret().map_err(setup)?;
    let headers = Headers::new();
    for (name, value) in fetch::headers(secret.expose()) { headers.set(&name, &value).map_err(setup)?; }
    let signal = signal(caller)?;
    let mut init = RequestInit::new();
    init.with_method(Method::Get).with_headers(headers).with_redirect(RequestRedirect::Manual);
    let request = Request::new_with_init(fetch::URL, &init).map_err(setup)?;
    let mut response = Fetch::Request(request).send_with_signal(&signal).await.map_err(|e| GetFailure::Unreachable(message(e)))?;
    let observed_at_ms = clock();
    let status = response.status_code();
    let retry_after = response.headers().get("retry-after").ok().flatten();
    let body = response.text().await.map(|text| JsString16::from_utf8(&text)).map_err(|_| GatewayError::unavailable("Claude usage response text could not be read.", "claude_usage_invalid_response"));
    Ok(Response { status, body, retry_after, observed_at_ms })
}
/// Actual Date.parse receives the original UTF16 string, without scalar loss.
pub fn parse_date(text: &JsString16) -> Option<i64> {
    let text = JSON::parse(text.to_source_json().ok()?.as_str()).ok()?;
    let date = Reflect::get(&worker::js_sys::global(), &JsValue::from_str("Date")).ok()?;
    let parse = Reflect::get(&date, &JsValue::from_str("parse")).ok()?.dyn_into::<Function>().ok()?;
    let at = parse.call1(&date, &text).ok()?.as_f64()?;
    at.is_finite().then_some(at as i64)
}
