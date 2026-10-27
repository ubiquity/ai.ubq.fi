//! Private selected capture and manifest for the bounded Workers Free migration.
//! It opens one private stopped capture and writes no database, target, or
//! credential. A later preparation still requires the existing custody gate.
#![cfg(all(unix, not(target_arch = "wasm32")))]
use gateway_migration::native_stage::Captured;
use gateway_migration_data::error::MigrationError;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    fs::{self, File, OpenOptions},
    io::{Read, Write},
    os::unix::fs::{MetadataExt, OpenOptionsExt},
    path::{Path, PathBuf},
};

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Input {
    v: u32,
    capture: PathBuf,
    directory: PathBuf,
    expected_entries: usize,
    expected_sha256: String,
}
#[derive(Serialize)]
struct Receipt {
    v: u32,
    kind: &'static str,
    capture: String,
    bytes: u64,
    sha256: String,
    entries: usize,
    manifest_sha256: String,
    selected_capture_sha256: String,
    selected_capture_path: String,
    selected_capture_bytes: u64,
    manifest: gateway_migration_core_state::CoreStateManifest,
    eof: bool,
    spool_directory: String,
    target_touched: bool,
    credential_opened: bool,
}
fn failure(message: &str) -> gateway_migration_data::error::MigrationError {
    gateway_core::error::GatewayError::unavailable(message, "m20_core_state_manifest_invalid")
        .into()
}
fn main() -> Result<(), MigrationError> {
    assert_eq!(std::env::args_os().len(), 1, "Private input is stdin only");
    let mut raw = Vec::new();
    std::io::stdin()
        .take(1024 * 1024 + 1)
        .read_to_end(&mut raw)
        .map_err(|_| failure("Private input cannot be read"))?;
    if raw.len() > 1024 * 1024 {
        return Err(failure("Private input exceeds its bound"));
    }
    let input: Input =
        serde_json::from_slice(&raw).map_err(|_| failure("Private input is invalid"))?;
    let receipt = execute(input)?;
    serde_json::to_writer(std::io::stdout().lock(), &receipt)
        .map_err(|_| failure("Core-state manifest cannot be printed"))?;
    println!();
    Ok(())
}
fn hash_file(path: &Path) -> Result<String, MigrationError> {
    let mut file = File::open(path).map_err(|_| failure("Private capture cannot be read"))?;
    let mut hash = Sha256::new();
    let mut chunk = vec![0; 1024 * 1024];
    loop {
        let read = file
            .read(&mut chunk)
            .map_err(|_| failure("Private capture cannot be read"))?;
        if read == 0 {
            break;
        }
        hash.update(&chunk[..read]);
    }
    Ok(format!("{:x}", hash.finalize()))
}
fn write_selected(
    directory: &Path,
    bytes: &[u8],
    expected: &str,
) -> Result<PathBuf, MigrationError> {
    let path = directory.join("selected-capture.private.json");
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(&path)
        .map_err(|_| failure("Selected capture must be a new private file"))?;
    file.write_all(bytes)
        .and_then(|()| file.sync_all())
        .map_err(|_| failure("Selected capture cannot be written"))?;
    let opened = file
        .metadata()
        .map_err(|_| failure("Selected capture metadata is unavailable"))?;
    drop(file);
    let current = fs::symlink_metadata(&path).map_err(|_| failure("Selected capture changed"))?;
    if !current.is_file()
        || current.nlink() != 1
        || current.mode() & 0o777 != 0o600
        || current.dev() != opened.dev()
        || current.ino() != opened.ino()
        || current.len() != bytes.len() as u64
        || hash_file(&path)? != expected
    {
        return Err(failure("Selected capture changed"));
    }
    File::open(directory)
        .and_then(|file| file.sync_all())
        .map_err(|_| failure("Selected capture directory cannot be synced"))?;
    Ok(path)
}
fn execute(input: Input) -> Result<Receipt, MigrationError> {
    if input.v != 1 || !input.capture.is_absolute() || !input.directory.is_absolute() {
        return Err(failure("Private core-state options are invalid"));
    }
    let bytes = std::fs::metadata(&input.capture)
        .map_err(|_| failure("Private capture metadata is unavailable"))?
        .len();
    let sha256 = hash_file(&input.capture)?;
    if sha256 != input.expected_sha256 {
        return Err(failure(
            "Private capture hash differs from its producer receipt",
        ));
    }
    let directory = gateway_migration_spool::directory(&input.directory)?;
    let capture = Captured::read(&input.capture, &directory)?;
    if capture.count() != input.expected_entries {
        return Err(failure(
            "Private capture inventory count differs from its producer receipt",
        ));
    }
    let core = capture.core_capture()?;
    let selected_bytes = serde_json::to_vec(&core.capture)
        .map_err(|_| failure("Selected capture cannot be hashed"))?;
    let selected_capture_sha256: String = Sha256::digest(&selected_bytes)
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect();
    let manifest_bytes = serde_json::to_vec(&core.manifest)
        .map_err(|_| failure("Core-state manifest cannot be hashed"))?;
    let manifest_sha256: String = Sha256::digest(&manifest_bytes)
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect();
    if hash_file(&input.capture)? != sha256 {
        return Err(failure("Private input changed during selection"));
    }
    let selected_capture_path =
        write_selected(&directory, &selected_bytes, &selected_capture_sha256)?;
    Ok(Receipt {
        v: 2,
        kind: "m20_core_state_manifest",
        capture: input.capture.to_string_lossy().into_owned(),
        bytes,
        sha256,
        entries: capture.count(),
        manifest_sha256,
        selected_capture_sha256,
        selected_capture_path: selected_capture_path.to_string_lossy().into_owned(),
        selected_capture_bytes: selected_bytes.len() as u64,
        manifest: core.manifest,
        eof: true,
        spool_directory: directory.to_string_lossy().into_owned(),
        target_touched: false,
        credential_opened: false,
    })
}

#[cfg(test)]
mod tests {
    use super::{Input, execute, hash_file, write_selected};
    use gateway_core::tuple_key::{KeyPart, TupleKey};
    use gateway_migration_data::native_capture::NativeCapture;
    use gateway_migration_data::native_capture::NativeEntry;
    use gateway_migration_data::native_capture::NativeRevision;
    use gateway_migration_data::native_capture::NativeValue;
    use std::{
        fs,
        os::unix::fs::{DirBuilderExt, PermissionsExt},
        path::PathBuf,
        time::{SystemTime, UNIX_EPOCH},
    };

    struct Directory(PathBuf);
    impl Directory {
        fn new() -> Self {
            let stamp = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos();
            let path = std::env::temp_dir()
                .join(format!("uos-core-export-{}-{stamp}", std::process::id()));
            fs::DirBuilder::new().mode(0o700).create(&path).unwrap();
            Self(path)
        }
        fn input(&self, document: &[u8], entries: usize) -> Input {
            let path = self.0.join("source.private.json");
            fs::write(&path, document).unwrap();
            fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
            Input {
                v: 1,
                capture: path.clone(),
                directory: self.0.clone(),
                expected_entries: entries,
                expected_sha256: hash_file(&path).unwrap(),
            }
        }
    }
    impl Drop for Directory {
        fn drop(&mut self) {
            fs::remove_dir_all(&self.0).unwrap();
        }
    }
    fn entry(parts: &[&str], bytes: Vec<u8>) -> NativeEntry {
        NativeEntry {
            raw_key: TupleKey::from_parts(
                &parts
                    .iter()
                    .map(|part| KeyPart::String((*part).into()))
                    .collect::<Vec<_>>(),
            )
            .unwrap()
            .sort_bytes()
            .to_vec(),
            value: NativeValue::Bytes(bytes),
            revision: NativeRevision {
                version: "9".into(),
                sequence: "2".into(),
            },
            expiration_ms: i64::MAX,
        }
    }
    fn source() -> NativeCapture {
        // Same current-window schema as tests/core_state.rs; selection refuses
        // a capture without this truthful counter instead of resetting quota.
        let mut window = entry(
            &[
                "uos_ai",
                "api_key_usage",
                "v3",
                "window",
                "core-key",
                "v3:1000",
            ],
            vec![],
        );
        window.raw_key.extend(
            TupleKey::from_parts(&[KeyPart::Number(2000.0)])
                .unwrap()
                .sort_bytes(),
        );
        let fields = serde_json::json!({
            "v":3,"key_id":"core-key","policy_version":"v3:1000",
            "window_start_ms":2000,"window_reset_at_ms":3000,
            "committed_requests":42,"reserved_requests":0,"updated_at_ms":2100
        });
        let entries = fields
            .as_object()
            .unwrap()
            .iter()
            .map(|(key, value)| {
                let atom = if value.is_string() {
                    serde_json::json!({"type":"string","value":value})
                } else {
                    serde_json::json!({"type":"number","value":value})
                };
                serde_json::json!([key, atom])
            })
            .collect::<Vec<_>>();
        window.value = NativeValue::SourceGraphV1(
            gateway_core::source_json::SourceJson::from_serializable(
                &serde_json::json!({"root":{"type":"ref","ref":0},"nodes":[{"type":"object","entries":entries}]}),
            ).unwrap(),
        );
        NativeCapture {
            v: 2,
            source_git_sha: "b".repeat(40),
            captured_at_ms: 2200,
            source_codec: "bun-jsc-v1".into(),
            native_schema_version: 3,
            data_revision: NativeRevision {
                version: "10".into(),
                sequence: "0".into(),
            },
            queue_entries: 0,
            running_queue_entries: 0,
            entries: vec![
                entry(&["uos_ai", "runtime_config", "v2"], vec![0, 255]),
                entry(&["uos_ai", "admin_error_log", "v1", "history"], vec![99]),
                window,
            ],
        }
    }
    fn selected_files(directory: &Directory) -> usize {
        fs::read_dir(&directory.0)
            .unwrap()
            .filter_map(Result::ok)
            .filter(|item| {
                item.path().is_dir() && item.path().join("selected-capture.private.json").exists()
            })
            .count()
    }
    #[test]
    fn durable_selected_bytes_are_reduced_private_and_match_v2_receipt() {
        let directory = Directory::new();
        let original = source();
        let bytes = serde_json::to_vec(&original).unwrap();
        let input = directory.input(&bytes, 3);
        let source_path = input.capture.clone();
        let receipt = execute(input).unwrap();
        assert_eq!(receipt.v, 2);
        assert_eq!(receipt.entries, 3);
        assert_eq!(receipt.manifest.selected_rows, 2);
        assert_eq!(receipt.manifest.excluded_rows, 1);
        assert_eq!(receipt.manifest.do_row_writes, 5);
        let path = PathBuf::from(&receipt.selected_capture_path);
        let selected_bytes = fs::read(&path).unwrap();
        assert_eq!(selected_bytes.len() as u64, receipt.selected_capture_bytes);
        assert_eq!(hash_file(&path).unwrap(), receipt.selected_capture_sha256);
        assert_eq!(
            fs::metadata(&path).unwrap().permissions().mode() & 0o777,
            0o600
        );
        let selected: NativeCapture = serde_json::from_slice(&selected_bytes).unwrap();
        assert_eq!(selected.source_git_sha, original.source_git_sha);
        assert_eq!(selected.data_revision, original.data_revision);
        assert_eq!(selected.entries.len(), 2);
        assert_eq!(selected.entries[0].raw_key, original.entries[0].raw_key);
        assert_eq!(selected.entries[0].revision, original.entries[0].revision);
        assert_eq!(selected.entries[0].expiration_ms, i64::MAX);
        assert_eq!(
            serde_json::to_vec(&selected.entries[0].value).unwrap(),
            serde_json::to_vec(&original.entries[0].value).unwrap()
        );
        assert_eq!(fs::read(source_path).unwrap(), bytes);
        assert!(!receipt.target_touched && !receipt.credential_opened);
        assert!(!serde_json::to_string(&receipt).unwrap().contains("history"));
    }
    #[test]
    fn invalid_capture_hash_count_family_and_eof_publish_no_selected_file() {
        for bad in ["hash", "count", "family", "eof", "json"] {
            let directory = Directory::new();
            let mut capture = source();
            if bad == "family" {
                capture.entries.push(entry(&["unknown", "key"], vec![1]));
            }
            let mut bytes = serde_json::to_vec(&capture).unwrap();
            if bad == "eof" {
                bytes.extend_from_slice(b" trailing");
            }
            if bad == "json" {
                bytes = b"{}".to_vec();
            }
            let mut input = directory.input(&bytes, capture.entries.len());
            if bad == "hash" {
                input.expected_sha256 = "a".repeat(64);
            }
            if bad == "count" {
                input.expected_entries += 1;
            }
            assert!(execute(input).is_err(), "{bad}");
            assert_eq!(selected_files(&directory), 0, "{bad}");
        }
    }
    #[test]
    fn selected_output_never_replaces_existing_file_or_symlink() {
        let directory = Directory::new();
        let path = directory.0.join("selected-capture.private.json");
        fs::write(&path, b"keep").unwrap();
        assert!(write_selected(&directory.0, b"replace", "bad").is_err());
        assert_eq!(fs::read(&path).unwrap(), b"keep");
        fs::remove_file(&path).unwrap();
        let target = directory.0.join("existing");
        fs::write(&target, b"keep").unwrap();
        std::os::unix::fs::symlink(&target, &path).unwrap();
        assert!(write_selected(&directory.0, b"replace", "bad").is_err());
        assert_eq!(fs::read(target).unwrap(), b"keep");
    }
}
