use crate::source as s;
use gateway_core::error::GatewayError;
use gateway_core::js_string::JsString16;
use gateway_core::source_json::SourceJson;
use gateway_local_refusal::source_error::SourceRequestError;
use serde_json::Value as ScalarValue;

pub fn js_whitespace(c: char) -> bool {
    matches!(c, '\u{0009}'..='\u{000d}' | ' ' | '\u{00a0}' | '\u{1680}'
        | '\u{2000}'..='\u{200a}' | '\u{2028}' | '\u{2029}' | '\u{202f}'
        | '\u{205f}' | '\u{3000}' | '\u{feff}')
}



pub fn key_is(key: &JsString16, name: &str) -> bool {
    key.as_units().iter().copied().eq(name.encode_utf16())
}


pub fn scalar_error(status: u16, message: impl Into<String>, code: Option<&str>) -> SourceRequestError {
    SourceRequestError::from_gateway(GatewayError::new(status, message, code))
}


pub fn invalid(message: impl Into<String>, param: &str) -> SourceRequestError {
    SourceRequestError::from_gateway(
        GatewayError::new(400, message, Some("invalid_request_error")).with_param(Some(param)),
    )
}


pub fn keyed_error(prefix: &str, key: &JsString16, suffix: &str, param: bool) -> SourceRequestError {
    let message = s::concat(&[
        &JsString16::from_utf8(prefix),
        key,
        &JsString16::from_utf8(suffix),
    ]);
    SourceRequestError::new(
        400,
        message,
        "invalid_request_error",
        Some("invalid_request_error"),
        param.then(|| Some(key.clone())),
    )
}


pub fn lift(raw: &ScalarValue) -> Result<SourceJson, GatewayError> {
    s::scalar_json(raw)
}




pub fn base64_encode(bytes: &[u8]) -> String {
    const CHARS: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut result = String::new();
    for chunk in bytes.chunks(3) {
        let a = chunk[0] as usize;
        let b = chunk.get(1).copied().unwrap_or(0) as usize;
        let c = chunk.get(2).copied().unwrap_or(0) as usize;
        result.push(CHARS[a >> 2] as char);
        result.push(CHARS[((a & 3) << 4) | (b >> 4)] as char);
        result.push(if chunk.len() > 1 {
            CHARS[((b & 15) << 2) | (c >> 6)] as char
        } else {
            '='
        });
        result.push(if chunk.len() > 2 {
            CHARS[c & 63] as char
        } else {
            '='
        });
    }
    result
}




pub fn malformed(message: impl Into<String>) -> GatewayError {
    GatewayError::new(502, message, Some("upstream_stream_error")).with_type("server_error")
}
