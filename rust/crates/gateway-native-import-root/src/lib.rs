use gateway_authority_native_records::import_effects::NativeImportBinding;
use gateway_native_custody_io::protocol::unavailable;
use gateway_source::error::GatewayError;
use rustix::fs::{Mode, OFlags, fstat, openat};
use std::{
    fs::File,
    os::unix::fs::MetadataExt,
    path::{Component, Path},
};

pub struct NativeRootView<'a> {
    directory: &'a File,
    home: &'a Path,
    native_owner_id: &'a str,
    account_id_hash: &'a str,
}
impl<'a> NativeRootView<'a> {
    pub fn require_owner_binding(&self, binding: &NativeImportBinding) -> Result<(), GatewayError> {
        if binding.native_owner_id != self.native_owner_id
            || binding.account_id_hash != self.account_id_hash
        {
            return Err(unavailable());
        }
        Ok(())
    }

    pub fn borrow_registered_store(
        directory: &'a File,
        home: &'a Path,
        native_owner_id: &'a str,
        account_id_hash: &'a str,
    ) -> Self {
        Self {
            directory,
            home,
            native_owner_id,
            account_id_hash,
        }
    }
    pub fn recheck_live_root(
        &self,
        binding: &NativeImportBinding,
        stopped_root: (u64, u64),
    ) -> Result<(), GatewayError> {
        if binding.native_owner_id != self.native_owner_id
            || binding.account_id_hash != self.account_id_hash
        {
            return Err(unavailable());
        }
        let root = self.directory.metadata().map_err(|_| unavailable())?;
        let reopened = open_native_home(self.home)?;
        let current = reopened.metadata().map_err(|_| unavailable())?;
        if root.dev() != stopped_root.0
            || root.ino() != stopped_root.1
            || current.dev() != root.dev()
            || current.ino() != root.ino()
        {
            return Err(unavailable());
        }
        Ok(())
    }
}
fn open_native_home(home: &Path) -> Result<File, GatewayError> {
    if !home.is_absolute() {
        return Err(unavailable());
    }
    let mut directory = File::from(
        openat(
            rustix::fs::CWD,
            "/",
            OFlags::RDONLY | OFlags::DIRECTORY | OFlags::CLOEXEC,
            Mode::empty(),
        )
        .map_err(|_| unavailable())?,
    );
    for component in home.components() {
        match component {
            Component::RootDir => (),
            Component::Normal(name) => {
                directory = File::from(
                    openat(
                        &directory,
                        name,
                        OFlags::RDONLY | OFlags::DIRECTORY | OFlags::NOFOLLOW | OFlags::CLOEXEC,
                        Mode::empty(),
                    )
                    .map_err(|_| unavailable())?,
                );
            }
            _ => return Err(unavailable()),
        }
    }
    let stat = fstat(&directory).map_err(|_| unavailable())?;
    if stat.st_mode & 0o077 != 0 {
        return Err(unavailable());
    }
    Ok(directory)
}
