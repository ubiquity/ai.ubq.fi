#![cfg(target_os = "macos")]
use super::{observe, parse};
use crate::pin::PinnedExecutable;
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_native_confinement::file_observation::Facts;
use std::{
    fs,
    io::Write,
    os::unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt},
    path::{Path, PathBuf},
    process::{Command, Stdio},
    sync::atomic::{AtomicU64, Ordering},
    time::{Duration, Instant},
};
static FOUNDATION_FIXTURES: std::sync::Mutex<()> = std::sync::Mutex::new(());
pub(super) fn assert_owned_canonical_directory(path: &Path) {
    assert_eq!(fs::canonicalize(path).unwrap(), path);
    let metadata = fs::symlink_metadata(path).unwrap();
    assert!(metadata.is_dir() && !metadata.file_type().is_symlink());
    assert_eq!(metadata.uid(), rustix::process::getuid().as_raw());
}
struct Home(PathBuf);
impl Home {
    fn new() -> Self {
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
fn helper() -> (PathBuf, String) {
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
fn deadline() -> Instant {
    Instant::now() + Duration::from_secs(15)
}
fn report_inspector_failure(directory: &fs::File, helper: &PinnedExecutable, path: &Path) {
    let ls = fs::read("/bin/ls").map(|bytes| sha256_hex(&bytes));
    eprintln!(
        "NATIVE_INSPECTOR_FIRST_FAILURE os={} arch={} fixed_ls_sha256={ls:?} synthetic_leaf={}",
        std::env::consts::OS,
        std::env::consts::ARCH,
        path.display()
    );
    for (kind, metadata) in [
        ("fixed_ls", fs::symlink_metadata("/bin/ls")),
        ("synthetic_leaf", directory.metadata()),
    ] {
        if let Ok(metadata) = metadata {
            eprintln!(
                "NATIVE_INSPECTOR_METADATA kind={kind} uid={} gid={} mode={:o} links={} size={}",
                metadata.uid(),
                metadata.gid(),
                metadata.mode(),
                metadata.nlink(),
                metadata.len()
            );
        }
    }
    let Ok(stdin) = directory.try_clone() else {
        eprintln!("NATIVE_INSPECTOR_DIAGNOSTIC stdin_clone_failed");
        return;
    };
    let child = Command::new(helper.path())
        .env_clear()
        .env("LC_ALL", "C")
        .env("LANG", "C")
        .env("TZ", "UTC")
        .stdin(Stdio::from(stdin))
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn();
    let Ok(mut child) = child else {
        eprintln!("NATIVE_INSPECTOR_DIAGNOSTIC fixed_helper_spawn_failed");
        return;
    };
    let limit = Instant::now() + Duration::from_secs(3);
    let timed_out = loop {
        match child.try_wait() {
            Ok(Some(_)) => break false,
            Ok(None) if Instant::now() < limit => std::thread::sleep(Duration::from_millis(5)),
            _ => {
                let _ = child.kill();
                break true;
            }
        }
    };
    if let Ok(output) = child.wait_with_output() {
        eprintln!(
            "NATIVE_INSPECTOR_DIAGNOSTIC fixed_helper_exit={:?} timed_out={timed_out} stdout={:?} stderr={:?}",
            output.status.code(),
            String::from_utf8_lossy(&output.stdout[..output.stdout.len().min(1024)]),
            String::from_utf8_lossy(&output.stderr[..output.stderr.len().min(1024)])
        );
    }
}
#[test]
fn actual_anchored_inspector_no_acl_preserves_parent_cwd_and_rejects_real_ace() {
    let _fixture_family = FOUNDATION_FIXTURES.lock().unwrap();
    let home = Home::new();
    let file = fs::File::open(&home.0).unwrap();
    let (helper, hash) = helper();
    let helper = PinnedExecutable::verify(&helper, &hash).unwrap();
    let cwd = std::env::current_dir().unwrap();
    let observed = observe(&file, &helper, deadline());
    if observed.is_err() {
        report_inspector_failure(&file, &helper, &home.0);
    }
    observed.unwrap();
    assert_eq!(std::env::current_dir().unwrap(), cwd);
    let output = Command::new("/usr/bin/id")
        .arg("-un")
        .env_clear()
        .output()
        .unwrap();
    assert!(output.status.success() && output.stderr.is_empty());
    let username = std::str::from_utf8(&output.stdout)
        .unwrap()
        .trim_end_matches('\n');
    assert!(
        !username.is_empty()
            && username
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
    );
    let ace = format!("user:{username} allow readsecurity");
    let status = Command::new("/bin/chmod")
        .args(["+a", &ace])
        .arg(&home.0)
        .status()
        .unwrap();
    assert!(status.success());
    assert!(observe(&file, &helper, deadline()).is_err());
    let status = Command::new("/bin/chmod")
        .args(["-a", &ace])
        .arg(&home.0)
        .status()
        .unwrap();
    assert!(status.success());
    observe(&file, &helper, deadline()).unwrap();
    assert_eq!(std::env::current_dir().unwrap(), cwd);
}
#[test]
fn full_numeric_header_parser_rejects_prefixes_acl_control_unknown_and_stat_mismatch() {
    let _fixture_family = FOUNDATION_FIXTURES.lock().unwrap();
    let home = Home::new();
    let facts = Facts::read(&fs::File::open(&home.0).unwrap()).unwrap();
    let header = format!(
        "drwx------@ {} {} {} {} Oct 20 04:55 .\n",
        facts.links, facts.uid, facts.gid, facts.size
    );
    parse(header.as_bytes(), &facts).unwrap();
    for bytes in [
        header.trim_end().as_bytes().to_vec(),
        format!(" {header}").into_bytes(),
        header.replace(".\n", ". \n").into_bytes(),
        header.replace(" 20 ", " 020 ").into_bytes(),
        format!("{header} 0: user:501 allow write\n").into_bytes(),
        format!("{header}\n").into_bytes(),
        header.replace("04:55", "99:99").into_bytes(),
        header.replace(".\n", "\u{001b}.\n").into_bytes(),
        header.replace("drwx------@", "drwx------+ ").into_bytes(),
        vec![b' '; 1025],
    ] {
        assert!(parse(&bytes, &facts).is_err());
    }
    let mut changed = facts;
    changed.uid += 1;
    assert!(parse(header.as_bytes(), &changed).is_err());
}
