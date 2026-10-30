use crate::custodian_runtime::KernelSnapshot;
use gateway_native_custody_io::protocol::{self as wire, unavailable};
use gateway_native_custody_wire::peer_policy::{
    CustodyCommandData, CustodyPeerData, released_reply, stopped_reply, unknown_reply,
};
use gateway_native_custody_wire::{
    bootstrap::Bootstrap,
    wire::{Frame, Header},
};
use gateway_native_import_tools::{
    endpoints::ResolvedImportEndpoints,
    launch::{RunningImportProcess, VerifiedImportLaunch},
};
use gateway_source::error::GatewayError;
use std::{
    net::{TcpListener, TcpStream},
    path::Path,
    time::Instant,
};

pub(crate) struct ImportChildCommand {
    pub(crate) data: CustodyCommandData,
}

pub struct OwnedImportChildChannel<'cfg> {
    process: RunningImportProcess<'cfg>,
    socket: TcpStream,
    boot: Bootstrap,
    policy: CustodyPeerData,
}
impl<'cfg> OwnedImportChildChannel<'cfg> {
    pub(crate) fn deadline(&self) -> Instant {
        self.process.deadline_data()
    }
    pub(crate) fn settle_completed(&mut self) -> Result<(), GatewayError> {
        self.process.wait_completed()
    }
    pub(crate) fn check_live(&mut self) -> Result<(), GatewayError> {
        self.process.check_running()?;
        if self.socket.local_addr().map_err(|_| unavailable())? != self.boot.custody_peer
            || self.socket.peer_addr().map_err(|_| unavailable())? != self.boot.custody_local
        {
            return Err(unavailable());
        }
        Ok(())
    }
    pub(crate) fn receive(&mut self) -> Result<ImportChildCommand, GatewayError> {
        self.check_live()?;
        self.policy.require_idle()?;
        let deadline = self.deadline();
        let frame = wire::receive(&mut self.socket, deadline, None)?;
        self.check_live()?;
        let data = self.policy.accept_received(frame)?;
        Ok(ImportChildCommand { data })
    }
    pub(crate) fn require_pending(
        &mut self,
        command: &ImportChildCommand,
    ) -> Result<(), GatewayError> {
        self.check_live()?;
        self.policy.require_pending(&command.data)
    }
    pub(crate) fn consume_first_quiesce(
        &mut self,
        command: &ImportChildCommand,
    ) -> Result<Header, GatewayError> {
        self.require_pending(command)?;
        self.policy.consume_first(&command.data)
    }
    pub(crate) fn stopped(
        &mut self,
        command: &ImportChildCommand,
        snapshot: KernelSnapshot,
    ) -> Result<(), GatewayError> {
        self.require_pending(command)?;
        let response = stopped_reply(
            command.data.header(),
            (snapshot.epoch, snapshot.root_device, snapshot.root_inode),
        )?;
        self.reply(command, response)
    }
    fn reply(&mut self, command: &ImportChildCommand, response: Frame) -> Result<(), GatewayError> {
        self.require_pending(command)?;
        let deadline = self.deadline();
        wire::send(&mut self.socket, &response, deadline, None)?;
        self.check_live()?;
        self.policy.commit_reply()
    }
    pub(crate) fn released(&mut self, command: &ImportChildCommand) -> Result<(), GatewayError> {
        let response = released_reply(&command.data)?;
        self.reply(command, response)
    }
    pub(crate) fn refuse(&mut self, command: &ImportChildCommand) -> Result<(), GatewayError> {
        self.reply(command, unknown_reply(command.data.header())?)
    }
}
impl Drop for OwnedImportChildChannel<'_> {
    fn drop(&mut self) {
        self.process.close_parent_input();
        let _ = self.socket.shutdown(std::net::Shutdown::Both);
        self.process.abort_and_reap();
    }
}

impl<'cfg> OwnedImportChildChannel<'cfg> {
    pub(crate) fn bind_spawn(
        spawn: gateway_native_import_tools::launch::PreparedImportSpawn<'cfg>,
        socket: TcpStream,
        boot: Bootstrap,
    ) -> Result<Self, GatewayError> {
        let policy = CustodyPeerData::fresh(boot.session_ref.clone());
        let process = spawn.spawn()?;
        let mut peer = Self {
            process,
            socket,
            boot,
            policy,
        };
        peer.process.bootstrap()?;
        peer.check_live()?;
        Ok(peer)
    }
}
