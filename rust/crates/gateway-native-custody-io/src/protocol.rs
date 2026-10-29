//! Original-context guarded actual custody TCP framing; decoded wire DATA is no live grant.
use gateway_native_custody_wire::wire::{CUSTODY_FRAME_MAX, Frame};
use gateway_native_http_admission::native_import::NativeImportContext;
use gateway_source::error::GatewayError;
use std::{
    io::{Read, Write},
    net::TcpStream,
    time::{Duration, Instant},
};

pub fn random_ref() -> Result<String, GatewayError> {
    let mut bytes = [0u8; 32];
    getrandom::fill(&mut bytes).map_err(|_| unavailable())?;
    Ok(gateway_auth_primitives::crypto::sha256_hex(&bytes))
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
pub fn receive(
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
pub fn send(
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
pub fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Native import custody is unavailable; reconcile the original action",
        "native_import_custody_unknown",
    )
}
