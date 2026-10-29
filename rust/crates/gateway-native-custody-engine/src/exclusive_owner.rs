//! Kernel facts are observations, never permission reconstructed from JSON or caller booleans.
use crate::owner_runtime;
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_authority_native_records::owner::RegisteredRenewalOwner;
use gateway_native_confinement::file_observation::Facts;
use gateway_native_owner_inspector::inspector::VerifiedInspector;
use gateway_source::error::GatewayError;
use rustix::fs::{AtFlags, Mode, OFlags, fstat, openat, statat};
use std::{
    fs::File,
    io::Read,
    path::{Component, Path, PathBuf},
    time::Instant,
};
struct Anchor {
    directory: File,
    facts: Facts,
    component: Option<std::ffi::OsString>,
}
/// Verified kernel custody, NOT permission to mutate. No public constructor/serde/Clone/Debug.
/// ```compile_fail
/// use gateway_native_custody_engine::exclusive_owner::ExclusiveManagedOwner;
/// let forged: ExclusiveManagedOwner = serde_json::from_str("{}").unwrap();
/// ```
pub struct ExclusiveManagedOwner {
    anchors: Vec<Anchor>,
    pub(crate) inspector: VerifiedInspector,
    pub(crate) home: PathBuf,
    pub(crate) writer_uid: u32,
    pub(crate) native_owner_id: String,
    pub(crate) account_id_hash: String,
}
impl ExclusiveManagedOwner {
    pub fn prepare(
        store: (&File, &Path, &str, &str),
        registered: &RegisteredRenewalOwner,
        writer: u32,
        clients: &[u32],
        inspector: &Path,
        digest: &str,
        deadline: Instant,
    ) -> Result<Self, GatewayError> {
        let (store_directory, store_home, store_owner, store_account) = store;
        let uid = rustix::process::getuid();
        let effective = rustix::process::geteuid();
        if uid.is_root()
            || uid != effective
            || uid.as_raw() != writer
            || clients.is_empty()
            || clients
                .iter()
                .any(|client| *client == 0 || *client == writer)
            || registered.native_owner_id() != store_owner
            || Path::new(registered.registered_home()) != store_home
        {
            return Err(fail());
        }
        let inspector = VerifiedInspector::prepare(inspector, digest)?;
        let mut anchors = Vec::new();
        let root = File::from(
            openat(
                rustix::fs::CWD,
                "/",
                OFlags::RDONLY | OFlags::DIRECTORY | OFlags::NOFOLLOW | OFlags::CLOEXEC,
                Mode::empty(),
            )
            .map_err(|_| fail())?,
        );
        let facts = Facts::read(&root)?;
        anchors.push(Anchor {
            directory: root,
            facts,
            component: None,
        });
        for component in store_home.components() {
            let Component::Normal(name) = component else {
                if matches!(component, Component::RootDir) {
                    continue;
                }
                return Err(fail());
            };
            let previous = anchors.last().ok_or_else(fail)?;
            let directory = File::from(
                openat(
                    &previous.directory,
                    name,
                    OFlags::RDONLY | OFlags::DIRECTORY | OFlags::NOFOLLOW | OFlags::CLOEXEC,
                    Mode::empty(),
                )
                .map_err(|_| fail())?,
            );
            let facts = Facts::read(&directory)?;
            anchors.push(Anchor {
                directory,
                facts,
                component: Some(name.into()),
            });
        }
        let this = Self {
            anchors,
            inspector,
            home: store_home.into(),
            writer_uid: writer,
            native_owner_id: store_owner.into(),
            account_id_hash: store_account.into(),
        };
        this.recheck(deadline)?;
        let leaf = this.anchors.last().ok_or_else(fail)?;
        if leaf.facts.uid != writer
            || leaf.facts.mode & 0o777 != 0o700
            || !Facts::read(store_directory)?.same_object(&leaf.facts)
        {
            return Err(fail());
        }
        Ok(this)
    }
    pub fn recheck(&self, deadline: Instant) -> Result<(), GatewayError> {
        for (index, anchor) in self.anchors.iter().enumerate() {
            if Instant::now() >= deadline
                || !Facts::read(&anchor.directory)?.same_object(&anchor.facts)
                || anchor.facts.mode & 0o170000 != 0o040000
                || ![0, self.writer_uid].contains(&anchor.facts.uid)
            {
                return Err(fail());
            }
            // A common root-owned sticky directory protects an owner-owned child from OTHER UIDs.
            // This never claims protection from another process with our own writer UID.
            if anchor.facts.mode & 0o022 != 0
                && !(anchor.facts.uid == 0 && anchor.facts.mode & 0o1000 != 0)
            {
                return Err(fail());
            }
            if let Some(name) = &anchor.component {
                let parent = &self.anchors[index - 1];
                let current = statat(&parent.directory, name, AtFlags::SYMLINK_NOFOLLOW)
                    .map_err(|_| fail())?;
                let pinned = fstat(&anchor.directory).map_err(|_| fail())?;
                if current.st_dev != pinned.st_dev
                    || current.st_ino != pinned.st_ino
                    || current.st_mode != pinned.st_mode
                    || current.st_uid != pinned.st_uid
                {
                    return Err(fail());
                }
            }
            self.inspector
                .observe_directory(&anchor.directory, deadline)?;
        }
        let root_now = File::from(
            openat(
                rustix::fs::CWD,
                &self.home,
                OFlags::RDONLY | OFlags::DIRECTORY | OFlags::NOFOLLOW | OFlags::CLOEXEC,
                Mode::empty(),
            )
            .map_err(|_| fail())?,
        );
        if !Facts::read(&root_now)?.same_object(&self.anchors.last().ok_or_else(fail)?.facts) {
            return Err(fail());
        }
        self.inspector.recheck()
    }
    pub fn native_owner_id(&self) -> &str {
        &self.native_owner_id
    }
    pub fn account_id_hash(&self) -> &str {
        &self.account_id_hash
    }
    pub(crate) fn directory(&self) -> Result<&File, GatewayError> {
        Ok(&self.anchors.last().ok_or_else(fail)?.directory)
    }
    pub fn prepare_runtime(self) -> Result<owner_runtime::OwnedRuntime, GatewayError> {
        owner_runtime::OwnedRuntime::prepare(self)
    }
}
pub(crate) fn fail() -> GatewayError {
    GatewayError::unavailable(
        "Exclusive native owner custody is unavailable or changed",
        "native_import_owner_exclusion_unavailable",
    )
}
