//! Actual JavaScript label ordering for existing observational fingerprints.
use gateway_source::error::GatewayError;
use std::cmp::Ordering;
use worker::{
    js_sys::{Function, Reflect},
    wasm_bindgen::{JsCast, JsValue},
};

pub fn compare_labels(left: &str, right: &str) -> Result<Ordering, GatewayError> {
    let unavailable = || {
        GatewayError::unavailable(
            "Administrative label collation is unavailable.",
            "admin_label_collation_unavailable",
        )
    };
    let string = Reflect::get(&worker::js_sys::global(), &JsValue::from_str("String"))
        .map_err(|_| unavailable())?;
    let prototype =
        Reflect::get(&string, &JsValue::from_str("prototype")).map_err(|_| unavailable())?;
    let compare = Reflect::get(&prototype, &JsValue::from_str("localeCompare"))
        .map_err(|_| unavailable())?
        .dyn_into::<Function>()
        .map_err(|_| unavailable())?;
    compare
        .call1(&JsValue::from_str(left), &JsValue::from_str(right))
        .map_err(|_| unavailable())?
        .as_f64()
        .and_then(|value| value.partial_cmp(&0.0))
        .ok_or_else(unavailable)
}
