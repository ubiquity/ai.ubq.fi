use crate::{endpoints::ResolvedImportEndpoints, pin::PinnedExecutable};
use gateway_native_custody_io::protocol::unavailable;
use gateway_native_custody_wire::bootstrap::Bootstrap;
use gateway_source::error::GatewayError;
use std::{
    io::Write,
    net::TcpStream,
    os::fd::OwnedFd,
    path::{Path, PathBuf},
    process::{Child, ChildStdin, Command, Stdio},
    time::{Duration, Instant},
};

pub struct VerifiedImportLaunch<'cfg> {
    config: &'cfg [u8],
    actor: PinnedExecutable,
    sandbox: PinnedExecutable,
    owner_root: PathBuf,
    replay_directory: PathBuf,
    endpoints: gateway_native_custody_wire::bootstrap::ImportEndpointData,
    native_owner_id: String,
    account_id_hash: String,
}
pub struct PreparedImportSpawn<'cfg> {
    launch: VerifiedImportLaunch<'cfg>,
    client: TcpStream,
    actor_socket: TcpStream,
    boot: Bootstrap,
    profile: String,
}
pub struct RunningImportProcess<'cfg> {
    child: Option<Child>,
    input: Option<ChildStdin>,
    deadline: Instant,
    config: &'cfg [u8],
    boot: Bootstrap,
    attempted: bool,
}
impl<'cfg> VerifiedImportLaunch<'cfg> {
    pub(crate) fn prepare(
        artifact: (&Path, &str),
        daemon_config: &'cfg [u8],
        owner_root: &Path,
        replay_directory: &Path,
        endpoints: &ResolvedImportEndpoints,
        registered_owner: (&str, &str),
    ) -> Result<Self, GatewayError> {
        let (binary, digest) = artifact;
        let (native_owner_id, account_id_hash) = registered_owner;
        if !cfg!(target_os = "macos") || daemon_config.is_empty() || daemon_config.len() > 65_536 {
            return Err(unavailable());
        }
        let config = gateway_native_daemon_config::config::NativeDaemonConfig::read_import_stdin(
            daemon_config,
        )?;
        let registry = config.owner_renewal.as_ref().ok_or_else(unavailable)?;
        if config.nonce_database.parent() != Some(replay_directory) {
            return Err(unavailable());
        }
        if !registry
            .owners()
            .iter()
            .any(|owner| owner.owner_id() == native_owner_id && owner.codex_home() == owner_root)
        {
            return Err(unavailable());
        }
        endpoints.require_origin(registry.authority_origin())?;
        let actor = PinnedExecutable::verify(binary, digest)?;
        let sandbox = crate::sandbox_identity::sandbox_executable()?;
        Ok(Self {
            config: daemon_config,
            actor,
            sandbox,
            owner_root: owner_root.into(),
            replay_directory: replay_directory.into(),
            endpoints: endpoints.data()?,
            native_owner_id: native_owner_id.into(),
            account_id_hash: account_id_hash.into(),
        })
    }
    pub fn prepare_channel(
        self,
        client: TcpStream,
        actor_socket: TcpStream,
        session_ref: String,
    ) -> Result<(PreparedImportSpawn<'cfg>, Bootstrap), GatewayError> {
        let boot = Bootstrap {
            v: 1,
            session_ref,
            client_local: client.local_addr().map_err(|_| unavailable())?,
            client_peer: client.peer_addr().map_err(|_| unavailable())?,
            custody_local: actor_socket.local_addr().map_err(|_| unavailable())?,
            custody_peer: actor_socket.peer_addr().map_err(|_| unavailable())?,
            endpoints: self.endpoints,
            native_owner_id: self.native_owner_id.clone(),
            account_id_hash: self.account_id_hash.clone(),
        };
        boot.validate()?;
        let profile = gateway_native_connector::connector_policy::import_actor(
            self.actor.path(),
            &self.owner_root,
            &self.replay_directory,
            (boot.client_local, boot.client_peer),
            (boot.custody_local, boot.custody_peer),
            &boot.endpoints.addresses(),
        )
        .map_err(|_| unavailable())?;
        #[cfg(test)]
        for ancestor in std::fs::canonicalize(&self.owner_root)
            .map_err(|_| unavailable())?
            .ancestors()
            .skip(1)
        {
            let literal = serde_json::to_string(ancestor.to_str().ok_or_else(unavailable)?)
                .map_err(|_| unavailable())?;
            assert!(
                profile
                    .lines()
                    .any(|line| line == format!("(allow file-read-data (literal {literal}))")),
                "actual import profile must grant only the reached literal ancestor directory read"
            );
            assert!(
                !profile.contains(&format!("(allow file-read* (subpath {literal}))")),
                "actual import profile must not widen ancestor subtree reads"
            );
            assert!(
                !profile.contains(&format!("(allow file-write* (subpath {literal}))")),
                "actual import profile must not grant ancestor subtree writes"
            );
        }
        client.set_nonblocking(false).map_err(|_| unavailable())?;
        TcpStream::set_nonblocking(&actor_socket, false).map_err(|_| unavailable())?;
        if rustix::fs::fcntl_getfl(&client)
            .map_err(|_| unavailable())?
            .contains(rustix::fs::OFlags::NONBLOCK)
        {
            return Err(unavailable());
        }
        let observation = copy_bootstrap(&boot);
        Ok((
            PreparedImportSpawn {
                launch: self,
                client,
                actor_socket,
                boot,
                profile,
            },
            observation,
        ))
    }
}
impl<'cfg> PreparedImportSpawn<'cfg> {
    pub fn spawn(self) -> Result<RunningImportProcess<'cfg>, GatewayError> {
        self.launch.actor.recheck()?;
        self.launch.sandbox.recheck()?;
        let client: OwnedFd = self.client.into();
        let control: OwnedFd = self.actor_socket.into();
        let mut child = Command::new(self.launch.sandbox.path())
            .args(["-p", &self.profile])
            .arg(self.launch.actor.path())
            .env_clear()
            .current_dir(self.launch.actor.path().parent().ok_or_else(unavailable)?)
            .stdin(Stdio::piped())
            .stdout(Stdio::from(client))
            .stderr(Stdio::from(control))
            .spawn()
            .map_err(|_| unavailable())?;
        let input = child.stdin.take().ok_or_else(unavailable)?;
        Ok(RunningImportProcess {
            child: Some(child),
            input: Some(input),
            deadline: Instant::now() + Duration::from_secs(50),
            config: self.launch.config,
            boot: self.boot,
            attempted: false,
        })
    }
}
fn copy_bootstrap(boot: &Bootstrap) -> Bootstrap {
    Bootstrap {
        v: boot.v,
        session_ref: boot.session_ref.clone(),
        client_local: boot.client_local,
        client_peer: boot.client_peer,
        custody_local: boot.custody_local,
        custody_peer: boot.custody_peer,
        native_owner_id: boot.native_owner_id.clone(),
        account_id_hash: boot.account_id_hash.clone(),
        endpoints: gateway_native_custody_wire::bootstrap::ImportEndpointData {
            authority: boot.endpoints.authority,
            models: boot.endpoints.models,
            oauth: boot.endpoints.oauth,
        },
    }
}
impl RunningImportProcess<'_> {
    pub fn bootstrap(&mut self) -> Result<(), GatewayError> {
        if self.attempted {
            return Err(unavailable());
        }
        self.attempted = true;
        let bootstrap = serde_json::to_vec(&self.boot).map_err(|_| unavailable())?;
        if bootstrap.len() > gateway_native_custody_wire::bootstrap_bound::bootstrap_max_bytes()? {
            return Err(unavailable());
        }
        let input = self.input.as_mut().ok_or_else(unavailable)?;
        for bytes in [self.config, bootstrap.as_slice()] {
            let length = u32::try_from(bytes.len()).map_err(|_| unavailable())?;
            Write::write_all(input, &length.to_be_bytes()).map_err(|_| unavailable())?;
            input.write_all(bytes).map_err(|_| unavailable())?;
        }
        Ok(())
    }
    pub fn deadline_data(&self) -> Instant {
        self.deadline
    }
    pub fn check_running(&mut self) -> Result<(), GatewayError> {
        if Instant::now() >= self.deadline
            || self.input.is_none()
            || self
                .child
                .as_mut()
                .ok_or_else(unavailable)?
                .try_wait()
                .map_err(|_| unavailable())?
                .is_some()
        {
            return Err(unavailable());
        }
        Ok(())
    }
    pub fn wait_completed(&mut self) -> Result<(), GatewayError> {
        loop {
            let status = self
                .child
                .as_mut()
                .ok_or_else(unavailable)?
                .try_wait()
                .map_err(|_| unavailable())?;
            if let Some(status) = status {
                if !status.success() {
                    return Err(unavailable());
                }
                self.child.take();
                self.input.take();
                return Ok(());
            }
            if Instant::now() >= self.deadline {
                return Err(unavailable());
            }
            std::thread::sleep(Duration::from_millis(5));
        }
    }
    pub fn close_parent_input(&mut self) {
        self.input.take();
    }
    pub fn abort_and_reap(&mut self) {
        if let Some(mut child) = self.child.take() {
            if child.try_wait().ok().flatten().is_none() {
                let _ = child.kill();
            }
            let _ = child.wait();
        }
    }
}
impl Drop for RunningImportProcess<'_> {
    fn drop(&mut self) {
        self.close_parent_input();
        self.abort_and_reap();
    }
}
