//! Raw framed native response data and connection observation, never delivery/accounting proof.
use crate::error::unavailable;
use gateway_core::error::GatewayError;
use std::{
    io::Write,
    net::TcpStream,
    time::{SystemTime, UNIX_EPOCH},
};

pub fn disconnected(stream: &TcpStream) -> bool {
    if stream.set_nonblocking(true).is_err() {
        return true;
    }
    let closed = match stream.peek(&mut [0u8; 1]) {
        Ok(0) => true,
        Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => false,
        Err(_) => true,
        _ => false,
    };
    let reset = stream.set_nonblocking(false);
    closed || reset.is_err()
}
pub fn write_json(stream: &mut TcpStream, status: u16, body: &[u8]) -> Result<(), GatewayError> {
    let mut response = format!("HTTP/1.1 {status} Native\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",body.len()).into_bytes();
    response.extend_from_slice(body);
    // One framed write prevents an early refusal's body from being left behind
    // a separately flushed header when unread peer data makes close reset TCP.
    stream.write_all(&response).map_err(|_| unavailable())
}
pub fn write_error(stream: &mut TcpStream, error: GatewayError) -> Result<(), GatewayError> {
    let body = serde_json::to_vec(&error.envelope).map_err(|_| unavailable())?;
    write_json(stream, error.status, &body)
}
pub fn now_ms() -> Result<i64, GatewayError> {
    let value = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| unavailable())?
        .as_millis();
    i64::try_from(value).map_err(|_| unavailable())
}

pub fn write_import_response(
    stream: &mut TcpStream,
    status: u16,
    headers: std::collections::BTreeMap<String, String>,
    source_body: gateway_core::source_json::SourceJson,
) -> Result<(), GatewayError> {
    let body = serde_json::to_vec(&source_body).map_err(|_| unavailable())?;
    let mut response = format!("HTTP/1.1 {status} Native\r\n");
    let mut content_type = false;
    for (name, value) in headers {
        let name =
            ureq::http::HeaderName::from_bytes(name.as_bytes()).map_err(|_| unavailable())?;
        let value = ureq::http::HeaderValue::from_str(&value).map_err(|_| unavailable())?;
        if matches!(
            name.as_str(),
            "connection" | "content-length" | "transfer-encoding"
        ) {
            return Err(unavailable());
        }
        content_type |= name.as_str() == "content-type";
        response.push_str(name.as_str());
        response.push_str(": ");
        response.push_str(value.to_str().map_err(|_| unavailable())?);
        response.push_str("\r\n");
    }
    if !content_type {
        response.push_str("Content-Type: application/json\r\n");
    }
    response.push_str(&format!(
        "Cache-Control: no-store\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        body.len()
    ));
    let mut response = response.into_bytes();
    response.extend_from_slice(&body);
    stream.write_all(&response).map_err(|_| unavailable())
}
