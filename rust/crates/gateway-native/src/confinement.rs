//! Mandatory OS policy before serving. Unsupported enforcement fails closed;
//! portable business logic never substitutes for filesystem/network/process denial.
use gateway_core::error::GatewayError;
use std::path::{Path, PathBuf};

pub struct NativeConfinement {
    pub readonly_paths: Vec<PathBuf>,
    pub list_directories: Vec<PathBuf>,
    pub replay_directory: PathBuf,
    pub unix_sockets: Vec<PathBuf>,
}

#[cfg(target_os = "linux")]
pub fn enforce(policy: &NativeConfinement) -> Result<(), GatewayError> {
    if !policy.replay_directory.is_absolute()
        || policy
            .readonly_paths
            .iter()
            .chain(&policy.unix_sockets)
            .any(|path| !path.is_absolute())
    {
        return Err(unavailable());
    }
    restrict_filesystem(
        &policy.readonly_paths,
        &policy.list_directories,
        Some(&policy.replay_directory),
        &policy.unix_sockets,
    )?;
    deny_new_processes()
}
/// One-shot passive probe: no file read/write grant, only the configured Unix endpoint.
#[cfg(target_os = "linux")]
pub fn enforce_readonly_socket(socket: &Path) -> Result<(), GatewayError> {
    if !socket.is_absolute() {
        return Err(unavailable());
    }
    restrict_filesystem(&[], &[], None, &[socket.to_owned()])?;
    deny_new_processes()
}
#[cfg(not(target_os = "linux"))]
pub fn enforce_readonly_socket(_: &Path) -> Result<(), GatewayError> {
    Err(unavailable())
}
#[cfg(target_os = "linux")]
fn restrict_filesystem(
    readonly_paths: &[PathBuf],
    list_directories: &[PathBuf],
    replay_directory: Option<&Path>,
    unix_sockets: &[PathBuf],
) -> Result<(), GatewayError> {
    use landlock::{
        ABI, Access, AccessFs, AccessNet, CompatLevel, Compatible, PathBeneath, PathFd, Ruleset,
        RulesetAttr, RulesetCreatedAttr, RulesetStatus, Scope,
    };
    let abi = ABI::V9;
    let mut rules = Ruleset::default()
        .set_compatibility(CompatLevel::HardRequirement)
        .handle_access(AccessFs::from_all(abi))
        .map_err(|_| unavailable())?
        .handle_access(AccessNet::from_all(abi))
        .map_err(|_| unavailable())?
        .scope(Scope::AbstractUnixSocket | Scope::Signal)
        .map_err(|_| unavailable())?
        .create()
        .map_err(|_| unavailable())?;
    for directory in list_directories {
        if directory.exists() {
            rules = rules
                .add_rule(PathBeneath::new(
                    PathFd::new(directory).map_err(|_| unavailable())?,
                    AccessFs::ReadDir,
                ))
                .map_err(|_| unavailable())?;
        }
    }
    for path in readonly_paths {
        let canonical = std::fs::canonicalize(path).map_err(|_| unavailable())?;
        let metadata = std::fs::metadata(&canonical).map_err(|_| unavailable())?;
        let access = if metadata.is_dir() {
            AccessFs::ReadFile | AccessFs::ReadDir
        } else {
            AccessFs::ReadFile.into()
        };
        rules = rules
            .add_rule(PathBeneath::new(
                PathFd::new(canonical).map_err(|_| unavailable())?,
                access,
            ))
            .map_err(|_| unavailable())?;
    }
    for socket in unix_sockets {
        if socket.exists() {
            rules = rules
                .add_rule(PathBeneath::new(
                    PathFd::new(socket).map_err(|_| unavailable())?,
                    AccessFs::ResolveUnix,
                ))
                .map_err(|_| unavailable())?;
        }
    }
    let writable = AccessFs::ReadFile
        | AccessFs::ReadDir
        | AccessFs::WriteFile
        | AccessFs::MakeReg
        | AccessFs::RemoveFile
        | AccessFs::Truncate;
    if let Some(directory) = replay_directory {
        rules = rules
            .add_rule(PathBeneath::new(
                PathFd::new(directory).map_err(|_| unavailable())?,
                writable,
            ))
            .map_err(|_| unavailable())?;
    }
    let status = rules.restrict_self().map_err(|_| unavailable())?;
    if status.ruleset != RulesetStatus::FullyEnforced || !status.no_new_privs {
        return Err(unavailable());
    }
    Ok(())
}
#[cfg(target_os = "linux")]
fn deny_new_processes() -> Result<(), GatewayError> {
    use seccompiler::{
        BpfProgram, SeccompAction, SeccompCmpArgLen, SeccompCmpOp, SeccompCondition, SeccompFilter,
        SeccompRule,
    };
    use std::collections::BTreeMap;
    let mut denied = BTreeMap::new();
    for syscall in [
        libc::SYS_execve,
        libc::SYS_execveat,
        libc::SYS_ptrace,
        libc::SYS_process_vm_readv,
        libc::SYS_process_vm_writev,
        libc::SYS_mount,
        libc::SYS_umount2,
        libc::SYS_bpf,
        libc::SYS_io_uring_setup,
    ] {
        denied.insert(syscall, Vec::<SeccompRule>::new());
    }
    #[cfg(target_arch = "x86_64")]
    for syscall in [libc::SYS_fork, libc::SYS_vfork] {
        denied.insert(syscall, Vec::new());
    }
    denied.insert(
        libc::SYS_clone,
        vec![
            SeccompRule::new(vec![
                SeccompCondition::new(
                    0,
                    SeccompCmpArgLen::Qword,
                    SeccompCmpOp::MaskedEq(libc::CLONE_THREAD as u64),
                    0,
                )
                .map_err(|_| unavailable())?,
            ])
            .map_err(|_| unavailable())?,
        ],
    );
    // New sockets are Unix-only. The already-bound loopback listener and already
    // connected exact publication TLS socket, if any, retain their existing FDs.
    denied.insert(
        libc::SYS_socket,
        vec![
            SeccompRule::new(vec![
                SeccompCondition::new(
                    0,
                    SeccompCmpArgLen::Dword,
                    SeccompCmpOp::Ne,
                    libc::AF_UNIX as u64,
                )
                .map_err(|_| unavailable())?,
            ])
            .map_err(|_| unavailable())?,
        ],
    );
    let filter: BpfProgram = SeccompFilter::new(
        denied,
        SeccompAction::Allow,
        SeccompAction::Errno(libc::EPERM as u32),
        std::env::consts::ARCH
            .try_into()
            .map_err(|_| unavailable())?,
    )
    .map_err(|_| unavailable())?
    .try_into()
    .map_err(|_| unavailable())?;
    seccompiler::apply_filter_all_threads(&filter).map_err(|_| unavailable())?;
    // libc retries clone3 ENOSYS through clone, whose flags above permit threads
    // while refusing a new process. EPERM would also prevent legitimate workers.
    let clone3: BpfProgram = SeccompFilter::new(
        BTreeMap::from([(libc::SYS_clone3, Vec::new())]),
        SeccompAction::Allow,
        SeccompAction::Errno(libc::ENOSYS as u32),
        std::env::consts::ARCH
            .try_into()
            .map_err(|_| unavailable())?,
    )
    .map_err(|_| unavailable())?
    .try_into()
    .map_err(|_| unavailable())?;
    seccompiler::apply_filter_all_threads(&clone3).map_err(|_| unavailable())?;
    Ok(())
}
#[cfg(not(target_os = "linux"))]
pub fn enforce(_: &NativeConfinement) -> Result<(), GatewayError> {
    Err(unavailable())
}
fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Required native OS confinement is unavailable",
        "native_confinement_unavailable",
    )
}
