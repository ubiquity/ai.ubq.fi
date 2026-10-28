//! Finite native live upgrade DATA and trusted checked-connection handler.
use gateway_auth_native_proof::native_live::AuthenticatedNativeLive;
use gateway_source::error::GatewayError;
use std::net::TcpStream;
pub struct NativeLiveUpgradeRequest {
    pub action_ref: String,
    pub headers: Vec<(String, String)>,
    /// Bytes already read after the HTTP header, including immediate frames.
    pub buffered: Vec<u8>,
}
/// Neither variant is an upstream or delivery-success receipt.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum NativeLiveOutcome {
    CloseRequested { code: u16 },
    TransportUnknown,
}
pub trait NativeLiveHandler: Send + Sync {
    fn handle(
        &self,
        stream: &mut TcpStream,
        request: NativeLiveUpgradeRequest,
        proof: AuthenticatedNativeLive,
    ) -> Result<NativeLiveOutcome, GatewayError>;
}
