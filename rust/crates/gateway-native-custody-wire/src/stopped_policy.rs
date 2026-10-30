use crate::wire::unavailable;
use crate::wire::{self as data, Stopped};
use gateway_authority::native_control::import::custody_session::ImportCustodyStartedRequest;
use gateway_source::error::GatewayError;

pub fn verify_stopped_data(
    stopped: &Stopped,
    binding: &ImportCustodyStartedRequest,
    session_ref: &str,
    challenge: &str,
    sequence: u64,
) -> Result<(), GatewayError> {
    if stopped.v != 1
        || stopped.sequence != sequence
        || stopped.session_ref != session_ref
        || stopped.challenge != challenge
        || stopped.binding.canonical_bytes()? != binding.canonical_bytes()?
        || !data::hash(&stopped.epoch)
        || stopped.root_inode == 0
    {
        return Err(unavailable());
    }
    Ok(())
}
