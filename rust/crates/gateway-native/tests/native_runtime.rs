#![cfg(target_os = "linux")]
use gateway_auth::{
    crypto::base64url, native_auth::NativeNonceAuthority, native_replay::StoreNonceAuthority,
};
use gateway_core::state::{AuthorityStore, RecordKey};
use gateway_native::{daemon_config::NativeDaemonConfig, nonce_sqlite::SqliteNonceStore};
use serde_json::json;
use std::{
    fs,
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
};

type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;
/// Converts a missing value or a failed result into the test's error instead of a panic.
trait Must<T> {
    fn must(self) -> TestResult<T>;
}
impl<T> Must<T> for Option<T> {
    fn must(self) -> TestResult<T> {
        self.ok_or_else(|| "missing value".into())
    }
}
impl<T, E: std::fmt::Debug> Must<T> for Result<T, E> {
    fn must(self) -> TestResult<T> {
        self.map_err(|error| format!("{error:?}").into())
    }
}
struct Fixture(PathBuf);
impl Fixture {
    fn new() -> std::io::Result<Self> {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let path = std::env::temp_dir().join(format!(
            "uos-native-runtime-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&path)?;
        Ok(Self(path))
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

#[test]
fn native_nonce_fences_survive_actual_database_close_and_restart() -> TestResult {
    let fixture = Fixture::new()?;
    let path = fixture.0.join("nonces.sqlite3");
    let nonce = base64url(&[7; 32]);
    {
        let store = SqliteNonceStore::open(&path).must()?;
        let authority = StoreNonceAuthority::new(&store);
        futures_executor::block_on(authority.consume_nonce("fixture-key", &nonce, 31_000, 1_000))
            .must()?;
        assert!(
            futures_executor::block_on(store.read(&RecordKey("not-the-fixed-native-key".into())))
                .is_err()
        );
    }
    let store = SqliteNonceStore::open(&path).must()?;
    let authority = StoreNonceAuthority::new(&store);
    let replay =
        futures_executor::block_on(authority.consume_nonce("fixture-key", &nonce, 31_000, 31_000))
            .err()
            .must()?;
    assert_eq!(
        replay.envelope.error.code.as_deref(),
        Some("native_nonce_replayed")
    );
    futures_executor::block_on(authority.consume_nonce("fixture-key", &nonce, 61_001, 31_001))
        .must()?;
    Ok(())
}

#[test]
fn replay_database_refuses_foreign_sqlite_without_modifying_it_or_its_directory() -> TestResult {
    let fixture = Fixture::new()?;
    let path = fixture.0.join("foreign.sqlite3");
    let database = rusqlite::Connection::open(&path).must()?;
    database.execute_batch("CREATE TABLE retained(value TEXT);INSERT INTO retained VALUES('synthetic-retained-value');").must()?;
    drop(database);
    let before = fs::read(&path).must()?;
    let mut entries = fs::read_dir(&fixture.0)
        .must()?
        .map(|entry| entry.map(|entry| entry.file_name()))
        .collect::<Result<Vec<_>, _>>()
        .must()?;
    entries.sort();
    assert!(SqliteNonceStore::open(&path).is_err());
    assert_eq!(fs::read(&path).must()?, before);
    let mut after = fs::read_dir(&fixture.0)
        .must()?
        .map(|entry| entry.map(|entry| entry.file_name()))
        .collect::<Result<Vec<_>, _>>()
        .must()?;
    after.sort();
    assert_eq!(entries, after);
    Ok(())
}

#[test]
fn daemon_stdin_requires_explicit_loopback_sources_and_canonical_dedicated_key() -> TestResult {
    let value = json!({"v":1,"listen":"127.0.0.1:0","identity":{"key_id":"fixture-key","owner_id":"fixture-owner","owner_epoch":"fixture-epoch","key_base64url":base64url(&[11;32])},"nonce_database":"/tmp/fixture-only/nonces.sqlite3","sources":[{"id":"fixture","name":"Synthetic Source","socket_path":"/tmp/fixture-only/source.sock","codex_home":null}]});
    let parsed = NativeDaemonConfig::read_stdin(value.to_string().as_bytes()).must()?;
    assert!(parsed.listen.ip().is_loopback());
    assert_eq!(parsed.sources[0].id(), "fixture");
    let mut outside = value.clone();
    outside["listen"] = json!("0.0.0.0:8071");
    assert!(NativeDaemonConfig::read_stdin(outside.to_string().as_bytes()).is_err());
    let mut key = value.clone();
    key["identity"]["key_base64url"] = json!(format!("{}=", base64url(&[11; 32])));
    assert!(NativeDaemonConfig::read_stdin(key.to_string().as_bytes()).is_err());
    let mut injected = value;
    injected["command"] = json!("not-an-operation");
    assert!(NativeDaemonConfig::read_stdin(injected.to_string().as_bytes()).is_err());
    Ok(())
}

#[test]
fn actual_kernel_profile_refuses_unconfigured_files_sockets_network_and_exec() -> TestResult {
    use std::{net::TcpListener, os::unix::net::UnixListener, process::Command};
    let fixture = Fixture::new()?;
    let allowed = fixture.0.join("allowed.txt");
    let outside = fixture.0.join("outside.txt");
    let private = fixture.0.join("replay");
    fs::write(&allowed, "synthetic-allowed").must()?;
    fs::write(&outside, "synthetic-outside-control").must()?;
    fs::create_dir(&private).must()?;
    let socket = fixture.0.join("allowed.sock");
    let other = fixture.0.join("outside.sock");
    let _allowed = UnixListener::bind(&socket).must()?;
    let _outside = UnixListener::bind(&other).must()?;
    let tcp = TcpListener::bind("127.0.0.1:0").must()?;
    let input = json!({"allowed":allowed,"outside":outside,"private":private,"socket":socket,"other_socket":other,"tcp":tcp.local_addr().must()?.to_string()});
    let output = Command::new(std::env::current_exe().must()?)
        .args(["--ignored", "--exact", "confinement_child", "--nocapture"])
        .env("UOS_NATIVE_TEST_PROFILE", input.to_string())
        .output()
        .must()?;
    assert!(
        output.status.success(),
        "child profile failed: {} {}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(String::from_utf8_lossy(&output.stdout).contains("NATIVE_PROFILE_ACCEPTED"));
    assert_eq!(
        fs::read_to_string(&outside).must()?,
        "synthetic-outside-control"
    );
    Ok(())
}

#[test]
#[ignore = "isolated child entered only by the kernel profile acceptance test"]
fn confinement_child() -> TestResult {
    use gateway_native::confinement::{NativeConfinement, enforce};
    use std::{net::TcpStream, os::unix::net::UnixStream, process::Command};
    let input: serde_json::Value =
        serde_json::from_str(&std::env::var("UOS_NATIVE_TEST_PROFILE").must()?).must()?;
    let path = |key: &str| input[key].as_str().map(PathBuf::from).must();
    let allowed = path("allowed")?;
    let outside = path("outside")?;
    let private = path("private")?;
    let socket = path("socket")?;
    let other = path("other_socket")?;
    let tcp = input["tcp"].as_str().must()?.to_owned();
    assert!(fs::read(&outside).is_ok());
    assert!(UnixStream::connect(&other).is_ok());
    assert!(TcpStream::connect(&tcp).is_ok());
    enforce(&NativeConfinement {
        readonly_paths: vec![allowed.clone()],
        list_directories: vec![],
        replay_directory: private.clone(),
        unix_sockets: vec![socket.clone()],
    })
    .must()?;
    assert_eq!(fs::read_to_string(&allowed).must()?, "synthetic-allowed");
    assert!(fs::read(&outside).is_err());
    assert!(fs::write(&outside, "must-not-write").is_err());
    fs::write(private.join("fixture-write"), "owned").must()?;
    assert!(UnixStream::connect(&socket).is_ok());
    assert!(UnixStream::connect(&other).is_err());
    assert!(TcpStream::connect(&tcp).is_err());
    assert!(Command::new("/bin/true").status().is_err());
    assert_eq!(std::thread::spawn(|| 17).join().must()?, 17);
    println!("NATIVE_PROFILE_ACCEPTED");
    Ok(())
}
