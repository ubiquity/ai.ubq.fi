use gateway_native_custody_io::protocol::unavailable;
use gateway_native_custody_wire::bootstrap::{Bootstrap, ImportEndpointData};
use gateway_source::error::GatewayError;
use std::{io::Read, net::TcpStream, os::fd::AsFd};

pub(crate) struct InheritedCustodyChannel {
    pub(crate) socket: TcpStream,
    pub(crate) session_ref: String,
    pub(crate) next_sequence: u64,
}
pub(crate) struct ActorBootstrap {
    pub(crate) config: Vec<u8>,
    pub(crate) client: TcpStream,
    pub(crate) custody: InheritedCustodyChannel,
    pub(crate) endpoints: ImportEndpointData,
    pub(crate) native_owner_id: String,
    pub(crate) account_id_hash: String,
}
impl ActorBootstrap {
    // Only the fixed parameterless import role calls this, before any thread or FD clone.
    pub(crate) fn from_stdio() -> Result<Self, GatewayError> {
        gateway_native_connector::connector_policy::inherited_only_stdio()
            .map_err(|_| unavailable())?;
        let stdin = std::io::stdin();
        if rustix::fs::FileType::from_raw_mode(
            rustix::fs::fstat(&stdin)
                .map_err(|_| unavailable())?
                .st_mode,
        ) != rustix::fs::FileType::Fifo
        {
            return Err(unavailable());
        }
        let mut input = stdin.lock();
        let config = bootstrap_part(&mut input, 65_536)?;
        let boot: Bootstrap = serde_json::from_slice(&bootstrap_part(
            &mut input,
            gateway_native_custody_wire::bootstrap_bound::bootstrap_max_bytes()?,
        )?)
        .map_err(|_| unavailable())?;
        boot.validate()?;
        let stdout = std::io::stdout();
        let stderr = std::io::stderr();
        for fd in [stdout.as_fd(), stderr.as_fd()] {
            if rustix::net::sockopt::socket_type(fd).map_err(|_| unavailable())?
                != rustix::net::SocketType::STREAM
            {
                return Err(unavailable());
            }
        }
        let client = TcpStream::from(
            stdout
                .as_fd()
                .try_clone_to_owned()
                .map_err(|_| unavailable())?,
        );
        let socket = TcpStream::from(
            stderr
                .as_fd()
                .try_clone_to_owned()
                .map_err(|_| unavailable())?,
        );
        if client.local_addr().map_err(|_| unavailable())? != boot.client_local
            || client.peer_addr().map_err(|_| unavailable())? != boot.client_peer
            || socket.local_addr().map_err(|_| unavailable())? != boot.custody_local
            || socket.peer_addr().map_err(|_| unavailable())? != boot.custody_peer
        {
            return Err(unavailable());
        }
        Ok(Self {
            config,
            client,
            endpoints: boot.endpoints,
            native_owner_id: boot.native_owner_id,
            account_id_hash: boot.account_id_hash,
            custody: InheritedCustodyChannel {
                socket,
                session_ref: boot.session_ref,
                next_sequence: 1,
            },
        })
    }
}
fn bootstrap_part(input: &mut impl Read, maximum: usize) -> Result<Vec<u8>, GatewayError> {
    let mut length = [0; 4];
    input.read_exact(&mut length).map_err(|_| unavailable())?;
    let length = usize::try_from(u32::from_be_bytes(length)).map_err(|_| unavailable())?;
    if length == 0 || length > maximum {
        return Err(unavailable());
    }
    let mut bytes = vec![0; length];
    input.read_exact(&mut bytes).map_err(|_| unavailable())?;
    Ok(bytes)
}

#[cfg(test)]
mod tests {
    use super::bootstrap_part;
    use gateway_native_custody_io::protocol::unavailable;
    use gateway_native_custody_wire::bootstrap::{Bootstrap, ImportEndpointData};
    use gateway_native_custody_wire::bootstrap_bound::bootstrap_max_bytes;
    use gateway_source::error::GatewayError;
    use std::net::SocketAddr;
    #[test]
    fn bootstrap_bound_is_derived_and_unknown_peer_data_refuses() -> Result<(), GatewayError> {
        use std::net::{Ipv6Addr, SocketAddrV6};
        let local = SocketAddr::V6(SocketAddrV6::new(
            Ipv6Addr::LOCALHOST,
            u16::MAX,
            u32::MAX,
            u32::MAX,
        ));
        let mut peer = local;
        peer.set_port(u16::MAX - 1);
        let remote = SocketAddr::V6(SocketAddrV6::new(
            Ipv6Addr::new(
                0x7777, 0x7777, 0x7777, 0x7777, 0x7777, 0x7777, 0x7777, 0x7777,
            ),
            u16::MAX,
            u32::MAX,
            u32::MAX,
        ));
        let boot = Bootstrap {
            v: 1,
            session_ref: "f".repeat(64),
            client_local: local,
            client_peer: peer,
            custody_local: local,
            custody_peer: peer,
            native_owner_id: "\u{1}".repeat(4096),
            account_id_hash: "f".repeat(64),
            endpoints: ImportEndpointData {
                authority: remote,
                models: remote,
                oauth: remote,
            },
        };
        boot.validate()?;
        let encoded = serde_json::to_vec(&boot).map_err(|_| unavailable())?;
        assert_eq!(encoded.len(), bootstrap_max_bytes()?);
        let round_trip: Bootstrap = serde_json::from_slice(&encoded).map_err(|_| unavailable())?;
        round_trip.validate()?;
        let mut extra: serde_json::Value =
            serde_json::from_slice(&encoded).map_err(|_| unavailable())?;
        extra["caller_destination"] = serde_json::json!("https://forbidden.invalid");
        assert!(serde_json::from_value::<Bootstrap>(extra).is_err());
        let mut over = std::io::Cursor::new(
            u32::try_from(bootstrap_max_bytes()? + 1)
                .map_err(|_| unavailable())?
                .to_be_bytes(),
        );
        assert!(bootstrap_part(&mut over, bootstrap_max_bytes()?).is_err());
        assert_eq!(over.position(), 4);
        Ok(())
    }
}
