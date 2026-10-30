use gateway_native_confinement::file_observation::Facts;
use gateway_source::error::GatewayError;
use serde::{Deserialize, Serialize};
pub const EPOCH_LIMIT: u64 = 2048;
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Epoch {
    pub v: u8,
    pub native_owner_id: String,
    pub account_id_hash: String,
    pub root_device: u64,
    pub root_inode: u64,
    pub boot: String,
    pub phase: Phase,
}
#[derive(Clone, Copy, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Phase {
    Running,
    Stopping,
    Stopped,
}
pub fn encode(epoch: &Epoch) -> Result<Vec<u8>, GatewayError> {
    let bytes = serde_json::to_vec(epoch).map_err(|_| fail())?;
    if bytes.len() as u64 > EPOCH_LIMIT {
        return Err(fail());
    }
    Ok(bytes)
}
pub fn decode(bytes: &[u8]) -> Result<Option<Epoch>, GatewayError> {
    if bytes.len() as u64 > EPOCH_LIMIT {
        return Err(fail());
    }
    serde_json::from_slice(bytes).map(Some).map_err(|_| fail())
}
pub fn require_identity(
    epoch: &Epoch,
    owner: &str,
    account: &str,
    facts: Facts,
) -> Result<(), GatewayError> {
    if epoch.v != 1
        || epoch.native_owner_id != owner
        || epoch.account_id_hash != account
        || epoch.root_device != facts.device
        || epoch.root_inode != facts.inode
        || !gateway_auth_credentials::provider_credentials::valid_native_generation(&epoch.boot)
    {
        return Err(fail());
    }
    Ok(())
}
fn fail() -> GatewayError {
    GatewayError::unavailable(
        "Exclusive native owner custody is unavailable or changed",
        "native_import_owner_exclusion_unavailable",
    )
}
