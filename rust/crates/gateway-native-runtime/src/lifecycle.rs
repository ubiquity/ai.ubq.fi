//! Authoritative resource-loss events stop new work and bound the drain before supervisor restart.
use rustix::event::{PollFd, PollFlags, poll};
use std::{
    fs::File,
    io::{self, Write},
    net::{TcpListener, TcpStream},
    os::unix::net::UnixStream,
    path::PathBuf,
    sync::{
        Arc, Condvar, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    time::{Duration, Instant},
};

#[derive(Clone, Debug)]
pub struct FailureSignal(Arc<Signal>);
#[derive(Debug)]
struct Signal {
    lost: AtomicBool,
    wake: Mutex<UnixStream>,
}
impl FailureSignal {
    pub fn fail(&self) {
        if !self.0.lost.swap(true, Ordering::AcqRel)
            && let Ok(mut wake) = self.0.wake.lock()
        {
            let _ = wake.write_all(&[1]);
        }
    }
    pub fn failed(&self) -> bool {
        self.0.lost.load(Ordering::Acquire)
    }
}

pub struct Recovery {
    signal: FailureSignal,
    wake: UnixStream,
    files: File,
    sockets: Vec<TcpStream>,
}
impl Recovery {
    /// All descriptors are acquired before confinement; duplicates pin the actual socket, never a reusable numeric FD.
    pub fn prepare(paths: &[PathBuf], sockets: &[&TcpStream]) -> io::Result<Self> {
        let (wake, sender) = UnixStream::pair()?;
        Ok(Self {
            signal: FailureSignal(Arc::new(Signal {
                lost: AtomicBool::new(false),
                wake: Mutex::new(sender),
            })),
            wake,
            files: watch_files(paths)?,
            sockets: sockets
                .iter()
                .map(|socket| socket.try_clone())
                .collect::<io::Result<_>>()?,
        })
    }
    pub fn signal(&self) -> FailureSignal {
        self.signal.clone()
    }
    /// Blocks on kernel events. TLS bytes are never consumed or treated as a failure.
    pub fn accept(&self, listener: &TcpListener) -> io::Result<Option<TcpStream>> {
        loop {
            if self.signal.failed() {
                return Ok(None);
            }
            let mut watched = vec![
                PollFd::new(listener, PollFlags::IN),
                PollFd::new(&self.wake, PollFlags::IN),
                PollFd::new(&self.files, PollFlags::IN),
            ];
            watched.extend(
                self.sockets
                    .iter()
                    .map(|socket| PollFd::new(socket, close_flags())),
            );
            match poll(&mut watched, None) {
                Err(rustix::io::Errno::INTR) => continue,
                Err(error) => return Err(error.into()),
                Ok(_) => {}
            }
            if watched.iter().skip(1).any(|fd| !fd.revents().is_empty()) || self.signal.failed() {
                self.signal.fail();
                return Ok(None);
            }
            if watched[0]
                .revents()
                .intersects(PollFlags::ERR | PollFlags::HUP | PollFlags::NVAL)
            {
                return Err(io::Error::other("Native listener lost"));
            }
            if watched[0].revents().contains(PollFlags::IN) {
                return listener.accept().map(|(stream, _)| Some(stream));
            }
        }
    }
}
fn close_flags() -> PollFlags {
    #[cfg(target_os = "linux")]
    {
        PollFlags::RDHUP | PollFlags::HUP | PollFlags::ERR
    }
    #[cfg(not(target_os = "linux"))]
    {
        PollFlags::HUP | PollFlags::ERR
    }
}
#[cfg(target_os = "linux")]
fn watch_files(paths: &[PathBuf]) -> io::Result<File> {
    use rustix::fs::inotify::{self, CreateFlags, WatchFlags};
    let fd = inotify::init(CreateFlags::CLOEXEC | CreateFlags::NONBLOCK)?;
    for path in paths {
        inotify::add_watch(
            &fd,
            path,
            WatchFlags::ATTRIB | WatchFlags::MOVE_SELF | WatchFlags::DELETE_SELF,
        )?;
    }
    Ok(File::from(fd))
}
#[cfg(not(target_os = "linux"))]
fn watch_files(_: &[PathBuf]) -> io::Result<File> {
    Err(io::Error::new(
        io::ErrorKind::Unsupported,
        "Native owner lifecycle confinement is unavailable",
    ))
}

#[derive(Clone, Default)]
pub struct ActiveRequests(Arc<(Mutex<usize>, Condvar)>);
pub struct RequestLease(ActiveRequests);
impl ActiveRequests {
    pub fn acquire(&self, limit: usize) -> Option<RequestLease> {
        let mut count = self.0.0.lock().ok()?;
        if *count >= limit {
            return None;
        }
        *count += 1;
        Some(RequestLease(self.clone()))
    }
    pub fn drain(&self, duration: Duration) -> bool {
        let deadline = Instant::now() + duration;
        let Ok(mut count) = self.0.0.lock() else {
            return false;
        };
        while *count != 0 {
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return false;
            }
            let Ok((next, _)) = self.0.1.wait_timeout(count, remaining) else {
                return false;
            };
            count = next;
        }
        true
    }
}
impl Drop for RequestLease {
    fn drop(&mut self) {
        let (count, wake) = self.0.0.as_ref();
        if let Ok(mut count) = count.lock() {
            *count -= 1;
            wake.notify_all();
        }
    }
}
