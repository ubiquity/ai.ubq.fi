//! Original-context guarded actual custody TCP framing; decoded wire DATA is no live grant.
use crate::native_import::NativeImportContext;
#[cfg(test)]
use gateway_authority::native_control::import::custody_session::ImportCustodyStartedRequest;
use gateway_core::error::GatewayError;
use gateway_native_custody_wire::wire::{CUSTODY_FRAME_MAX, Frame};
#[cfg(test)]
use gateway_native_custody_wire::wire::{Header, Release, STARTED_BINDING_MAX, Stopped};
use std::{
    io::{Read, Write},
    net::TcpStream,
    time::{Duration, Instant},
};

pub(crate) fn random_ref() -> Result<String, GatewayError> {
    let mut bytes = [0u8; 32];
    getrandom::fill(&mut bytes).map_err(|_| unavailable())?;
    Ok(gateway_auth::crypto::sha256_hex(&bytes))
}
fn remaining(deadline: Instant) -> Result<Duration, GatewayError> {
    deadline
        .checked_duration_since(Instant::now())
        .filter(|d| !d.is_zero())
        .map(|d| d.min(Duration::from_millis(20)))
        .ok_or_else(unavailable)
}
fn check(context: Option<&NativeImportContext<'_>>) -> Result<(), GatewayError> {
    if let Some(context) = context {
        context.check()?;
    }
    Ok(())
}
fn read_exact(
    stream: &mut TcpStream,
    mut bytes: &mut [u8],
    deadline: Instant,
    context: Option<&NativeImportContext<'_>>,
) -> Result<(), GatewayError> {
    while !bytes.is_empty() {
        check(context)?;
        stream
            .set_read_timeout(Some(remaining(deadline)?))
            .map_err(|_| unavailable())?;
        match stream.read(bytes) {
            Ok(0) => return Err(unavailable()),
            Ok(n) => {
                bytes = &mut bytes[n..];
            }
            Err(e)
                if matches!(
                    e.kind(),
                    std::io::ErrorKind::TimedOut
                        | std::io::ErrorKind::WouldBlock
                        | std::io::ErrorKind::Interrupted
                ) => {}
            Err(_) => return Err(unavailable()),
        }
    }
    check(context)
}
pub(crate) fn receive(
    stream: &mut TcpStream,
    deadline: Instant,
    context: Option<&NativeImportContext<'_>>,
) -> Result<Frame, GatewayError> {
    let mut size = [0u8; 4];
    read_exact(stream, &mut size, deadline, context)?;
    let length = usize::try_from(u32::from_be_bytes(size)).map_err(|_| unavailable())?;
    if length == 0 || length > CUSTODY_FRAME_MAX {
        return Err(unavailable());
    }
    let mut bytes = vec![0; length];
    read_exact(stream, &mut bytes, deadline, context)?;
    Frame::decode(&bytes)
}
pub(crate) fn send(
    stream: &mut TcpStream,
    frame: &Frame,
    deadline: Instant,
    context: Option<&NativeImportContext<'_>>,
) -> Result<(), GatewayError> {
    let bytes = frame.encode()?;
    let mut framed = Vec::with_capacity(4 + bytes.len());
    framed.extend_from_slice(
        &u32::try_from(bytes.len())
            .map_err(|_| unavailable())?
            .to_be_bytes(),
    );
    framed.extend_from_slice(&bytes);
    let mut remaining_bytes = framed.as_slice();
    while !remaining_bytes.is_empty() {
        check(context)?;
        stream
            .set_write_timeout(Some(remaining(deadline)?))
            .map_err(|_| unavailable())?;
        match stream.write(remaining_bytes) {
            Ok(0) => return Err(unavailable()),
            Ok(n) => remaining_bytes = &remaining_bytes[n..],
            Err(e)
                if matches!(
                    e.kind(),
                    std::io::ErrorKind::TimedOut
                        | std::io::ErrorKind::WouldBlock
                        | std::io::ErrorKind::Interrupted
                ) => {}
            Err(_) => return Err(unavailable()),
        }
    }
    check(context)
}
pub(crate) fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Native import custody is unavailable; reconcile the original action",
        "native_import_custody_unknown",
    )
}
#[cfg(test)]
mod tests;
