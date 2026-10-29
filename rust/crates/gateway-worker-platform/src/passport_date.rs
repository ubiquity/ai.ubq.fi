use gateway_source::js_string::JsString16;
use worker::{
    js_sys::{Function, JsString, Reflect},
    wasm_bindgen::{JsCast, JsValue},
};

pub fn parse_date_ms(value: &JsString16) -> f64 {
    // Date::parse(&str) narrows the source domain before the actual JS
    // primitive. Pass a JS UTF16 string directly to Date.parse instead.
    let parsed = (|| {
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
    parsed.unwrap_or(f64::NAN)
}
