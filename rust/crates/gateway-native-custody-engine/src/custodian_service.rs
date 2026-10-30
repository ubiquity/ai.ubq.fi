//! Fixed actual Child service. No credentials, import document or raw fd enters it.
use crate::{actor_channel::OwnedImportChildChannel, custodian_runtime::CustodianRuntime};
use gateway_auth_native_proof::native_auth::NativeTransportIdentity;
use gateway_authority_contracts::NativeRenewalCancellation;
use gateway_native_custody_io::protocol::unavailable;
use gateway_source::error::GatewayError;
use std::{
    net::TcpStream,
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

struct OwnedPeerCancellation(Arc<Mutex<OwnedImportChildChannel>>);
impl NativeRenewalCancellation for OwnedPeerCancellation {
    fn cancelled(&self) -> bool {
        self.0
            .lock()
            .map(|mut peer| peer.check_live().is_err())
            .unwrap_or(true)
    }
}
pub(crate) fn serve(
    runtime: &mut CustodianRuntime,
    peer: OwnedImportChildChannel,
    connection: gateway_native_import_tools::endpoints::CustodyAuthorityConnection,
    identity: NativeTransportIdentity,
) -> Result<(), GatewayError> {
    let deadline = peer
        .deadline()
        .min(Instant::now() + Duration::from_secs(45));
    let peer = Arc::new(Mutex::new(peer));
    let cancellation = OwnedPeerCancellation(peer.clone());
    let authority = connection.into_remote_import_authority(identity, &cancellation, deadline)?;
    loop {
        let command = peer.lock().map_err(|_| unavailable())?.receive()?;
        if command.data.is_quiesce() {
            let fresh = authority.verify_started_custody(command.data.header().binding)?;
            {
                let mut peer = peer.lock().map_err(|_| unavailable())?;
                runtime.open_import_session(&mut peer, &command, fresh)?;
            }
            // Recheck CURRENT Started after actual kill/wait/kernel settlement,
            // before the first Stopped reply can reach the actor.
            let fresh = authority.verify_started_custody(command.data.header().binding)?;
            let mut peer = peer.lock().map_err(|_| unavailable())?;
            let snapshot = runtime.recheck_import_session(&mut peer, &command, fresh)?;
            peer.stopped(&command, snapshot)?;
        } else if command.data.is_check() {
            let fresh = authority.verify_started_custody(command.data.header().binding)?;
            let mut peer = peer.lock().map_err(|_| unavailable())?;
            let snapshot = runtime.recheck_import_session(&mut peer, &command, fresh)?;
            peer.stopped(&command, snapshot)?;
        } else if let Some(revision) = command.data.publication_revision() {
            let fresh =
                authority.verify_custody_terminal(command.data.header().binding, revision)?;
            let mut peer = peer.lock().map_err(|_| unavailable())?;
            runtime.release_import_session(&mut peer, &command, fresh)?;
            // Keep the sole parent pipe/channel alive until the actual actor
            // has written its response and exited. Never kill it on release.
            return peer.settle_completed();
        } else {
            let mut peer = peer.lock().map_err(|_| unavailable())?;
            peer.refuse(&command)?;
            // Cancel never stops a still-running writer, releases an uncertain
            // epoch, restarts a daemon or manufactures a terminal observation.
            return Err(unavailable());
        }
    }
}
