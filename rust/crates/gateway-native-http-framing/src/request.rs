//! Closed raw native HTTP framing. This module never authenticates or mints a context.
use crate::error::{invalid, too_large};
use gateway_auth_native_wire::{
    control::NATIVE_CONTROL_MAX_BYTES,
    native_read::{NATIVE_CONTROL_PATH, NATIVE_MAX_BODY_BYTES},
};
use gateway_core::error::GatewayError;
use std::{
    io::Read,
    net::TcpStream,
    time::{Duration, Instant},
};

pub struct HttpRequest {
    pub method: String,
    pub path: String,
    pub headers: Vec<(String, String)>,
    pub body: Vec<u8>,
    pub buffered: Vec<u8>,
}
pub fn validate_live_upgrade(request: &HttpRequest) -> Result<(), GatewayError> {
    let one = |name: &str| -> Result<&str, GatewayError> {
        let values = request
            .headers
            .iter()
            .filter(|(key, _)| key.eq_ignore_ascii_case(name))
            .collect::<Vec<_>>();
        if values.len() != 1 || values[0].1.is_empty() {
            return Err(invalid());
        }
        Ok(&values[0].1)
    };
    if !one("upgrade")?.eq_ignore_ascii_case("websocket")
        || !one("connection")?
            .split(',')
            .any(|token| token.trim().eq_ignore_ascii_case("upgrade"))
    {
        return Err(invalid());
    }
    one("host")?;
    one("sec-websocket-key")?;
    one("sec-websocket-version")?;
    Ok(())
}
fn read_more(
    stream: &mut TcpStream,
    bytes: &mut Vec<u8>,
    deadline: Instant,
) -> Result<(), GatewayError> {
    let remaining = deadline
        .checked_duration_since(Instant::now())
        .filter(|value| !value.is_zero())
        .ok_or_else(invalid)?;
    stream
        .set_read_timeout(Some(remaining))
        .map_err(|_| invalid())?;
    let mut buffer = [0u8; 8192];
    let count = stream.read(&mut buffer).map_err(|_| invalid())?;
    if count == 0 {
        return Err(invalid());
    }
    bytes.extend_from_slice(&buffer[..count]);
    Ok(())
}
pub enum IncomingRequest {
    Legacy(HttpRequest),
    Import(crate::import_head::ImportHead),
}
pub fn read_import_head(
    stream: &mut TcpStream,
) -> Result<crate::import_head::ImportHead, GatewayError> {
    match read_request(stream)? {
        IncomingRequest::Import(head) => Ok(head),
        IncomingRequest::Legacy(_) => Err(invalid()),
    }
}
pub fn read_request(stream: &mut TcpStream) -> Result<IncomingRequest, GatewayError> {
    let deadline = Instant::now() + Duration::from_secs(5);
    let mut bytes = Vec::new();
    let mut first_line_complete = false;
    let mut import_header_only = false;
    let header_end = loop {
        if let Some(end) = bytes.windows(4).position(|part| part == b"\r\n\r\n") {
            if end + 4 > 16_384 {
                return Err(invalid());
            }
            break end + 4;
        }
        if bytes.len() > 16_384 {
            return Err(invalid());
        }
        if !first_line_complete || import_header_only {
            let remaining = deadline
                .checked_duration_since(Instant::now())
                .filter(|value| !value.is_zero())
                .ok_or_else(invalid)?;
            stream
                .set_read_timeout(Some(remaining))
                .map_err(|_| invalid())?;
            let mut byte = [0u8; 1];
            if stream.read(&mut byte).map_err(|_| invalid())? == 0 {
                return Err(invalid());
            }
            bytes.push(byte[0]);
            if !first_line_complete && bytes.ends_with(b"\r\n") {
                let mut slots = [httparse::EMPTY_HEADER; 64];
                let mut line = httparse::Request::new(&mut slots);
                line.parse(&bytes).map_err(|_| invalid())?;
                import_header_only = line.path.and_then(crate::import_head::kind).is_some();
                first_line_complete = true;
            }
        } else {
            read_more(stream, &mut bytes, deadline)?;
        }
    };
    let mut slots = [httparse::EMPTY_HEADER; 64];
    let mut parsed = httparse::Request::new(&mut slots);
    if !parsed
        .parse(&bytes[..header_end])
        .map_err(|_| invalid())?
        .is_complete()
    {
        return Err(invalid());
    }
    let method = parsed.method.ok_or_else(invalid)?.to_owned();
    let path = parsed.path.ok_or_else(invalid)?.to_owned();
    let headers = parsed
        .headers
        .iter()
        .map(|header| {
            Ok((
                header.name.to_owned(),
                std::str::from_utf8(header.value)
                    .map_err(|_| invalid())?
                    .trim()
                    .to_owned(),
            ))
        })
        .collect::<Result<Vec<_>, GatewayError>>()?;
    if let Some(kind) = crate::import_head::kind(&path) {
        if bytes.len() != header_end || method != "POST" || parsed.version != Some(1) {
            return Err(invalid());
        }
        return Ok(IncomingRequest::Import(crate::import_head::ImportHead {
            kind,
            path,
            headers,
            framing_deadline: deadline,
        }));
    }
    let lengths: Vec<_> = headers
        .iter()
        .filter(|(key, _)| key.eq_ignore_ascii_case("content-length"))
        .collect();
    let encodings: Vec<_> = headers
        .iter()
        .filter(|(key, _)| key.eq_ignore_ascii_case("transfer-encoding"))
        .collect();
    if lengths.len() > 1 || encodings.len() > 1 || !lengths.is_empty() && !encodings.is_empty() {
        return Err(invalid());
    }
    if method == "GET" {
        if parsed.version != Some(1)
            || !encodings.is_empty()
            || lengths.first().is_some_and(|header| header.1 != "0")
        {
            return Err(invalid());
        }
        return Ok(IncomingRequest::Legacy(HttpRequest {
            method,
            path,
            headers,
            body: Vec::new(),
            buffered: bytes[header_end..].to_vec(),
        }));
    }
    let max_body = if path == NATIVE_CONTROL_PATH {
        NATIVE_CONTROL_MAX_BYTES
    } else {
        NATIVE_MAX_BODY_BYTES
    };
    let body = if let Some(length) = lengths.first() {
        if length.1.is_empty() || !length.1.bytes().all(|byte| byte.is_ascii_digit()) {
            return Err(invalid());
        }
        let length = length.1.parse::<usize>().map_err(|_| invalid())?;
        if length > max_body {
            return Err(too_large());
        }
        while bytes.len() < header_end + length {
            read_more(stream, &mut bytes, deadline)?;
        }
        if bytes.len() != header_end + length {
            return Err(invalid());
        }
        bytes[header_end..].to_vec()
    } else if encodings
        .first()
        .is_some_and(|header| header.1.eq_ignore_ascii_case("chunked"))
    {
        let mut offset = header_end;
        let mut output = Vec::new();
        loop {
            let (header, length) = loop {
                match httparse::parse_chunk_size(&bytes[offset..]).map_err(|_| invalid())? {
                    httparse::Status::Complete(value) => break value,
                    httparse::Status::Partial => {
                        if bytes.len() - offset > 1024 {
                            return Err(invalid());
                        }
                        read_more(stream, &mut bytes, deadline)?;
                    }
                }
            };
            let length = usize::try_from(length).map_err(|_| too_large())?;
            if length > max_body - output.len() {
                return Err(too_large());
            }
            offset += header;
            while bytes.len() - offset < length + 2 {
                read_more(stream, &mut bytes, deadline)?;
            }
            if &bytes[offset + length..offset + length + 2] != b"\r\n" {
                return Err(invalid());
            }
            output.extend_from_slice(&bytes[offset..offset + length]);
            offset += length + 2;
            if length == 0 {
                if bytes.len() != offset {
                    return Err(invalid());
                }
                break;
            }
            if offset > 8192 {
                bytes.drain(..offset);
                offset = 0;
            }
        }
        output
    } else {
        return Err(invalid());
    };
    Ok(IncomingRequest::Legacy(HttpRequest {
        method,
        path,
        headers,
        body,
        buffered: Vec::new(),
    }))
}
