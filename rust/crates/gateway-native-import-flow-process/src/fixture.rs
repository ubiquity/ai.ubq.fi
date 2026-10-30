use gateway_auth_contracts::passkeys::PasskeyUser;
use gateway_auth_native_contracts::native_import::{NativeImportMetadata, NativeImportTarget};
use gateway_auth_passkeys::passkeys;
use gateway_auth_primitives::crypto::{base64url, sha256_hex};
use gateway_authority_native_records::owner::RegisteredRenewalOwner;
use gateway_core::state::AuthorityStore;
use gateway_native_custody_engine::exclusive_owner::ExclusiveManagedOwner;
use gateway_native_import_flow_authority::fixture::{
    NEW, OLD, Servers, admin_actor, authority, id, now,
};
use gateway_native_import_store::store::ExistingAccountImportStore;
use gateway_native_import_test_support::fixture as base;
use gateway_source::error::GatewayError;
use std::{
    fs,
    io::{Read, Write},
    net::{TcpListener, TcpStream},
    os::unix::{
        fs::{MetadataExt, OpenOptionsExt, PermissionsExt},
        process::CommandExt,
    },
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    sync::atomic::{AtomicU64, AtomicUsize, Ordering},
    thread,
    time::{Duration, Instant},
};
pub struct CaseRoot {
    base: PathBuf,
    root: PathBuf,
}
impl CaseRoot {
    pub fn base(&self) -> &Path {
        &self.base
    }
    pub fn path(&self) -> &Path {
        &self.root
    }
}
pub fn assert_fixture_child_binding(base: &Path, root: &Path, home: &Path) {
    assert_owned_canonical_directory(base);
    assert_owned_canonical_directory(root);
    assert_eq!(root.parent(), Some(base));
    assert_eq!(home, root.join("home"));
    assert_owned_canonical_directory(home);
}
impl CaseRoot {
    pub fn new() -> Self {
        static N: AtomicUsize = AtomicUsize::new(0);
        let parent = if cfg!(target_os = "macos") {
            PathBuf::from("/private/tmp")
        } else {
            fs::canonicalize(std::env::temp_dir()).unwrap()
        };
        let sequence = N.fetch_add(1, Ordering::SeqCst);
        let base = parent.join(format!(
            "native-flow-base-{}-{sequence}",
            std::process::id()
        ));
        fs::create_dir(&base).unwrap();
        fs::set_permissions(&base, fs::Permissions::from_mode(0o700)).unwrap();
        let root = base.join(format!("native-flow-{}-{sequence}", std::process::id()));
        fs::create_dir(&root).unwrap();
        fs::set_permissions(&root, fs::Permissions::from_mode(0o700)).unwrap();
        for name in ["home", "replay"] {
            fs::create_dir(root.join(name)).unwrap();
            fs::set_permissions(root.join(name), fs::Permissions::from_mode(0o700)).unwrap();
        }
        for (name, bytes) in [("auth.json", OLD), (".uos-owner.lock", b"".as_slice())] {
            let mut f = fs::OpenOptions::new()
                .create_new(true)
                .write(true)
                .mode(0o600)
                .open(root.join("home").join(name))
                .unwrap();
            f.write_all(bytes).unwrap();
            f.sync_all().unwrap();
        }
        Self { base, root }
    }
}
impl Drop for CaseRoot {
    fn drop(&mut self) {
        if thread::panicking() {
            eprintln!("PRESERVED_NATIVE_FLOW {}", self.base.display());
        } else {
            fs::remove_dir_all(&self.base).unwrap();
        }
    }
}
#[derive(serde::Deserialize, serde::Serialize)]
#[serde(deny_unknown_fields)]
pub struct ProcessSetup {
    pub home: PathBuf,
    pub config: serde_json::Value,
    pub authority: std::net::SocketAddr,
    pub models: std::net::SocketAddr,
    pub oauth: std::net::SocketAddr,
    pub account_id_hash: String,
}
pub struct OwnedGroup {
    pub child: Option<Child>,
    group: u32,
}
impl OwnedGroup {
    pub fn wait_clean(&mut self) {
        let limit = Instant::now() + Duration::from_secs(5);
        loop {
            if let Some(status) = self.child.as_mut().unwrap().try_wait().unwrap() {
                assert!(
                    status.success(),
                    "actual custodian must clean-exit, never signal/crash-as-pass"
                );
                self.child.take();
                return;
            }
            assert!(
                Instant::now() < limit,
                "custodian/actor group did not settle"
            );
            thread::sleep(Duration::from_millis(5));
        }
    }
}
impl Drop for OwnedGroup {
    fn drop(&mut self) {
        if let Some(mut child) = self.child.take() {
            if let Some(group) = rustix::process::Pid::from_raw(self.group as i32) {
                let _ = rustix::process::kill_process_group(group, rustix::process::Signal::KILL);
            }
            let _ = child.wait();
        }
    }
}
pub fn native_config(root: &Path, listen: std::net::SocketAddr) -> serde_json::Value {
    serde_json::json!({"v":2,"listen":listen,"identity":{"key_id":"native-flow","owner_id":"fixture-host","owner_epoch":"epoch","key_base64url":base64url(&[83;32])},"nonce_database":root.join("replay/nonces.sqlite3"),"sources":[],"owner_renewal":{"authority_origin":"https://localhost/","owners":[{"owner_id":"native-owner","codex_home":root.join("home")}]}})
}
pub fn start_custodian(
    root: &CaseRoot,
    servers: &Servers,
    account: String,
) -> (OwnedGroup, std::net::SocketAddr) {
    use std::os::fd::OwnedFd;
    let listener = TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, 0)).unwrap();
    let address = listener.local_addr().unwrap();
    let setup = ProcessSetup {
        home: root.path().join("home"),
        config: native_config(root.path(), address),
        authority: servers.authority,
        models: servers.models,
        oauth: servers.oauth,
        account_id_hash: account,
    };
    let mut file = fs::OpenOptions::new()
        .create_new(true)
        .write(true)
        .mode(0o600)
        .open(root.path().join("process.json"))
        .unwrap();
    file.write_all(&serde_json::to_vec(&setup).unwrap())
        .unwrap();
    file.sync_all().unwrap();
    let out = fs::OpenOptions::new()
        .create_new(true)
        .write(true)
        .mode(0o600)
        .open(root.path().join("custodian.stdout"))
        .unwrap();
    let err = fs::OpenOptions::new()
        .create_new(true)
        .write(true)
        .mode(0o600)
        .open(root.path().join("custodian.stderr"))
        .unwrap();
    let input: OwnedFd = listener.into();
    let child = Command::new(std::env::current_exe().unwrap())
        .args([
            "--exact",
            "existing_account_import::flow_tests::controlled_custodian_process",
            "--nocapture",
            "--ignored",
        ])
        .current_dir(root.path())
        .env_clear()
        .env("TMPDIR", std::env::temp_dir())
        .stdin(Stdio::from(input))
        .stdout(Stdio::from(out))
        .stderr(Stdio::from(err))
        .process_group(0)
        .spawn()
        .unwrap();
    let group = child.id();
    (
        OwnedGroup {
            child: Some(child),
            group,
        },
        address,
    )
}
pub fn request(
    store: &base::Store,
    home: &Path,
    account: String,
    address: std::net::SocketAddr,
    body: &[u8],
) -> Result<(Vec<u8>, NativeImportMetadata), GatewayError> {
    let authority = authority(store);
    let identity = id();
    let config = base::config();
    let cancel = base::Cancel::default();
    let owner = RegisteredRenewalOwner::configured(
        "native-owner".into(),
        home.to_str().ok_or_else(base::fail)?.into(),
    )
    .map_err(|_| base::fail())?;
    let actor = admin_actor(store)?;
    let client = gateway_native_import_client::client::NativeImportClient::configured(
        address, &authority, &identity, &owner, &config, &cancel,
    )
    .map_err(|_| base::fail())?;
    let prepared = futures_executor::block_on(client.prepare(
        &actor,
        NativeImportTarget::ExistingAccount {
            account_id_hash: account,
        },
    ))
    .map_err(|_| base::fail())?;
    let packet = client
        .request(prepared, body, now(), &base64url(&[9; 32]))
        .map_err(|_| base::fail())?;
    let metadata = packet.metadata().clone();
    let mut socket = TcpStream::connect(packet.endpoint()).map_err(|_| base::fail())?;
    socket
        .set_read_timeout(Some(Duration::from_secs(50)))
        .map_err(|_| base::fail())?;
    socket
        .set_write_timeout(Some(Duration::from_secs(5)))
        .map_err(|_| base::fail())?;
    let mut head = format!(
        "POST {} HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\n",
        packet.path(),
        packet.body().len()
    );
    for (k, v) in packet.headers() {
        head.push_str(&format!("{k}: {v}\r\n"));
    }
    head.push_str("\r\n");
    socket
        .write_all(head.as_bytes())
        .map_err(|_| base::fail())?;
    socket.write_all(packet.body()).map_err(|_| base::fail())?;
    let mut response = Vec::new();
    if let Err(error) = socket.read_to_end(&mut response) {
        let path = home
            .parent()
            .ok_or_else(base::fail)?
            .join("partial-native-response.bin");
        let mut saved = fs::OpenOptions::new()
            .create_new(true)
            .write(true)
            .mode(0o600)
            .open(path)
            .map_err(|_| base::fail())?;
        saved
            .write_all(&response[..response.len().min(8192)])
            .map_err(|_| base::fail())?;
        panic!(
            "native response read failed ({:?}); {} bytes observed, bounded private response retained",
            error.kind(),
            response.len()
        );
    }
    Ok((response, metadata))
}
pub struct RequestRun {
    pub socket: TcpStream,
    pub deadline_ms: i64,
    pub reply: thread::JoinHandle<Result<Vec<u8>, GatewayError>>,
}
pub fn original_request(
    store: &base::Store,
    root: &CaseRoot,
    account: String,
    address: std::net::SocketAddr,
) -> Result<RequestRun, GatewayError> {
    let authority = authority(store);
    let identity = id();
    let config = base::config();
    let cancel = base::Cancel::default();
    let owner = RegisteredRenewalOwner::configured(
        "native-owner".into(),
        root.path()
            .join("home")
            .to_str()
            .ok_or_else(base::fail)?
            .into(),
    )
    .map_err(|_| base::fail())?;
    let actor = admin_actor(store)?;
    let client = gateway_native_import_client::client::NativeImportClient::configured(
        address, &authority, &identity, &owner, &config, &cancel,
    )
    .map_err(|_| base::fail())?;
    let prepared = futures_executor::block_on(client.prepare(
        &actor,
        NativeImportTarget::ExistingAccount {
            account_id_hash: account,
        },
    ))
    .map_err(|_| base::fail())?;
    let deadline_ms = prepared.deadline_ms();
    let packet = client
        .request(prepared, NEW, now(), &base64url(&[9; 32]))
        .map_err(|_| base::fail())?;
    let socket = TcpStream::connect(packet.endpoint()).map_err(|_| base::fail())?;
    socket
        .set_read_timeout(Some(Duration::from_secs(50)))
        .map_err(|_| base::fail())?;
    socket
        .set_write_timeout(Some(Duration::from_secs(5)))
        .map_err(|_| base::fail())?;
    let mut worker = socket.try_clone().map_err(|_| base::fail())?;
    let mut head = format!(
        "POST {} HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\n",
        packet.path(),
        packet.body().len()
    );
    for (key, value) in packet.headers() {
        head.push_str(&format!("{key}: {value}\r\n"));
    }
    head.push_str("\r\n");
    let body = packet.body().to_vec();
    let reply = thread::spawn(move || -> Result<Vec<u8>, GatewayError> {
        worker
            .write_all(head.as_bytes())
            .map_err(|_| base::fail())?;
        worker.write_all(&body).map_err(|_| base::fail())?;
        let mut bytes = Vec::new();
        let read = worker.read_to_end(&mut bytes);
        if let Err(error) = read {
            assert!(
                matches!(
                    error.kind(),
                    std::io::ErrorKind::ConnectionReset | std::io::ErrorKind::BrokenPipe
                ),
                "unexpected native read error {error}"
            );
        }
        Ok(bytes)
    });
    Ok(RequestRun {
        socket,
        deadline_ms,
        reply,
    })
}
pub fn wait_failure(group: &mut OwnedGroup) {
    let deadline = Instant::now() + Duration::from_secs(8);
    loop {
        if let Some(status) = group.child.as_mut().unwrap().try_wait().unwrap() {
            assert_eq!(
                status.code(),
                Some(79),
                "fixture refusal must be a normal explicit error, not a signal or crash"
            );
            group.child.take();
            return;
        }
        assert!(
            Instant::now() < deadline,
            "controlled refusal did not settle its actual Child"
        );
        thread::sleep(Duration::from_millis(5));
    }
}
pub fn no_publication(servers: &Servers) {
    let phases = servers.traffic.phases.lock().unwrap();
    assert!(!phases.contains(&"publication") && !phases.contains(&"catalog_seed"));
    assert_eq!(servers.traffic.oauth.load(Ordering::SeqCst), 0);
}
pub fn no_native_effect(root: &CaseRoot, servers: &Servers, response: &[u8]) {
    assert!(
        !response.starts_with(b"HTTP/1.1 200 "),
        "an aborted real import must never return source success"
    );
    assert_eq!(fs::read(root.path().join("home/auth.json")).unwrap(), OLD);
    assert!(
        fs::read_dir(root.path().join("home"))
            .unwrap()
            .all(|entry| !entry
                .unwrap()
                .file_name()
                .to_string_lossy()
                .starts_with(".uos-import-")),
        "no native tempfile/receipt is allowed before the denied effect"
    );
    no_publication(servers);
}
pub fn revoke_admin(store: &base::Store) {
    let key = passkeys::key("users", "flow-user");
    let current = futures_executor::block_on(store.read(&key))
        .unwrap()
        .unwrap();
    let mut user: PasskeyUser =
        serde_json::from_value(current.json_value().unwrap().clone()).unwrap();
    user.is_admin = false;
    store.seed(key, "user-revoked", user);
}
pub fn assert_writer_stopped(root: &CaseRoot) {
    let path = root.path().join("home/async-native-write");
    let witness = fs::read(&path).unwrap();
    thread::sleep(Duration::from_millis(25));
    assert_eq!(
        fs::read(path).unwrap(),
        witness,
        "actual owned writer must already be stopped and reaped"
    );
}
pub fn assert_owned_canonical_directory(path: &Path) {
    assert_eq!(fs::canonicalize(path).unwrap(), path);
    let metadata = fs::symlink_metadata(path).unwrap();
    assert!(metadata.is_dir() && !metadata.file_type().is_symlink());
    assert_eq!(metadata.uid(), rustix::process::getuid().as_raw());
}
pub struct Home(pub PathBuf);
impl Home {
    pub fn new() -> Self {
        static ID: AtomicU64 = AtomicU64::new(0);
        let base = fs::canonicalize(std::env::temp_dir()).unwrap();
        assert_owned_canonical_directory(&base);
        let path = base.join(format!(
            "exclusive-owner-{}-{}",
            std::process::id(),
            ID.fetch_add(1, Ordering::SeqCst)
        ));
        fs::create_dir(&path).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o700)).unwrap();
        let mut auth = fs::OpenOptions::new()
            .create_new(true)
            .write(true)
            .mode(0o600)
            .open(path.join("auth.json"))
            .unwrap();
        auth.write_all(b"SYNTHETIC_OWNER_DATA").unwrap();
        auth.sync_all().unwrap();
        let lock = fs::OpenOptions::new()
            .create_new(true)
            .write(true)
            .mode(0o600)
            .open(path.join(".uos-owner.lock"))
            .unwrap();
        lock.sync_all().unwrap();
        Self(path)
    }
    pub fn store(&self) -> (ExistingAccountImportStore, RegisteredRenewalOwner) {
        let registration = RegisteredRenewalOwner::configured(
            "synthetic-exclusive-owner".into(),
            self.0.to_str().unwrap().into(),
        )
        .unwrap();
        let store = ExistingAccountImportStore::configured(
            &registration,
            sha256_hex(b"synthetic-existing-account"),
        )
        .unwrap();
        (store, registration)
    }
}
impl Drop for Home {
    fn drop(&mut self) {
        if std::thread::panicking() {
            eprintln!("PRESERVED_SYNTHETIC_OWNER_FIXTURE {}", self.0.display());
            if let Ok(bytes) = fs::read(self.0.join(".uos-owner-fixture.stderr")) {
                eprintln!(
                    "OWNED_CHILD_DIAGNOSTIC {}",
                    String::from_utf8_lossy(&bytes[..bytes.len().min(16384)])
                );
            }
            return;
        }
        fs::remove_dir_all(&self.0).unwrap();
    }
}
pub fn helper() -> (PathBuf, String) {
    let binary = std::env::current_exe()
        .unwrap()
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .join("uos-native-owner-inspect");
    assert!(
        binary.is_file(),
        "build exact inspector bin before these tests"
    );
    let digest = sha256_hex(&fs::read(&binary).unwrap());
    (binary, digest)
}
pub fn deadline() -> Instant {
    Instant::now() + Duration::from_secs(15)
}
pub fn owner(home: &Home) -> ExclusiveManagedOwner {
    let (store, registered) = home.store();
    let (helper, hash) = helper();
    let uid = rustix::process::getuid().as_raw();
    store
        .prepare_exclusive_owner(&registered, uid, &[uid + 1], &helper, &hash, deadline())
        .unwrap()
}
// Failure-only synthetic metadata. Do not replace the original observation error.
pub fn wait_ready(home: &Home) {
    let limit = deadline();
    while !home.0.join("async-native-write").exists() {
        assert!(
            Instant::now() < limit,
            "owned child failed readiness/no-descendant control"
        );
        std::thread::sleep(Duration::from_millis(5));
    }
    assert_eq!(
        fs::read(home.0.join("writer-ready")).unwrap(),
        b"process-denial-observed"
    );
}
