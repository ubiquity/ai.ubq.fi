//! Private preparation CLI: one stopped owner-only capture into bounded stage frames.
//! It never opens a target, an HTTP route, a service, a credential refresh or a writer.
#![cfg(all(unix, not(target_arch = "wasm32")))]
use gateway_auth_credentials::{native_credentials::NativeOwnerId, native_credentials::OwnerFence};
use gateway_migration_data::staged::StageOwnerBinding;
use gateway_migration_data::staged::StageRequest;
use gateway_migration_native_custody::registry::ConfiguredNativeCustody;
use gateway_migration_native_custody::registry::NativeCustodyRegistration;
use gateway_migration_protocol::staged::decode_stage_request;
use gateway_migration_stage::native_stage::Captured;
use gateway_migration_stage::plan::StagePlan;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    fs::{File, OpenOptions},
    io::{Read, Write},
    os::unix::fs::{OpenOptionsExt, PermissionsExt},
    path::{Path, PathBuf},
};

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Fence {
    owner: String,
    account_id: String,
    generation_hash: String,
    authority_revision: String,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Registration {
    home: PathBuf,
    fence: Fence,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Expected {
    entries: usize,
    sha256: String,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Input {
    v: u32,
    capture: PathBuf,
    directory: PathBuf,
    frames: PathBuf,
    receipt: PathBuf,
    source_git_sha: String,
    expected: Expected,
    registrations: Vec<Registration>,
    #[serde(default)]
    owner: Option<StageOwnerBinding>,
}
#[derive(Serialize)]
struct Receipt {
    v: u32,
    kind: &'static str,
    source_git_sha: String,
    source_bytes: u64,
    source_sha256: String,
    entries: usize,
    frames: usize,
    append_frames: usize,
    max_stage_frame_bytes: usize,
    frame_file_bytes: u64,
    sealed: bool,
    owner_frame: bool,
    target_touched: bool,
    summary: gateway_migration_stage::plan::StagePlanSummary,
}
fn failure(message: &str) -> gateway_migration_data::error::MigrationError {
    gateway_source::error::GatewayError::unavailable(message, "m20_stage_frames_invalid").into()
}
fn sha256_file(path: &Path) -> Result<String, gateway_migration_data::error::MigrationError> {
    let mut file = File::open(path).map_err(|_| failure("Private capture cannot be hashed"))?;
    let mut hash = Sha256::new();
    let mut bytes = vec![0; 1024 * 1024];
    loop {
        let read = file
            .read(&mut bytes)
            .map_err(|_| failure("Private capture cannot be read"))?;
        if read == 0 {
            break;
        }
        hash.update(&bytes[..read]);
    }
    Ok(hash
        .finalize()
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect())
}
fn private_output(path: &Path) -> Result<File, gateway_migration_data::error::MigrationError> {
    OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(path)
        .map_err(|_| failure("Private stage output must be a NEW owner-only file"))
}
fn write_frame(
    file: &mut File,
    frame: &[u8],
    total: &mut u64,
    maximum: &mut usize,
) -> Result<(), gateway_migration_data::error::MigrationError> {
    if frame.len() > gateway_migration_data::staged::STAGED_BATCH_MAX_BYTES {
        return Err(failure("Private stage frame exceeds its bound"));
    }
    file.write_all(frame)
        .and_then(|()| file.write_all(b"\n"))
        .map_err(|_| failure("Private stage frame sink failed"))?;
    *maximum = (*maximum).max(frame.len());
    *total = total
        .checked_add(frame.len() as u64 + 1)
        .ok_or_else(|| failure("Private stage frame byte count overflows"))?;
    Ok(())
}
fn registrations(
    entries: Vec<Registration>,
) -> Result<Vec<NativeCustodyRegistration>, gateway_source::error::GatewayError> {
    entries
        .into_iter()
        .map(|registration| {
            NativeCustodyRegistration::existing(
                registration.home,
                OwnerFence {
                    owner: NativeOwnerId(registration.fence.owner),
                    account_id: registration.fence.account_id,
                    generation_hash: registration.fence.generation_hash,
                    authority_revision: registration.fence.authority_revision,
                },
            )
        })
        .collect()
}
fn poll_ready<F: std::future::Future>(
    mut future: std::pin::Pin<&mut F>,
    context: &mut std::task::Context<'_>,
) -> F::Output {
    loop {
        if let std::task::Poll::Ready(value) = future.as_mut().poll(context) {
            return value;
        }
        std::thread::yield_now();
    }
}
fn validate_input(input: &Input) -> Result<(), gateway_migration_data::error::MigrationError> {
    if input.v != 1
        || !input.capture.is_absolute()
        || !input.directory.is_absolute()
        || !input.frames.is_absolute()
        || !input.receipt.is_absolute()
        || !(1..=2).contains(&input.registrations.len())
        || input.expected.sha256.len() != 64
    {
        return Err(failure("Private stage frame options are invalid"));
    }
    Ok(())
}
fn main() -> Result<(), gateway_migration_data::error::MigrationError> {
    assert_eq!(std::env::args_os().len(), 1, "Private input is stdin only");
    let mut raw = Vec::new();
    std::io::stdin()
        .take(1024 * 1024)
        .read_to_end(&mut raw)
        .map_err(|_| failure("Private input cannot be read"))?;
    let input: Input =
        serde_json::from_slice(&raw).map_err(|_| failure("Private input is invalid"))?;
    validate_input(&input)?;
    let parent = std::fs::metadata(&input.directory)
        .map_err(|_| failure("Private stage parent is unavailable"))?;
    if !parent.is_dir() || parent.permissions().mode() & 0o077 != 0 {
        return Err(failure("Private stage parent must be owner-only"));
    }
    let source_bytes = std::fs::metadata(&input.capture)
        .map_err(|_| failure("Private capture metadata is unavailable"))?
        .len();
    let source_sha256 = sha256_file(&input.capture)?;
    if source_sha256 != input.expected.sha256 {
        return Err(failure(
            "Private capture hash differs from its producer receipt",
        ));
    }
    let registrations = registrations(input.registrations)?;
    let custody = ConfiguredNativeCustody::new(registrations)?;
    let directory = gateway_migration_spool::directory(&input.directory)?;
    let source = Captured::read(&input.capture, &directory)?;
    if source.count() != input.expected.entries {
        return Err(failure(
            "Private capture inventory count differs from its producer receipt",
        ));
    }
    let mut context = std::task::Context::from_waker(std::task::Waker::noop());
    {
        let verify = source.verify_configured(&custody);
        let mut verify = std::pin::pin!(verify);
        poll_ready(verify.as_mut(), &mut context)?;
    }
    let prepare = source.prepare(&custody);
    let mut prepare = std::pin::pin!(prepare);
    let mut plan: StagePlan = poll_ready(prepare.as_mut(), &mut context)?;
    let summary = plan.summary();
    if summary.source_git_sha != input.source_git_sha {
        return Err(failure(
            "Private capture source identity differs from its producer",
        ));
    }
    let mut frames = private_output(&input.frames)?;
    let mut receipt_file = private_output(&input.receipt)?;
    let mut total = 0u64;
    let mut maximum = 0usize;
    write_frame(
        &mut frames,
        &serde_json::to_vec(&plan.begin())
            .map_err(|_| failure("Private begin frame cannot be encoded"))?,
        &mut total,
        &mut maximum,
    )?;
    let mut append_frames = 0usize;
    let mut frame = Vec::new();
    while plan.write_next_batch(&mut frame)? {
        write_frame(&mut frames, &frame, &mut total, &mut maximum)?;
        append_frames += 1;
        frame.clear();
    }
    write_frame(
        &mut frames,
        &serde_json::to_vec(&plan.seal())
            .map_err(|_| failure("Private seal frame cannot be encoded"))?,
        &mut total,
        &mut maximum,
    )?;
    let owner_frame = if let Some(owner) = input.owner {
        write_frame(
            &mut frames,
            &serde_json::to_vec(&StageRequest::BindOwner(owner.clone()))
                .map_err(|_| failure("Private owner frame cannot be encoded"))?,
            &mut total,
            &mut maximum,
        )?;
        // The exact selected frame must decode through the shared closed schema.
        decode_stage_request(
            &serde_json::to_vec(&StageRequest::BindOwner(owner))
                .map_err(|_| failure("Private owner frame cannot be encoded"))?,
        )?;
        true
    } else {
        false
    };
    frames
        .sync_all()
        .map_err(|_| failure("Private stage frames cannot be synced"))?;
    let receipt = Receipt {
        v: 1,
        kind: "m20_stage_frames",
        source_git_sha: input.source_git_sha,
        source_bytes,
        source_sha256,
        entries: summary.source_rows,
        frames: append_frames + 2 + usize::from(owner_frame),
        append_frames,
        max_stage_frame_bytes: maximum,
        frame_file_bytes: total,
        sealed: true,
        owner_frame,
        target_touched: false,
        summary,
    };
    serde_json::to_writer_pretty(&mut receipt_file, &receipt)
        .map_err(|_| failure("Private stage receipt cannot be encoded"))?;
    receipt_file
        .write_all(b"\n")
        .and_then(|()| receipt_file.sync_all())
        .map_err(|_| failure("Private stage receipt cannot be synced"))?;
    serde_json::to_writer(std::io::stdout().lock(), &receipt)
        .map_err(|_| failure("Private stage receipt cannot be printed"))?;
    println!();
    Ok(())
}
