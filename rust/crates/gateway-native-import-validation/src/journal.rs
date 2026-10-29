use gateway_auth_credentials::native_credentials::NativeCredentials;
use gateway_native_custody_io::protocol::unavailable;
use gateway_native_import_context::context::NativeImportInputs;
use gateway_native_import_request::live::LiveImportCustody;
use gateway_native_import_root::NativeRootView;
use gateway_source::{error::GatewayError, source_json::SourceJson};
use rustix::fs::{Mode, OFlags, fstat, openat};
use std::{fs::File, io::Read, path::Path};
const AUTH_LIMIT: u64 = 1_048_576;

pub struct RotationStore<'a> {
    directory: &'a File,
    root: NativeRootView<'a>,
}
impl<'a> RotationStore<'a> {
    pub fn borrow_registered_store(
        directory: &'a File,
        home: &'a Path,
        owner: &'a str,
        account: &'a str,
    ) -> Self {
        Self {
            directory,
            root: NativeRootView::borrow_registered_store(directory, home, owner, account),
        }
    }
    pub(crate) fn root(&self) -> &NativeRootView<'_> {
        &self.root
    }
    pub fn retain_rotated(
        &self,
        custody: &mut LiveImportCustody<'_>,
        context: &NativeImportInputs<'_>,
        rotated: &SourceJson,
    ) -> Result<(), GatewayError> {
        retain_rotated(self, custody, context, rotated)
    }
}
fn read_fixed(directory: &File, name: &str, limit: u64) -> Result<Vec<u8>, GatewayError> {
    let file = File::from(
        openat(
            directory,
            name,
            OFlags::RDONLY | OFlags::NOFOLLOW | OFlags::CLOEXEC | OFlags::NONBLOCK,
            Mode::empty(),
        )
        .map_err(|_| unavailable())?,
    );
    let parent = fstat(directory).map_err(|_| unavailable())?;
    let stat = fstat(&file).map_err(|_| unavailable())?;
    if rustix::fs::FileType::from_raw_mode(stat.st_mode) != rustix::fs::FileType::RegularFile
        || stat.st_uid != parent.st_uid
        || stat.st_mode & 0o077 != 0
        || stat.st_nlink != 1
        || stat.st_size < 0
        || stat.st_size as u64 > limit
    {
        return Err(unavailable());
    }
    let mut bytes = Vec::new();
    file.take(limit + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| unavailable())?;
    if bytes.len() as u64 > limit {
        return Err(unavailable());
    }
    Ok(bytes)
}
fn credentials(raw: &[u8]) -> Result<NativeCredentials, GatewayError> {
    let document = SourceJson::parse_bytes(raw).map_err(|_| unavailable())?;
    gateway_native_import_source::documents::credentials(&document).map_err(|_| unavailable())
}
pub fn retain_rotated(
    store: &RotationStore<'_>,
    custody: &mut LiveImportCustody<'_>,
    context: &NativeImportInputs<'_>,
    rotated: &SourceJson,
) -> Result<(), GatewayError> {
    use std::io::Write;
    custody.check_live(&store.root, context)?;
    let binding = custody.copy_binding_data();
    store.root.require_owner_binding(&binding)?;
    let current = credentials(&read_fixed(store.directory, "auth.json", AUTH_LIMIT)?)?;
    let next = gateway_native_import_source::documents::credentials(rotated)?;
    let name = custody.rotation_name_for_source(&current, &next)?;
    let mut file = File::from(
        openat(
            store.directory,
            &name,
            OFlags::WRONLY | OFlags::CREATE | OFlags::EXCL | OFlags::NOFOLLOW | OFlags::CLOEXEC,
            Mode::RUSR | Mode::WUSR,
        )
        .map_err(|_| unavailable())?,
    );
    custody.check_live(&store.root, context)?;
    file.write_all(rotated.as_bytes())
        .map_err(|_| unavailable())?;
    custody.check_live(&store.root, context)?;
    file.sync_all().map_err(|_| unavailable())?;
    custody.check_live(&store.root, context)?;
    rustix::fs::fsync(store.directory).map_err(|_| unavailable())?;
    custody.check_live(&store.root, context)
}
