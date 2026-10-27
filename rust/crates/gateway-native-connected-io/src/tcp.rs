//! Already-owned TCP bytes only; no destination, request or authentication selector.
use std::{
    io::{self, Read, Write},
    net::TcpStream,
    time::Duration,
};
use ureq::unversioned::transport::{Buffers, LazyBuffers, NextTimeout, Transport};

#[derive(Debug)]
pub struct PreconnectedTcp {
    socket: TcpStream,
    buffers: LazyBuffers,
}
impl PreconnectedTcp {
    /// This wraps only the caller's already-owned stream and confers no peer/destination/authentication claim.
    pub fn from_connected(socket: TcpStream) -> Self {
        Self {
            socket,
            buffers: LazyBuffers::new(8192, 8192),
        }
    }
}
impl Transport for PreconnectedTcp {
    fn buffers(&mut self) -> &mut dyn Buffers {
        &mut self.buffers
    }
    fn transmit_output(&mut self, amount: usize, timeout: NextTimeout) -> Result<(), ureq::Error> {
        self.socket.set_write_timeout(io_timeout(timeout)?)?;
        self.socket
            .write_all(&self.buffers.output()[..amount])
            .map_err(|error| io_error(error, timeout))
    }
    fn await_input(&mut self, timeout: NextTimeout) -> Result<bool, ureq::Error> {
        self.socket.set_read_timeout(io_timeout(timeout)?)?;
        let count = self
            .socket
            .read(self.buffers.input_append_buf())
            .map_err(|error| io_error(error, timeout))?;
        self.buffers.input_appended(count);
        Ok(count > 0)
    }
    fn is_open(&mut self) -> bool {
        if self.socket.set_nonblocking(true).is_err() {
            return false;
        }
        let idle = matches!(self.socket.peek(&mut[0u8;1]),Err(error) if error.kind()==io::ErrorKind::WouldBlock);
        let restored = self.socket.set_nonblocking(false).is_ok();
        idle && restored
    }
}
fn io_timeout(timeout: NextTimeout) -> Result<Option<Duration>, ureq::Error> {
    use ureq::unversioned::transport::time::Duration as TransportDuration;
    match timeout.after {
        TransportDuration::NotHappening => Ok(None),
        TransportDuration::Exact(duration) if duration.is_zero() => {
            Err(ureq::Error::Timeout(timeout.reason))
        }
        TransportDuration::Exact(duration) => Ok(Some(duration)),
    }
}
fn io_error(error: io::Error, timeout: NextTimeout) -> ureq::Error {
    if matches!(
        error.kind(),
        io::ErrorKind::TimedOut | io::ErrorKind::WouldBlock
    ) {
        ureq::Error::Timeout(timeout.reason)
    } else {
        error.into()
    }
}
