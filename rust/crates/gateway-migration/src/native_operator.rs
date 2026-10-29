//! Native orchestration over fixed custody, complete source preparation and private sinks.
use gateway_migration_native_custody::registry::{
    ConfiguredNativeCustody, NativeCustodyRegistration,
};

/// Native-only full audit/custody preparation. Only bounded canonical stage frames
/// leave this process. Ordinary profile/capture codecs and atomic adoption stay intact.
pub async fn prepare_local_stage(
    source: gateway_migration_data::native_capture::NativeCapture,
    registrations: Vec<NativeCustodyRegistration>,
) -> Result<gateway_migration_stage::plan::StagePlan, gateway_migration_data::error::MigrationError>
{
    let custody = ConfiguredNativeCustody::new(registrations)?;
    custody.verify_existing_access_only(&source).await?;
    let directory = gateway_migration_spool::directory(&std::env::temp_dir())?;
    gateway_migration_stage::native_stage::Captured::owned(source, &directory)?
        .prepare(&custody)
        .await
}

/// Same configured custody, with a stopped owner-only source file and private
/// sealed spool. No source payload inventory is collected by the operator.
pub async fn prepare_local_stage_file(
    path: &std::path::Path,
    directory: &std::path::Path,
    registrations: Vec<NativeCustodyRegistration>,
) -> Result<gateway_migration_stage::plan::StagePlan, gateway_migration_data::error::MigrationError>
{
    let custody = ConfiguredNativeCustody::new(registrations)?;
    let source = gateway_migration_stage::native_stage::Captured::read(path, directory)?;
    source.verify_configured(&custody).await?;
    source.prepare(&custody).await
}

/// Copy-only reconciliation over the caller's immutable authority snapshot.
/// The original file must be stopped and owner-only. All output is private data;
/// current Rust controls and nonces go only to the separate custody sink.
/// On any error discard the partial custody output and candidate spools.
pub async fn reconcile_local_history_file<
    S: gateway_core::query::QueryStore,
    H: gateway_migration_native_custody::native_custody::NativeCustody,
    C: std::io::Write,
>(
    store: &S,
    now_ms: i64,
    host: &H,
    artifacts: gateway_migration_data::custody::ArtifactContext,
    original_path: &std::path::Path,
    directory: &std::path::Path,
    custody_sink: &mut C,
) -> Result<
    gateway_migration_stage::native_stage::Captured,
    gateway_migration_data::error::MigrationError,
> {
    use std::{fs::OpenOptions, os::unix::fs::OpenOptionsExt};
    let root = gateway_migration_spool::directory(directory)?;
    let latest_path = root.join("latest-native.json");
    let mut latest_file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(&latest_path)
        .map_err(|_| {
            gateway_migration_data::invalid("Private latest source file cannot be created")
        })?;
    let (_, manifest, mapping_keys) =
        gateway_migration_latest::native_latest::write_latest_for_reconciliation(
            store,
            now_ms,
            host,
            artifacts,
            &mut latest_file,
            custody_sink,
        )
        .await?;
    latest_file.sync_all().map_err(|_| {
        gateway_migration_data::invalid("Private latest source file cannot be synced")
    })?;
    drop(latest_file);
    let original_directory = gateway_migration_spool::directory(&root)?;
    let latest_directory = gateway_migration_spool::directory(&root)?;
    let original =
        gateway_migration_stage::native_stage::Captured::read(original_path, &original_directory)?;
    let latest =
        gateway_migration_stage::native_stage::Captured::read(&latest_path, &latest_directory)?;
    gateway_migration_stage::native_stage::Captured::reconcile_histories(
        original,
        latest,
        &manifest,
        &mapping_keys,
        &root,
    )
}

/// Normal complete preparation follows the data-only reconciliation; this emits
/// no public authority, ownership or activation receipt.
pub async fn prepare_reconciled_stage_file<
    S: gateway_core::query::QueryStore,
    H: gateway_migration_native_custody::native_custody::NativeCustody,
    C: std::io::Write,
>(
    store: &S,
    now_ms: i64,
    host: &H,
    artifacts: gateway_migration_data::custody::ArtifactContext,
    original_path: &std::path::Path,
    directory: &std::path::Path,
    custody_sink: &mut C,
) -> Result<gateway_migration_stage::plan::StagePlan, gateway_migration_data::error::MigrationError>
{
    reconcile_local_history_file(
        store,
        now_ms,
        host,
        artifacts,
        original_path,
        directory,
        custody_sink,
    )
    .await?
    .prepare(host)
    .await
}
