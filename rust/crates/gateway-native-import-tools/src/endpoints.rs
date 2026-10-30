use gateway_auth_native_proof::native_auth::NativeTransportIdentity;
use gateway_authority_contracts::NativeRenewalCancellation;
use gateway_native_custody_io::protocol::unavailable;
use gateway_native_custody_wire::bootstrap::ImportEndpointData;
use gateway_source::error::GatewayError;
use std::{
    net::{SocketAddr, TcpStream, ToSocketAddrs},
    time::{Duration, Instant},
};
/// Only trusted bootstrap resolves these. The actor cannot select another URL.
pub struct ResolvedImportEndpoints {
    origin: String,
    data: ImportEndpointData,
}
impl ResolvedImportEndpoints {
    pub(crate) fn resolve(authority_origin: &str) -> Result<Self, GatewayError> {
        let origin = checked_origin(authority_origin)?;
        let authority = resolve(
            origin.host_str().ok_or_else(unavailable)?,
            origin.port_or_known_default().ok_or_else(unavailable)?,
        )?;
        let data = ImportEndpointData {
            authority,
            models: resolve("chatgpt.com", 443)?,
            oauth: resolve("auth.openai.com", 443)?,
        };
        data.validate()?;
        Ok(Self {
            origin: origin.into(),
            data,
        })
    }
    #[cfg(feature = "native-import-fixture")]
    pub(crate) fn fixture(
        authority: SocketAddr,
        models: SocketAddr,
        oauth: SocketAddr,
    ) -> Result<Self, GatewayError> {
        let data = ImportEndpointData {
            authority,
            models,
            oauth,
        };
        data.validate()?;
        if data
            .addresses()
            .iter()
            .any(|address| !address.ip().is_loopback())
        {
            return Err(unavailable());
        }
        Ok(Self {
            origin: "https://localhost/".into(),
            data,
        })
    }
    pub(crate) fn require_origin(&self, origin: &str) -> Result<(), GatewayError> {
        let origin: String = checked_origin(origin)?.into();
        if self.origin != origin {
            return Err(unavailable());
        }
        self.data.validate()
    }
    pub(crate) fn data(&self) -> Result<ImportEndpointData, GatewayError> {
        self.data.validate()?;
        Ok(ImportEndpointData {
            authority: self.data.authority,
            models: self.data.models,
            oauth: self.data.oauth,
        })
    }
}
fn checked_origin(value: &str) -> Result<url::Url, GatewayError> {
    let origin = url::Url::parse(value).map_err(|_| unavailable())?;
    if origin.scheme() != "https"
        || origin.host_str().is_none()
        || !origin.username().is_empty()
        || origin.password().is_some()
        || origin.path() != "/"
        || origin.query().is_some()
        || origin.fragment().is_some()
    {
        return Err(unavailable());
    }
    Ok(origin)
}
fn resolve(host: &str, port: u16) -> Result<SocketAddr, GatewayError> {
    (host, port)
        .to_socket_addrs()
        .map_err(|_| unavailable())?
        .find(|address| {
            address.port() != 0 && !address.ip().is_unspecified() && !address.ip().is_multicast()
        })
        .ok_or_else(unavailable)
}

pub(crate) fn fixed_authority_client<'a, C: NativeRenewalCancellation>(
    origin: &str,
    socket: TcpStream,
    identity: gateway_auth_native_proof::native_auth::NativeTransportIdentity,
    cancellation: &'a C,
    deadline: Instant,
) -> Result<
    gateway_authority::native_control::import::remote::RemoteImportAuthority<'a, C>,
    GatewayError,
> {
    let origin = checked_origin(origin)?;
    #[cfg(feature = "native-import-fixture")]
    {
        if origin.host_str() != Some("localhost") {
            return Err(unavailable());
        }
        gateway_authority::native_control::import::remote::RemoteImportAuthority::from_fixture_connected(
            socket,
            identity,
            cancellation,
            deadline,
        )
    }
    #[cfg(not(feature = "native-import-fixture"))]
    {
        let target = origin
            .join(gateway_auth_native_wire::native_read::NATIVE_RENEWAL_AUTHORITY_PATH)
            .map_err(|_| unavailable())?;
        gateway_authority::native_control::import::remote::RemoteImportAuthority::from_connected(
            target.as_str(),
            socket,
            identity,
            cancellation,
            deadline,
        )
    }
}

pub struct CustodyAuthorityConnection {
    origin: String,
    socket: TcpStream,
}
impl ResolvedImportEndpoints {
    pub fn resolve_import(
        config: &gateway_native_daemon_config::config::PreparedNativeDaemonConfig,
    ) -> Result<Self, GatewayError> {
        let registry = config.owner_renewal.as_ref().ok_or_else(unavailable)?;
        Self::resolve(registry.authority_origin())
    }
    pub fn connect_custody_authority(
        &self,
        config: &gateway_native_daemon_config::config::PreparedNativeDaemonConfig,
    ) -> Result<CustodyAuthorityConnection, GatewayError> {
        let registry = config.owner_renewal.as_ref().ok_or_else(unavailable)?;
        self.require_origin(registry.authority_origin())?;
        let socket = TcpStream::connect_timeout(&self.data()?.authority, Duration::from_secs(5))
            .map_err(|_| unavailable())?;
        Ok(CustodyAuthorityConnection {
            origin: registry.authority_origin().into(),
            socket,
        })
    }
}
impl CustodyAuthorityConnection {
    pub fn into_remote_import_authority<'a, C: NativeRenewalCancellation>(
        self,
        identity: NativeTransportIdentity,
        cancellation: &'a C,
        deadline: Instant,
    ) -> Result<
        gateway_authority::native_control::import::remote::RemoteImportAuthority<'a, C>,
        GatewayError,
    > {
        fixed_authority_client(&self.origin, self.socket, identity, cancellation, deadline)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn closed_endpoint_data_rejects_mixed_fields_zero_ports_and_unfixed_origins() {
        for origin in [
            "http://localhost/",
            "https://localhost/path",
            "https://user@localhost/",
            "https://localhost/?target=x",
            "https://localhost/#x",
        ] {
            assert!(checked_origin(origin).is_err());
        }
        let raw = r#"{"authority":"127.0.0.1:443","models":"127.0.0.1:443","oauth":"127.0.0.1:443","provider":"anything"}"#;
        assert!(serde_json::from_str::<ImportEndpointData>(raw).is_err());
        let data = ImportEndpointData {
            authority: "127.0.0.1:443".parse().unwrap(),
            models: "127.0.0.1:0".parse().unwrap(),
            oauth: "127.0.0.1:443".parse().unwrap(),
        };
        assert!(data.validate().is_err());
    }
}
