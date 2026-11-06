//! Native reverse step: a verified Worker export archive into a NEW private
//! NativeCapture v2 through the existing latest inverse and custody checks.
//! It opens no live database; a Bun KV is written separately and only new.
use crate::archive::ArchiveStore;
use crate::refused;
use gateway_auth::native_credentials::OwnerFence;
use gateway_core::error::GatewayError;
use gateway_migration::error::MigrationError;
use gateway_migration::inverse::ArtifactContext;
use gateway_migration::native_custody::{NativeCustody, OwnedNativeSnapshot};
use gateway_migration::native_operator::{
    ConfiguredNativeCustody, NativeCustodyRegistration, write_private_latest,
};
use gateway_migration::operational::ArtifactInput;
use serde::{Deserialize, Serialize};
use std::fs::{DirBuilder, File, OpenOptions};
use std::io::{Read, Write};
use std::os::unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt};
use std::path::{Path, PathBuf};

/// Refuses every managed Codex account: only a capture without managed
/// refresh material can be reversed without configured native custody.
pub struct NoCustody;
impl NativeCustody for NoCustody {
    async fn observe(&self, _account_id: &str) -> Result<OwnedNativeSnapshot, GatewayError> {
        Err(GatewayError::unavailable(
            "A managed Codex account requires configured native custody",
            "migration_native_custody_required",
        ))
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ArchiveFact {
    pub path: PathBuf,
    pub sha256: String,
    pub records: usize,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Registration {
    pub home: PathBuf,
    pub fence: OwnerFence,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ReverseInput {
    pub v: u32,
    pub now_ms: i64,
    pub archive: ArchiveFact,
    pub registrations: Vec<Registration>,
    pub artifacts: ArtifactInput,
    pub output_directory: PathBuf,
}
#[derive(Debug, Serialize)]
pub struct OutputFact {
    pub path: PathBuf,
    pub sha256: String,
    pub bytes: u64,
}
#[derive(Debug, Serialize)]
pub struct ReverseReceipt {
    pub v: u32,
    pub kind: &'static str,
    pub archive_sha256: String,
    pub archive_records: usize,
    pub now_ms: i64,
    pub source_entries: usize,
    pub source: OutputFact,
    pub custody: OutputFact,
    pub activation_authorized: bool,
}

fn private_bytes(path: &Path) -> Result<Vec<u8>, GatewayError> {
    let metadata =
        std::fs::symlink_metadata(path).map_err(|_| refused("Export archive is unavailable"))?;
    if !path.is_absolute() || !metadata.is_file() || metadata.mode() & 0o077 != 0 {
        return Err(refused(
            "Export archive must be an absolute owner-only file",
        ));
    }
    let mut bytes = Vec::new();
    File::open(path)
        .and_then(|mut file| file.read_to_end(&mut bytes))
        .map_err(|_| refused("Export archive cannot be read"))?;
    if bytes.len() as u64 != metadata.len() {
        return Err(refused("Export archive changed while reading"));
    }
    Ok(bytes)
}

fn output(path: &Path) -> Result<File, GatewayError> {
    OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(path)
        .map_err(|_| refused("Reverse output must be a NEW owner-only file"))
}

fn fact(path: PathBuf) -> Result<OutputFact, GatewayError> {
    let bytes = std::fs::read(&path).map_err(|_| refused("Reverse output cannot be read"))?;
    Ok(OutputFact {
        sha256: gateway_auth::crypto::sha256_hex(&bytes),
        bytes: bytes.len() as u64,
        path,
    })
}

/// The caller holds the archive taken from a quiesced Worker. This returns
/// private evidence only: no restore, activation or owner constructor.
pub async fn reverse<H: NativeCustody>(
    store: &ArchiveStore,
    now_ms: i64,
    host: &H,
    artifacts: ArtifactContext,
    output_directory: &Path,
) -> Result<(usize, OutputFact, OutputFact), MigrationError> {
    if !output_directory.is_absolute() {
        return Err(refused("Reverse output directory must be absolute").into());
    }
    DirBuilder::new()
        .mode(0o700)
        .create(output_directory)
        .map_err(|_| refused("Reverse output directory must be NEW"))?;
    let source_path = output_directory.join("source.private.json");
    let custody_path = output_directory.join("custody.private.json");
    let mut source = output(&source_path)?;
    let mut custody = output(&custody_path)?;
    let entries =
        write_private_latest(store, now_ms, host, artifacts, &mut source, &mut custody).await?;
    for file in [&mut source, &mut custody] {
        file.flush()
            .and_then(|()| file.sync_all())
            .map_err(|_| refused("Reverse output cannot be synced"))?;
    }
    Ok((entries, fact(source_path)?, fact(custody_path)?))
}

fn run<F: std::future::Future>(future: F) -> F::Output {
    let mut context = std::task::Context::from_waker(std::task::Waker::noop());
    let mut future = std::pin::pin!(future);
    loop {
        if let std::task::Poll::Ready(value) = future.as_mut().poll(&mut context) {
            return value;
        }
        std::thread::yield_now();
    }
}

pub fn execute(input: ReverseInput) -> Result<ReverseReceipt, MigrationError> {
    if input.v != 1 || !(0..=gateway_core::identity::MAX_SAFE_INTEGER).contains(&input.now_ms) {
        return Err(refused("Reverse input is invalid").into());
    }
    let bytes = private_bytes(&input.archive.path)?;
    let store = ArchiveStore::from_bytes(&bytes, &input.archive.sha256, input.archive.records)?;
    let artifacts: ArtifactContext = input.artifacts.into();
    let (entries, source, custody) = if input.registrations.is_empty() {
        run(reverse(
            &store,
            input.now_ms,
            &NoCustody,
            artifacts,
            &input.output_directory,
        ))?
    } else {
        let registrations = input
            .registrations
            .into_iter()
            .map(|r| NativeCustodyRegistration::existing(r.home, r.fence))
            .collect::<Result<Vec<_>, _>>()?;
        let host = ConfiguredNativeCustody::new(registrations)?;
        run(reverse(
            &store,
            input.now_ms,
            &host,
            artifacts,
            &input.output_directory,
        ))?
    };
    if private_bytes(&input.archive.path)? != bytes {
        return Err(refused("Export archive changed during the reverse step").into());
    }
    Ok(ReverseReceipt {
        v: 1,
        kind: "state_transfer_reverse",
        archive_sha256: input.archive.sha256,
        archive_records: store.len(),
        now_ms: input.now_ms,
        source_entries: entries,
        source,
        custody,
        activation_authorized: false,
    })
}
