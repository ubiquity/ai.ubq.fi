mod private_https;
use crate::private_https::{PrivateHttps, PrivateResponse};
use gateway_auth_credentials::provider_credentials::codex_credential_identity;
use gateway_auth_native_proof::native_auth::NativeTransportIdentity;
use gateway_auth_native_protocol::native_import::publication::NativeImportPublication;
use gateway_auth_native_wire::publication_command::{
    ImportAccessDocument, ImportCatalogData, ImportPublicationCommandData,
};
use gateway_auth_primitives::crypto::{Secret, sha256_hex};
use gateway_authority_contracts::NativeRenewalCancellation;
use gateway_native_custody_io::protocol::unavailable;
use gateway_native_custody_wire::bootstrap::ImportEndpointData;
use gateway_source::error::GatewayError;
use std::{
    net::{SocketAddr, TcpStream},
    time::{Duration, Instant},
};

pub struct FixedImportConnections {
    authority: TcpStream,
    publication: TcpStream,
    models: TcpStream,
    oauth: TcpStream,
}
pub struct AfterAuthority {
    publication: TcpStream,
    models: TcpStream,
    oauth: TcpStream,
}
pub struct AfterValidation {
    publication: TcpStream,
}
pub struct ValidationClients {
    models: PrivateHttps,
    oauth: Option<PrivateHttps>,
    client_version: String,
}
impl FixedImportConnections {
    pub fn prepare_authority<'a, C: NativeRenewalCancellation>(
        self,
        origin: &str,
        identity: NativeTransportIdentity,
        cancellation: &'a C,
        deadline: Instant,
    ) -> Result<
        (
            gateway_authority::native_control::import::remote::RemoteImportAuthority<'a, C>,
            AfterAuthority,
        ),
        GatewayError,
    > {
        let authority =
            fixed_authority_client(origin, self.authority, identity, cancellation, deadline)?;
        Ok((
            authority,
            AfterAuthority {
                publication: self.publication,
                models: self.models,
                oauth: self.oauth,
            },
        ))
    }
}
impl AfterAuthority {
    pub fn prepare_validation(
        self,
        version: Option<&gateway_source::js_string::JsString16>,
    ) -> Result<(ValidationClients, AfterValidation), GatewayError> {
        let client_version = gateway_native_import_source::catalog::selected_version(version);
        let roots = validation_roots();
        let models = PrivateHttps::models_connected(self.models, roots.clone(), &client_version)?;
        let oauth = PrivateHttps::from_connected(
            "https://auth.openai.com/oauth/token",
            "/oauth/token",
            self.oauth,
            roots,
        )?;
        Ok((
            ValidationClients {
                models,
                oauth: Some(oauth),
                client_version,
            },
            AfterValidation {
                publication: self.publication,
            },
        ))
    }
}
impl AfterValidation {
    pub fn prepare_publication(
        self,
        origin: &str,
        identity: NativeTransportIdentity,
    ) -> Result<gateway_native_import_publication_http::ImportPublicationHttp, GatewayError> {
        #[cfg(feature = "native-import-fixture")]
        {
            let _ = origin;
            gateway_native_import_publication_http::ImportPublicationHttp::from_fixture_connected(
                self.publication,
                identity,
            )
        }
        #[cfg(not(feature = "native-import-fixture"))]
        {
            let target = url::Url::parse(origin)
                .map_err(|_| unavailable())?
                .join(gateway_auth_native_wire::native_read::NATIVE_PUBLICATION_PATH)
                .map_err(|_| unavailable())?;
            gateway_native_import_publication_http::ImportPublicationHttp::from_connected(
                target.as_str(),
                self.publication,
                identity,
            )
        }
    }
}
impl ValidationClients {
    pub fn probe_native_credentials(
        &self,
        credentials: &gateway_auth_credentials::native_credentials::NativeCredentials,
        deadline: Instant,
    ) -> Result<NativeProviderResponseData, GatewayError> {
        let headers = vec![
            (
                "Authorization".into(),
                format!("Bearer {}", credentials.access_token.expose()),
            ),
            ("ChatGPT-Account-ID".into(), credentials.account_id.clone()),
            ("originator".into(), "codex_cli_rs".into()),
            (
                "user-agent".into(),
                format!("codex_cli_rs/{} (ai.ubq.fi)", self.client_version),
            ),
        ];
        let result = self.models.get_models(&headers, deadline).map_err(|_| GatewayError::new(502,
            "Upstream validation request failed: Codex upstream request failed: upstream unreachable.", Some("codex_upstream_unreachable")))?;
        Ok(NativeProviderResponseData {
            status: result.status,
            headers: result.headers,
            body: result.body,
        })
    }
    pub fn normalize_models_data(
        &self,
        body: &gateway_source::source_json::SourceJson,
        status: u16,
        observed_at_ms: i64,
    ) -> Result<gateway_native_import_source::catalog::CatalogValidation, GatewayError> {
        gateway_native_import_source::catalog::normalized_catalog(
            body,
            &self.client_version,
            status,
            observed_at_ms,
        )
    }
    pub fn dispatch_original_refresh(
        &mut self,
        credentials: &gateway_auth_credentials::native_credentials::NativeCredentials,
        deadline: Instant,
    ) -> Result<NativeProviderResponseData, GatewayError> {
        let transport = self.oauth.take().ok_or_else(unavailable)?;
        let request = serde_json::to_vec(&serde_json::json!({"client_id":"app_EMoamEEZ73f0CkXaXp7hrann", "grant_type":"refresh_token",
            "refresh_token":credentials.refresh_token.expose(), "scope":"openid profile email"})).map_err(|_| unavailable())?;
        let result = transport.send(
            &[],
            &request,
            deadline.min(Instant::now() + Duration::from_secs(10)),
            usize::MAX,
        )?;
        Ok(NativeProviderResponseData {
            status: result.status,
            headers: result.headers,
            body: result.body,
        })
    }
}
/// Ordinary authenticated-transport observations, not native permission or a validated candidate.
pub struct NativeProviderResponseData {
    pub status: u16,
    pub headers: Vec<(String, String)>,
    pub body: Vec<u8>,
}
pub fn connect_endpoints<C: NativeRenewalCancellation>(
    data: ImportEndpointData,
    cancellation: &C,
    deadline: Instant,
) -> Result<FixedImportConnections, GatewayError> {
    data.validate()?;
    let connect = |address: SocketAddr| {
        if cancellation.cancelled() {
            return Err(unavailable());
        }
        let remaining = deadline
            .checked_duration_since(Instant::now())
            .filter(|d| !d.is_zero())
            .ok_or_else(unavailable)?;
        let socket = TcpStream::connect_timeout(&address, remaining.min(Duration::from_secs(5)))
            .map_err(|_| unavailable())?;
        socket.set_nonblocking(false).map_err(|_| unavailable())?;
        if cancellation.cancelled()
            || Instant::now() >= deadline
            || socket.peer_addr().map_err(|_| unavailable())? != address
        {
            return Err(unavailable());
        }
        Ok(socket)
    };
    // Distinct streams; no application byte proxy or shared ureq agent.
    Ok(FixedImportConnections {
        authority: connect(data.authority)?,
        publication: connect(data.authority)?,
        models: connect(data.models)?,
        oauth: connect(data.oauth)?,
    })
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
fn fixed_authority_client<'a, C: NativeRenewalCancellation>(
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
pub fn validation_roots() -> ureq::tls::RootCerts {
    #[cfg(feature = "native-import-fixture")]
    {
        ureq::tls::RootCerts::new_with_certs(&[ureq::tls::Certificate::from_der(include_bytes!(
            "../tests/fixtures/import-tls/ca.der"
        ))
        .to_owned()])
    }
    #[cfg(not(feature = "native-import-fixture"))]
    {
        ureq::tls::RootCerts::WebPki
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        net::TcpListener,
        sync::atomic::{AtomicBool, Ordering},
    };
    struct Cancellation(AtomicBool);
    impl NativeRenewalCancellation for Cancellation {
        fn cancelled(&self) -> bool {
            self.0.load(Ordering::SeqCst)
        }
    }
    #[test]
    fn actual_fixed_role_sockets_are_distinct_and_cancellation_opens_none()
    -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
        let authority = TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, 0))?;
        let models = TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, 0))?;
        let oauth = TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, 0))?;
        let data = || -> std::io::Result<ImportEndpointData> {
            Ok(ImportEndpointData {
                authority: authority.local_addr()?,
                models: models.local_addr()?,
                oauth: oauth.local_addr()?,
            })
        };
        let cancellation = Cancellation(AtomicBool::new(false));
        let connected = connect_endpoints(
            data()?,
            &cancellation,
            Instant::now() + Duration::from_secs(2),
        )?;
        assert_ne!(
            connected.authority.local_addr()?,
            connected.publication.local_addr()?
        );
        for (listener, socket) in [
            (&authority, &connected.authority),
            (&authority, &connected.publication),
            (&models, &connected.models),
            (&oauth, &connected.oauth),
        ] {
            let (accepted, peer) = listener.accept()?;
            assert_eq!(peer, socket.local_addr()?);
            assert_eq!(accepted.local_addr()?, socket.peer_addr()?);
        }
        drop(connected);
        cancellation.0.store(true, Ordering::SeqCst);
        assert!(
            connect_endpoints(
                data()?,
                &cancellation,
                Instant::now() + Duration::from_secs(2)
            )
            .is_err()
        );
        for listener in [&authority, &models, &oauth] {
            listener.set_nonblocking(true)?;
            assert!(
                matches!(listener.accept(), Err(error) if error.kind() == std::io::ErrorKind::WouldBlock)
            );
        }
        Ok(())
    }
}
