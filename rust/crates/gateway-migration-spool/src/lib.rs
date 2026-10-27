//! Finite native-private sealed DATA spools; generic storage never leaves this crate.
#![cfg(all(unix, not(target_arch = "wasm32")))]
pub mod canonical;
pub mod native_entries;
mod raw;
pub mod rows;
use gateway_core::error::GatewayError;
use gateway_migration_data::invalid;
use std::path::{Path, PathBuf};

/// Fresh private directory under the caller's task-owned stopped-file location.
pub fn directory(parent: &Path) -> Result<PathBuf, GatewayError> {
    use std::os::unix::fs::DirBuilderExt;
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|_| invalid("Private spool clock is invalid"))?
        .as_nanos();
    static NEXT_DIRECTORY: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let sequence = NEXT_DIRECTORY.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let path = parent.join(format!(
        "uos-native-stage-{}-{stamp}-{sequence}",
        std::process::id()
    ));
    std::fs::DirBuilder::new()
        .mode(0o700)
        .create(&path)
        .map_err(|_| invalid("Private spool directory cannot be created"))?;
    Ok(path)
}
