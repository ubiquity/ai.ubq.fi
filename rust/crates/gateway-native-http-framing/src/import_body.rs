//! Original bounded import framing and zeroizing raw DATA; no authentication or custody.
use crate::{error::invalid, import_head::ImportHead};
use gateway_auth_native_proof::native_context::control_interrupted;
use gateway_authority_contracts::NativeRenewalCancellation;
use gateway_source::error::GatewayError;
use std::{io::Read, net::TcpStream, time::Instant};

/// Unauthenticated raw request bytes only; no proof or source authority.
/// The original request scope retains this value through its last consumer.
pub struct RawImportDocument(Vec<u8>);
impl RawImportDocument {
    pub fn as_bytes(&self) -> &[u8] {
        &self.0
    }
}
impl Drop for RawImportDocument {
    fn drop(&mut self) {
        self.0.fill(0);
    }
}
pub fn read_import_body<C: NativeRenewalCancellation + ?Sized>(
    stream: &mut TcpStream,
    head: &ImportHead,
    cancellation: &C,
) -> Result<RawImportDocument, GatewayError> {
    let length = optional(head, "content-length")?;
    let encoding = optional(head, "transfer-encoding")?;
    if length.is_some() && encoding.is_some() {
        return Err(invalid());
    }
    let maximum = head.kind.body_limit_bytes();
    let mut body = RawImportDocument(Vec::new());
    if let Some(length) = length {
        gateway_native_import_source::body::check_auth_content_length(Some(length))
            .map_err(|_| auth_body_error())?;
        let length = length.parse::<usize>().map_err(|_| auth_body_error())?;
        append(
            stream,
            &mut body.0,
            length,
            head.framing_deadline,
            cancellation,
        )?;
    } else if encoding.is_some_and(|encoding| encoding.eq_ignore_ascii_case("chunked")) {
        loop {
            let mut line = Vec::new();
            while !line.ends_with(b"\r\n") {
                if line.len() >= 1024 {
                    return Err(invalid());
                }
                append(stream, &mut line, 1, head.framing_deadline, cancellation)?;
            }
            let size = match httparse::parse_chunk_size(&line).map_err(|_| invalid())? {
                httparse::Status::Complete((used, size)) if used == line.len() => {
                    usize::try_from(size).map_err(|_| auth_body_error())?
                }
                _ => return Err(invalid()),
            };
            if size > maximum - body.0.len() {
                return Err(auth_body_error());
            }
            append(
                stream,
                &mut body.0,
                size,
                head.framing_deadline,
                cancellation,
            )?;
            let mut end = Vec::new();
            append(stream, &mut end, 2, head.framing_deadline, cancellation)?;
            if end != b"\r\n" {
                return Err(invalid());
            }
            if size == 0 {
                break;
            }
        }
    } else {
        return Err(invalid());
    }
    check_cancelled(cancellation)?;
    // Preserve the closed native framing contract: no buffered trailers or second request.
    stream.set_nonblocking(true).map_err(|_| invalid())?;
    let extra = match stream.peek(&mut [0u8; 1]) {
        Ok(count) => count > 0,
        Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => false,
        Err(_) => true,
    };
    stream.set_nonblocking(false).map_err(|_| invalid())?;
    if extra {
        return Err(invalid());
    }
    Ok(body)
}
fn append<C: NativeRenewalCancellation + ?Sized>(
    stream: &mut TcpStream,
    output: &mut Vec<u8>,
    length: usize,
    deadline: Instant,
    cancellation: &C,
) -> Result<(), GatewayError> {
    let mut remaining = length;
    let mut chunk = [0u8; 8192];
    while remaining > 0 {
        check_cancelled(cancellation)?;
        let time = deadline
            .checked_duration_since(Instant::now())
            .filter(|duration| !duration.is_zero())
            .ok_or_else(auth_body_error)?;
        stream
            .set_read_timeout(Some(time))
            .map_err(|_| auth_body_error())?;
        let take = remaining.min(chunk.len());
        let count = match stream.read(&mut chunk[..take]) {
            Ok(count) => count,
            Err(_) => {
                chunk.fill(0);
                return Err(auth_body_error());
            }
        };
        if count == 0 {
            chunk.fill(0);
            check_cancelled(cancellation)?;
            return Err(auth_body_error());
        }
        output.extend_from_slice(&chunk[..count]);
        chunk[..count].fill(0);
        remaining -= count;
        check_cancelled(cancellation)?;
    }
    Ok(())
}
fn optional<'a>(head: &'a ImportHead, name: &str) -> Result<Option<&'a str>, GatewayError> {
    let mut values = head
        .headers
        .iter()
        .filter(|(key, _)| key.eq_ignore_ascii_case(name));
    let first = values.next().map(|(_, value)| value.as_str());
    if values.next().is_some() {
        return Err(invalid());
    }
    Ok(first)
}
fn check_cancelled<C: NativeRenewalCancellation + ?Sized>(
    cancellation: &C,
) -> Result<(), GatewayError> {
    if cancellation.cancelled() {
        return Err(control_interrupted());
    }
    Ok(())
}
fn auth_body_error() -> GatewayError {
    let error = gateway_native_import_source::body::BodyError::AuthBody;
    GatewayError::new(error.status(), error.message(), Some(error.code()))
}
