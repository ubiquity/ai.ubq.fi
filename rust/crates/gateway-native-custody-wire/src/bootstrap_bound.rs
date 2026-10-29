use crate::bootstrap::{Bootstrap, ImportEndpointData};
use crate::wire::unavailable;
use gateway_source::error::GatewayError;
pub fn bootstrap_max_bytes() -> Result<usize, GatewayError> {
    use std::net::{Ipv6Addr, SocketAddrV6};
    let local = std::net::SocketAddr::V6(SocketAddrV6::new(
        Ipv6Addr::LOCALHOST,
        u16::MAX,
        u32::MAX,
        u32::MAX,
    ));
    let remote = std::net::SocketAddr::V6(SocketAddrV6::new(
        Ipv6Addr::new(
            0x7777, 0x7777, 0x7777, 0x7777, 0x7777, 0x7777, 0x7777, 0x7777,
        ),
        u16::MAX,
        u32::MAX,
        u32::MAX,
    ));
    serde_json::to_vec(&Bootstrap {
        v: 1,
        session_ref: "f".repeat(64),
        client_local: local,
        client_peer: local,
        custody_local: local,
        custody_peer: local,
        native_owner_id: "\u{1}".repeat(4096),
        account_id_hash: "f".repeat(64),
        endpoints: ImportEndpointData {
            authority: remote,
            models: remote,
            oauth: remote,
        },
    })
    .map(|bytes| bytes.len())
    .map_err(|_| unavailable())
}
