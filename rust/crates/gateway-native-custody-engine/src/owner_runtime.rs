//! Owned-process custody only. Dirty state never becomes another import start permit.
use crate::exclusive_owner::{ExclusiveManagedOwner, fail};
use gateway_authority::native_control::import::AuthenticatedNativeImport;
use gateway_authority::native_control::import::effects::NativeImportStart;
use gateway_authority::native_control::receiver::NativeRenewalAuthority;
use gateway_authority_contracts::NativeRenewalCancellation;
use gateway_authority_contracts::TokenSource;
use gateway_authority_native_records::import_effects::NativeImportBinding;
use gateway_core::state::AuthorityStore;
use gateway_native_confinement::file_observation::Facts;
use gateway_native_custody_wire::epoch::{EPOCH_LIMIT, Epoch, Phase};
use gateway_native_custody_wire::owner_file_policy::{
    require_epoch_file_shape, require_generation_file, require_prepared_file,
    require_stopped_auth_file,
};
use gateway_native_http_admission::native_import::NativeImportContext;
use gateway_native_owned_writer::{FixedWriterPlan, OwnedWriterChild, UnlaunchedCodex};
use gateway_source::error::GatewayError;
use rustix::fs::{AtFlags, FlockOperation, Mode, OFlags, flock, fsync, openat, renameat, statat};
#[cfg(all(feature = "native-import-fixture", target_os = "macos"))]
use std::process::{Command, Stdio};
use std::{
    fs::File,
    io::{Read, Write},
    path::Path,
    time::{Duration, Instant},
};
const EPOCH: &str = ".uos-owner-runtime";
const EPOCH_TEMP: &str = ".uos-owner-runtime.next";
/// Actual private process custody; a JSON marker/available flock does not mint stopped-child proof.
pub struct OwnedRuntime {
    owner: ExclusiveManagedOwner,
    lock: File,
    epoch: Option<Epoch>,
    child: Option<OwnedWriterChild>,
    settled: bool,
}
/// An original-start-bound settled OWNED writer. No mutation API is exposed by this foundation.
/// Bootstrap exclusivity/full credential importer/publication remain separate acceptance.
/// ```compile_fail
/// use gateway_native_custody_engine::owner_runtime::ExclusiveOwnerLease;
/// let forged: ExclusiveOwnerLease<'_> = serde_json::from_str("{}").unwrap();
/// ```
pub struct ExclusiveOwnerLease<'a> {
    runtime: &'a mut OwnedRuntime,
    binding: NativeImportBinding,
    original: &'a AuthenticatedNativeImport,
}
impl ExclusiveOwnerLease<'_> {
    pub fn binding(&self) -> &NativeImportBinding {
        &self.binding
    }
    pub async fn recheck<S: AuthorityStore, I: TokenSource, C: NativeRenewalCancellation>(
        &self,
        authority: &NativeRenewalAuthority<'_, S, I, C>,
        original_body: &[u8],
        context: &NativeImportContext<'_>,
        deadline: Instant,
    ) -> Result<(), GatewayError> {
        context.check_document(self.original, self.original.metadata(), original_body)?;
        context.check_current(authority).await?;
        self.original
            .require_body(original_body, authority.now_ms()?)?;
        if !self.runtime.settled || self.runtime.child.is_some() {
            return Err(fail());
        }
        self.runtime.owner.recheck(deadline)?;
        self.runtime.check_epoch(Phase::Stopped)?;
        self.runtime.check_generation(&self.binding)?;
        context.check_current(authority).await?;
        context.check_document(self.original, self.original.metadata(), original_body)
    }
}
impl OwnedRuntime {
    pub(crate) fn prepare(owner: ExclusiveManagedOwner) -> Result<Self, GatewayError> {
        // Readonly preparation. Initial empty/missing runtime history establishes NO daemon absence.
        let directory = owner.directory()?;
        let existing = read_epoch(directory)?;
        if existing.as_ref().is_some_and(|e| e.phase != Phase::Stopped) {
            return Err(fail());
        }
        let lock = File::from(
            openat(
                directory,
                ".uos-owner.lock",
                OFlags::RDONLY | OFlags::NOFOLLOW | OFlags::CLOEXEC | OFlags::NONBLOCK,
                Mode::empty(),
            )
            .map_err(|_| fail())?,
        );
        let facts = Facts::read(&lock)?;
        require_prepared_file(&facts, owner.writer_uid)?;
        let auth = File::from(
            openat(
                directory,
                "auth.json",
                OFlags::RDONLY | OFlags::NOFOLLOW | OFlags::CLOEXEC | OFlags::NONBLOCK,
                Mode::empty(),
            )
            .map_err(|_| fail())?,
        );
        let auth_facts = Facts::read(&auth)?;
        require_prepared_file(&auth_facts, owner.writer_uid)?;
        // A PREPARED stable lock inode coordinates trusted actors; auth.json is replaced by import.
        // Missing pre-existing lock/custody is unavailable, never created before the original start.
        flock(&lock, FlockOperation::NonBlockingLockExclusive).map_err(|_| fail())?;
        let this = Self {
            owner,
            lock,
            epoch: existing,
            child: None,
            settled: false,
        };
        if let Some(epoch) = &this.epoch {
            this.match_epoch(epoch)?;
        }
        Ok(this)
    }
    fn match_epoch(&self, epoch: &Epoch) -> Result<(), GatewayError> {
        let facts = Facts::read(self.owner.directory()?)?;
        gateway_native_custody_wire::epoch::require_identity(
            epoch,
            &self.owner.native_owner_id,
            &self.owner.account_id_hash,
            facts,
        )?;
        Ok(())
    }
    fn check_epoch(&self, phase: Phase) -> Result<(), GatewayError> {
        let pinned = rustix::fs::fstat(&self.lock).map_err(|_| fail())?;
        let current = statat(
            self.owner.directory()?,
            ".uos-owner.lock",
            AtFlags::SYMLINK_NOFOLLOW,
        )
        .map_err(|_| fail())?;
        if pinned.st_dev != current.st_dev
            || pinned.st_ino != current.st_ino
            || pinned.st_mode != current.st_mode
            || pinned.st_uid != current.st_uid
        {
            return Err(fail());
        }
        let epoch = read_epoch(self.owner.directory()?)?.ok_or_else(fail)?;
        self.match_epoch(&epoch)?;
        if epoch.phase != phase || self.epoch.as_ref().is_none_or(|e| e.boot != epoch.boot) {
            return Err(fail());
        }
        Ok(())
    }
    fn write_epoch(&mut self, phase: Phase) -> Result<(), GatewayError> {
        let directory = self.owner.directory()?;
        let epoch = self.epoch.as_mut().ok_or_else(fail)?;
        epoch.phase = phase;
        let bytes = gateway_native_custody_wire::epoch::encode(epoch)?;
        let mut file = File::from(
            openat(
                directory,
                EPOCH_TEMP,
                OFlags::WRONLY | OFlags::CREATE | OFlags::EXCL | OFlags::NOFOLLOW | OFlags::CLOEXEC,
                Mode::RUSR | Mode::WUSR,
            )
            .map_err(|_| fail())?,
        );
        file.write_all(&bytes).map_err(|_| fail())?;
        file.sync_all().map_err(|_| fail())?;
        renameat(directory, EPOCH_TEMP, directory, EPOCH).map_err(|_| fail())?;
        fsync(directory).map_err(|_| fail())?;
        self.check_epoch(phase)
    }
    /// A prepared root with unknown preexisting daemon custody cannot be promoted by this method.
    /// Actual bootstrap/owned launcher must establish its private initial custody before production use.
    pub fn launch_codex(
        &mut self,
        executable: &Path,
        deadline: Instant,
    ) -> Result<(), GatewayError> {
        let binary = UnlaunchedCodex::verify(executable)?;
        self.owner.recheck(deadline)?;
        // No operator/bootstrap ownership port exists yet. A marker or process census is not that port.
        let _ = binary;
        Err(fail())
    }
    #[cfg(all(feature = "native-import-fixture", target_os = "macos"))]
    pub fn start_owned_writer_fixture(&mut self, deadline: Instant) -> Result<(), GatewayError> {
        if self.child.is_some() || self.epoch.is_some() || Instant::now() >= deadline {
            return Err(fail());
        }
        self.owner.recheck(deadline)?;
        let launch = FixedWriterPlan::prepare_owned_fixture(&self.owner.home)?;
        let mut random = [0u8; 32];
        getrandom::fill(&mut random).map_err(|_| fail())?;
        let root = Facts::read(self.owner.directory()?)?;
        self.epoch = Some(Epoch {
            v: 1,
            native_owner_id: self.owner.native_owner_id.clone(),
            account_id_hash: self.owner.account_id_hash.clone(),
            root_device: root.device,
            root_inode: root.inode,
            boot: gateway_auth_primitives::crypto::sha256_hex(&random),
            phase: Phase::Running,
        });
        // Dirty BEFORE spawn; even failed spawn/reap cannot silently create a clean restart.
        self.write_epoch(Phase::Running)?;
        let launch = launch.recheck()?;
        let errors = File::from(
            openat(
                self.owner.directory()?,
                ".uos-owner-fixture.stderr",
                OFlags::WRONLY | OFlags::CREATE | OFlags::EXCL | OFlags::NOFOLLOW | OFlags::CLOEXEC,
                Mode::RUSR | Mode::WUSR,
            )
            .map_err(|_| fail())?,
        );
        self.child = Some(launch.spawn_fixed(errors)?);
        self.settled = false;
        Ok(())
    }
    fn stop_kernel(&mut self, deadline: Instant) -> Result<(), GatewayError> {
        self.check_epoch(Phase::Running)?;
        // Dirty intent is durable before any possible kill; uncertainty cannot reopen custody.
        self.write_epoch(Phase::Stopping)?;
        let child = self.child.as_mut().ok_or_else(fail)?;
        child.stop_and_reap(deadline)?;
        // The actual retained Child was reaped, not an arbitrary PID or socket closed.
        self.child.take();
        self.owner.recheck(deadline)?;
        fsync(self.owner.directory()?).map_err(|_| fail())?;
        self.write_epoch(Phase::Stopped)?;
        self.settled = true;
        Ok(())
    }
    fn stop_owned(&mut self, deadline: Instant) -> Result<(), GatewayError> {
        self.stop_kernel(deadline)?;
        let auth = File::from(
            openat(
                self.owner.directory()?,
                "auth.json",
                OFlags::RDONLY | OFlags::NOFOLLOW | OFlags::CLOEXEC | OFlags::NONBLOCK,
                Mode::empty(),
            )
            .map_err(|_| fail())?,
        );
        let facts = Facts::read(&auth)?;
        require_stopped_auth_file(&facts, self.owner.writer_uid)?;
        Ok(())
    }
    pub async fn settle_for_import<
        'a,
        S: AuthorityStore,
        I: TokenSource,
        C: NativeRenewalCancellation,
    >(
        &'a mut self,
        authority: &NativeRenewalAuthority<'_, S, I, C>,
        start: NativeImportStart<'a>,
        proof: &'a AuthenticatedNativeImport,
        body: &[u8],
        context: &NativeImportContext<'_>,
        deadline: Instant,
    ) -> Result<ExclusiveOwnerLease<'a>, GatewayError> {
        context.check_document(proof, proof.metadata(), body)?;
        context.check_current(authority).await?;
        start.require_body(body, authority.now_ms()?)?;
        if proof.metadata().operation_id != start.binding().operation_id
            || proof.metadata().action_ref != start.binding().action_ref
            || self.owner.native_owner_id != start.binding().native_owner_id
            || self.owner.account_id_hash != start.binding().account_id_hash
        {
            return Err(fail());
        }
        context.check_document(proof, proof.metadata(), body)?;
        self.stop_owned(deadline)?;
        self.check_generation(start.binding())?;
        context.check_current(authority).await?;
        context.check_document(proof, proof.metadata(), body)?;
        Ok(ExclusiveOwnerLease {
            runtime: self,
            binding: start.binding().clone(),
            original: proof,
        })
    }
    fn check_generation(&self, binding: &NativeImportBinding) -> Result<(), GatewayError> {
        let file = File::from(
            openat(
                self.owner.directory()?,
                "auth.json",
                OFlags::RDONLY | OFlags::NOFOLLOW | OFlags::CLOEXEC | OFlags::NONBLOCK,
                Mode::empty(),
            )
            .map_err(|_| fail())?,
        );
        let before = Facts::read(&file)?;
        require_generation_file(&before, self.owner.writer_uid)?;
        let mut bytes = Vec::new();
        file.try_clone()
            .map_err(|_| fail())?
            .take(1_048_577)
            .read_to_end(&mut bytes)
            .map_err(|_| fail())?;
        if bytes.len() > 1_048_576 || Facts::read(&file)? != before {
            return Err(fail());
        }
        gateway_native_custody_wire::generation_policy::require_managed_generation_data(
            &bytes,
            (&binding.account_id_hash, &binding.source_native_generation),
        )
    }

    // Kernel/actual-Child adapter only. These methods never read credential bytes or generations.
    pub(crate) fn require_owned_child(&self) -> Result<(), GatewayError> {
        if self.settled || self.child.is_none() {
            return Err(fail());
        }
        self.check_epoch(Phase::Running)
    }
    pub(crate) fn custody_stop(&mut self, deadline: Instant) -> Result<(), GatewayError> {
        self.require_owned_child()?;
        self.stop_kernel(deadline)
    }
    pub(crate) fn custody_snapshot(
        &self,
        deadline: Instant,
    ) -> Result<crate::custodian_runtime::KernelSnapshot, GatewayError> {
        if !self.settled || self.child.is_some() {
            return Err(fail());
        }
        self.owner.recheck(deadline)?;
        self.check_epoch(Phase::Stopped)?;
        let epoch = self.epoch.as_ref().ok_or_else(fail)?;
        Ok(crate::custodian_runtime::KernelSnapshot {
            epoch: epoch.boot.clone(),
            root_device: epoch.root_device,
            root_inode: epoch.root_inode,
        })
    }
    pub(crate) fn custody_registry(&self) -> Result<(&str, &str, &Path), GatewayError> {
        self.require_owned_child()?;
        Ok((
            &self.owner.native_owner_id,
            &self.owner.account_id_hash,
            &self.owner.home,
        ))
    }
    pub(crate) fn matches_custody_owner(&self, owner: &str, account: &str) -> bool {
        self.owner.native_owner_id == owner && self.owner.account_id_hash == account
    }
    /// Runtime history is an observation; stopped disk DATA without this live Child never issues a lease.
    pub fn has_settled_owned_writer(&self) -> bool {
        self.settled && self.child.is_none()
    }
}
impl Drop for OwnedRuntime {
    fn drop(&mut self) {
        // Drop cannot classify cancellation as stopped or erase the dirty marker. Best-effort
        // cleanup only touches the actual retained owned Child; no replacement/start/rollback.
        if let Some(child) = &mut self.child {
            child.abort_and_reap();
        }
        let _ = &self.lock;
    }
}
fn read_epoch(directory: &File) -> Result<Option<Epoch>, GatewayError> {
    match statat(directory, EPOCH, AtFlags::SYMLINK_NOFOLLOW) {
        Err(rustix::io::Errno::NOENT) => return Ok(None),
        Err(_) => return Err(fail()),
        Ok(_) => (),
    }
    let file = File::from(
        openat(
            directory,
            EPOCH,
            OFlags::RDONLY | OFlags::NOFOLLOW | OFlags::CLOEXEC | OFlags::NONBLOCK,
            Mode::empty(),
        )
        .map_err(|_| fail())?,
    );
    let facts = Facts::read(&file)?;
    require_epoch_file_shape(&facts)?;
    if facts.uid != Facts::read(directory)?.uid {
        return Err(fail());
    }
    let mut bytes = Vec::new();
    file.take(EPOCH_LIMIT + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| fail())?;
    gateway_native_custody_wire::epoch::decode(&bytes)
}

impl OwnedRuntime {
    #[cfg(all(feature = "native-import-fixture", target_os = "macos"))]
    pub fn stop_owned_writer_fixture(&mut self, deadline: Instant) -> Result<(), GatewayError> {
        self.stop_owned(deadline)
    }
}
