//! Kernel/actual Child/epoch only. This module never opens or decodes credential documents.
use crate::{
    actor_channel::{ImportChildCommand, OwnedImportChildChannel},
    owner_runtime::OwnedRuntime,
};
use gateway_authority::native_control::import::{
    custody_session::VerifiedCurrentImportStarted, remote::VerifiedCurrentImportTerminal,
};
use gateway_native_custody_io::protocol::unavailable;
use gateway_native_custody_wire::session_policy::{CustodySessionData, TerminalObservationData};
use gateway_native_custody_wire::wire::Header;
use gateway_source::error::GatewayError;
use std::time::{Duration, Instant};

pub(crate) struct KernelSnapshot {
    pub(crate) epoch: String,
    pub(crate) root_device: u64,
    pub(crate) root_inode: u64,
}
struct Session {
    data: CustodySessionData,
    snapshot: Option<KernelSnapshot>,
    deadline: Instant,
}

/// Not a serializable lease or a renamed credential-bearing owner-settlement API.
/// Its private constructor accepts actual already-owned Child custody, never a PID/Boolean.
/// ```compile_fail
/// use gateway_native_custody_engine::custodian_runtime::CustodianRuntime;
/// let forged: CustodianRuntime = serde_json::from_str("{}").unwrap();
/// ```
pub struct CustodianRuntime {
    runtime: OwnedRuntime,
    session: Option<Session>,
}
impl CustodianRuntime {
    pub(crate) fn from_owned(runtime: OwnedRuntime) -> Result<Self, GatewayError> {
        runtime.require_owned_child()?;
        Ok(Self {
            runtime,
            session: None,
        })
    }
    pub(crate) fn open_import_session(
        &mut self,
        peer: &mut OwnedImportChildChannel,
        command: &ImportChildCommand,
        fresh: VerifiedCurrentImportStarted,
    ) -> Result<(), GatewayError> {
        if self.session.is_some() {
            return Err(unavailable());
        }
        let request = peer.consume_first_quiesce(command)?;
        self.match_observation(&request, &fresh)?;
        let now = gateway_auth_native_proof::native_context::now_ms()?;
        let remaining = fresh
            .deadline_ms()
            .checked_sub(now)
            .filter(|ms| *ms > 0 && *ms <= gateway_auth_native_wire::control::NATIVE_RENEWAL_MAX_MS)
            .ok_or_else(unavailable)?;
        let deadline = peer
            .deadline()
            .min(Instant::now() + Duration::from_millis(remaining as u64));
        // Install the session before every possible kernel effect, as originally.
        self.session = Some(Session {
            data: CustodySessionData::bind(request)?,
            snapshot: None,
            deadline,
        });
        self.runtime.custody_stop(deadline)?;
        peer.check_live()?;
        let snapshot = self.runtime.custody_snapshot(deadline)?;
        let session = self.session.as_mut().ok_or_else(unavailable)?;
        session.snapshot = Some(snapshot);
        session.data.mark_stopped_data();
        Ok(())
    }
    fn match_observation(
        &self,
        request: &Header,
        fresh: &VerifiedCurrentImportStarted,
    ) -> Result<(), GatewayError> {
        if fresh.request().canonical_bytes()? != request.binding.canonical_bytes()?
            || !self.runtime.matches_custody_owner(
                &request.binding.native_owner_id,
                &request.binding.account_id_hash,
            )
            || fresh.verified_at_ms() > gateway_auth_native_proof::native_context::now_ms()?
            || fresh.deadline_ms() <= gateway_auth_native_proof::native_context::now_ms()?
        {
            return Err(unavailable());
        }
        Ok(())
    }
    pub(crate) fn recheck_import_session(
        &mut self,
        peer: &mut OwnedImportChildChannel,
        command: &ImportChildCommand,
        fresh: VerifiedCurrentImportStarted,
    ) -> Result<KernelSnapshot, GatewayError> {
        peer.require_pending(command)?;
        let header = command.data.header();
        let session = self.session.as_ref().ok_or_else(unavailable)?;
        session.data.require_recheck_data(header, fresh.request())?;
        if fresh.deadline_ms() <= gateway_auth_native_proof::native_context::now_ms()? {
            return Err(unavailable());
        }
        peer.check_live()?;
        let snapshot = self.runtime.custody_snapshot(session.deadline)?;
        same_snapshot(
            session.snapshot.as_ref().ok_or_else(unavailable)?,
            &snapshot,
        )?;
        Ok(snapshot)
    }
    /// Consume a fresh CURRENT terminal only for this SAME already-stopped
    /// actual Child/channel session. Historical Started provenance stays here.
    pub(crate) fn release_import_session(
        &mut self,
        peer: &mut OwnedImportChildChannel,
        command: &ImportChildCommand,
        fresh: VerifiedCurrentImportTerminal,
    ) -> Result<(), GatewayError> {
        peer.require_pending(command)?;
        let header = command.data.header();
        let session = self.session.as_ref().ok_or_else(unavailable)?;
        let now = gateway_auth_native_proof::native_context::now_ms()?;
        session.data.require_stopped_data()?;
        if Instant::now() >= session.deadline {
            return Err(unavailable());
        }
        session.data.require_release_after_deadline_data(
            header,
            TerminalObservationData::observe(&fresh),
            command.data.publication_revision(),
            now,
        )?;
        let snapshot = self.runtime.custody_snapshot(session.deadline)?;
        same_snapshot(
            session.snapshot.as_ref().ok_or_else(unavailable)?,
            &snapshot,
        )?;
        peer.check_live()?;
        // Consume before the original reply IO; never rebuild after a lost reply.
        self.session
            .as_mut()
            .ok_or_else(unavailable)?
            .data
            .mark_released_data();
        peer.released(command)
    }
}
fn same_snapshot(expected: &KernelSnapshot, actual: &KernelSnapshot) -> Result<(), GatewayError> {
    gateway_native_custody_wire::identity_policy::same_identity_data(
        (&expected.epoch, expected.root_device, expected.root_inode),
        (&actual.epoch, actual.root_device, actual.root_inode),
    )
}
impl OwnedRuntime {
    pub fn into_custodian(self) -> Result<CustodianRuntime, GatewayError> {
        CustodianRuntime::from_owned(self)
    }
}

impl CustodianRuntime {
    pub fn serve_import(
        &mut self,
        client: std::net::TcpStream,
        config: &[u8],
    ) -> Result<(), GatewayError> {
        let plan = gateway_native_import_tools::service::FixedCustodianService::prepare_production(
            config,
        )?;
        self.serve_fixed(client, config, plan)
    }
    #[cfg(feature = "native-import-fixture")]
    pub fn serve_fixture(
        &mut self,
        client: std::net::TcpStream,
        config: &[u8],
        plan: gateway_native_import_tools::fixed_fixture::FixedCustodianFixturePlan,
    ) -> Result<(), GatewayError> {
        let service = plan.prepare_service()?;
        self.serve_fixed(
            client,
            config,
            gateway_native_import_tools::service::FixedCustodianService::Fixture(service),
        )
    }
    fn serve_fixed(
        &mut self,
        client: std::net::TcpStream,
        config: &[u8],
        service: gateway_native_import_tools::service::FixedCustodianService,
    ) -> Result<(), GatewayError> {
        let prepared =
            gateway_native_daemon_config::config::NativeDaemonConfig::read_import_stdin(config)?;
        let connection = service.connect_custody_authority(&prepared)?;
        let (owner, account, root) = self.runtime.custody_registry()?;
        let replay = prepared.nonce_database.parent().ok_or_else(unavailable)?;
        let launch = service.prepare_launch(config, root, replay, (owner, account))?;
        let listener = std::net::TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, 0))
            .map_err(|_| unavailable())?;
        let actor_socket =
            std::net::TcpStream::connect(listener.local_addr().map_err(|_| unavailable())?)
                .map_err(|_| unavailable())?;
        let (socket, _) = listener.accept().map_err(|_| unavailable())?;
        drop(listener);
        let (spawn, boot) = launch.prepare_channel(
            client,
            actor_socket,
            gateway_native_custody_io::protocol::random_ref()?,
        )?;
        let peer = crate::actor_channel::OwnedImportChildChannel::bind_spawn(spawn, socket, boot)?;
        crate::custodian_service::serve(self, peer, connection, prepared.identity)
    }
}
