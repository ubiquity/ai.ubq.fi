#![cfg(target_os = "linux")]
use gateway_native_runtime::{
    lifecycle::{ActiveRequests, Recovery},
    owner_socket,
};
use std::{
    fs,
    net::{TcpListener, TcpStream},
    os::unix::{
        fs::{PermissionsExt, symlink},
        net::{UnixListener, UnixStream},
    },
    path::PathBuf,
    sync::{
        atomic::{AtomicUsize, Ordering},
        mpsc,
    },
    thread,
    time::{Duration, Instant},
};

mod support;
use support::{Must, TestResult};

struct Fixture(PathBuf);
impl Fixture {
    fn new() -> TestResult<Self> {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let path = std::env::temp_dir().join(format!(
            "uos-owner-lifetime-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&path).must()?;
        fs::set_permissions(&path, fs::Permissions::from_mode(0o700)).must()?;
        Ok(Self(path))
    }
    fn auth(&self) -> TestResult<PathBuf> {
        let p = self.0.join("auth.synthetic");
        fs::write(&p, "before").must()?;
        Ok(p)
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
fn tcp() -> TestResult<(TcpStream, TcpStream)> {
    let listener = TcpListener::bind("127.0.0.1:0").must()?;
    let client = TcpStream::connect(listener.local_addr().must()?).must()?;
    Ok((client, listener.accept().must()?.0))
}
#[test]
fn relocated_private_socket_keeps_exact_owner_and_rejects_unsafe_custody() -> TestResult {
    let home = Fixture::new()?;
    let relocated = Fixture::new()?;
    let control = home.0.join("app-server-control");
    fs::create_dir(&control).must()?;
    fs::set_permissions(&control, fs::Permissions::from_mode(0o700)).must()?;
    let target = relocated.0.join("socket");
    let listener = UnixListener::bind(&target).must()?;
    fs::set_permissions(&target, fs::Permissions::from_mode(0o600)).must()?;
    symlink(&target, control.join("app-server-control.sock")).must()?;
    assert_eq!(owner_socket::resolve(&home.0).must()?, target);
    let peer = thread::spawn(move || listener.accept().map(|(stream, _)| stream));
    let connected = UnixStream::connect(owner_socket::resolve(&home.0).must()?).must()?;
    owner_socket::verify_peer(&connected).must()?;
    drop(peer.join().must()?.must()?);
    fs::set_permissions(&target, fs::Permissions::from_mode(0o666)).must()?;
    assert!(owner_socket::resolve(&home.0).is_err());
    fs::set_permissions(&target, fs::Permissions::from_mode(0o600)).must()?;
    fs::set_permissions(&relocated.0, fs::Permissions::from_mode(0o777)).must()?;
    assert!(owner_socket::resolve(&home.0).is_err());
    fs::set_permissions(&relocated.0, fs::Permissions::from_mode(0o700)).must()?;
    fs::remove_file(&target).must()?;
    fs::write(&target, "not a socket").must()?;
    fs::set_permissions(&target, fs::Permissions::from_mode(0o600)).must()?;
    assert!(owner_socket::resolve(&home.0).is_err());
    Ok(())
}
#[test]
fn remote_close_before_first_use_stops_admission() -> TestResult {
    let fixture = Fixture::new()?;
    let auth = fixture.auth()?;
    let (client, peer) = tcp()?;
    let recovery = Recovery::prepare(&[auth], &[&client]).must()?;
    let listener = TcpListener::bind("127.0.0.1:0").must()?;
    drop(peer);
    let start = Instant::now();
    assert!(recovery.accept(&listener).must()?.is_none());
    assert!(start.elapsed() < Duration::from_secs(1));
    assert!(recovery.signal().failed());
    Ok(())
}
#[test]
fn in_place_write_keeps_lifetime_but_atomic_replacement_loses_it() -> TestResult {
    let fixture = Fixture::new()?;
    let auth = fixture.auth()?;
    let (client, _peer) = tcp()?;
    let recovery = Recovery::prepare(std::slice::from_ref(&auth), &[&client]).must()?;
    let listener = TcpListener::bind("127.0.0.1:0").must()?;
    fs::write(&auth, "same inode").must()?;
    let incoming = TcpStream::connect(listener.local_addr().must()?).must()?;
    assert!(recovery.accept(&listener).must()?.is_some());
    assert!(!recovery.signal().failed());
    drop(incoming);
    let replacement = fixture.0.join("replacement");
    fs::write(&replacement, "new inode").must()?;
    fs::rename(replacement, auth).must()?;
    assert!(recovery.accept(&listener).must()?.is_none());
    Ok(())
}
#[test]
fn in_flight_loss_drains_existing_effect_without_another_admission() -> TestResult {
    let fixture = Fixture::new()?;
    let (client, peer) = tcp()?;
    let recovery = Recovery::prepare(&[fixture.auth()?], &[&client]).must()?;
    let signal = recovery.signal();
    let listener = TcpListener::bind("127.0.0.1:0").must()?;
    let active = ActiveRequests::default();
    let lease = active.acquire(1).must()?;
    let effects = std::sync::Arc::new(AtomicUsize::new(0));
    let observed = effects.clone();
    let (release, wait) = mpsc::channel();
    let handler = thread::spawn(move || {
        let _lease = lease;
        observed.fetch_add(1, Ordering::SeqCst);
        wait.recv()
    });
    drop(peer);
    assert!(recovery.accept(&listener).must()?.is_none());
    assert!(signal.failed());
    assert!(!active.drain(Duration::from_millis(1)));
    release.send(()).must()?;
    assert!(active.drain(Duration::from_secs(1)));
    handler.join().must()?.must()?;
    assert_eq!(effects.load(Ordering::SeqCst), 1);
    Ok(())
}
#[test]
fn explicit_transport_failure_wakes_blocked_accept_once() -> TestResult {
    let fixture = Fixture::new()?;
    let (client, _peer) = tcp()?;
    let recovery = Recovery::prepare(&[fixture.auth()?], &[&client]).must()?;
    let signal = recovery.signal();
    let listener = TcpListener::bind("127.0.0.1:0").must()?;
    let task = thread::spawn(move || recovery.accept(&listener).map(|v| v.is_none()));
    signal.fail();
    signal.fail();
    assert!(task.join().must()?.must()?);
    Ok(())
}
#[test]
fn non_socket_and_missing_parent_are_refused() -> TestResult {
    let fixture = Fixture::new()?;
    assert!(owner_socket::resolve(&fixture.0).is_err());
    Ok(())
}
