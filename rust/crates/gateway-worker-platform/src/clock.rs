//! Actual platform monotonic time; no caller clock or policy callback.
use gateway_core::error::GatewayError;
use std::time::Duration;
use worker::{
    js_sys::{Function, Reflect},
    wasm_bindgen::{JsCast, JsValue},
};

pub fn monotonic() -> Result<Duration, GatewayError> {
    let unavailable = || {
        GatewayError::unavailable(
            "The admission clock is unavailable.",
            "admission_clock_unavailable",
        )
    };
    let performance = Reflect::get(&worker::js_sys::global(), &JsValue::from_str("performance"))
        .map_err(|_| unavailable())?;
    let now = Reflect::get(&performance, &JsValue::from_str("now"))
        .map_err(|_| unavailable())?
        .dyn_into::<Function>()
        .map_err(|_| unavailable())?;
    let milliseconds = now
        .call0(&performance)
        .map_err(|_| unavailable())?
        .as_f64()
        .ok_or_else(unavailable)?;
    Duration::try_from_secs_f64(milliseconds / 1000.0).map_err(|_| unavailable())
}
