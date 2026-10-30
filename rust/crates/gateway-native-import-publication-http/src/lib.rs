mod private_https;
use gateway_auth_credentials::provider_credentials::codex_credential_identity;
use gateway_auth_native_proof::native_auth::NativeTransportIdentity;
use gateway_auth_native_protocol::native_import::publication::NativeImportPublication;
use gateway_auth_native_wire::publication_command::{
    ImportAccessDocument, ImportCatalogData, ImportPublicationCommandData,
};
use gateway_auth_primitives::crypto::{Secret, sha256_hex};
use gateway_native_custody_io::protocol::unavailable;
use gateway_source::error::GatewayError;
use std::{
    net::TcpStream,
    time::{Duration, Instant},
};
/// Actual matching signed network reply only; not a native durability or final completion proof.
pub struct AuthenticatedImportPublicationReply {
    publication: NativeImportPublication,
    request_digest: String,
}
impl AuthenticatedImportPublicationReply {
    pub(crate) fn publication(&self) -> &NativeImportPublication {
        &self.publication
    }
    pub(crate) fn request_digest(&self) -> &str {
        &self.request_digest
    }
}

impl AuthenticatedImportPublicationReply {
    pub fn require_current_terminal_data(
        &self,
        current: &gateway_auth_native_import_data::native_import::authority::ImportAuthorityTerminalData,
        request_digest: &str,
    ) -> Result<(), GatewayError> {
        let p = &self.publication.publication;
        if self.request_digest != request_digest
            || p.publication_revision != current.publication_revision
            || p.native_receipt_ref != current.native_receipt_ref
            || p.native_source_generation != current.native_source_generation
            || p.access_generation != current.access_generation
            || p.credential_version != current.credential_version
            || p.source_revision != current.source_revision
            || p.normalized_revision != current.normalized_revision
            || p.catalog_revision != current.catalog_revision
            || p.runtime_revision != current.runtime_revision
            || p.auth_generation_revision != current.auth_generation_revision
            || p.paid_control_config_revision != current.paid_control_config_revision
        {
            return Err(unavailable());
        }
        Ok(())
    }
    pub fn disposition_data(
        &self,
    ) -> gateway_auth_native_protocol::native_import::publication::NativeImportPublicationDisposition
    {
        self.publication.disposition
    }
}
use crate::private_https::PrivateHttps;

use gateway_auth_native_wire::native_read::{
    NATIVE_KEY_HEADER, NATIVE_MAC_HEADER, NATIVE_NONCE_HEADER, NATIVE_PUBLICATION_PATH,
    NATIVE_RESPONSE_MAC_HEADER, NATIVE_TIME_HEADER, NATIVE_VERSION_HEADER,
};
use gateway_auth_primitives::crypto::base64url;

/// One actual constructor-selected TLS transport; no raw-response/callback issuer.
pub struct ImportPublicationHttp {
    identity: NativeTransportIdentity,
    transport: PrivateHttps,
}
impl ImportPublicationHttp {
    #[cfg(not(feature = "native-import-fixture"))]
    pub fn from_connected(
        target: &str,
        socket: TcpStream,
        identity: NativeTransportIdentity,
    ) -> Result<Self, GatewayError> {
        Ok(Self {
            identity,
            transport: PrivateHttps::from_connected(
                target,
                NATIVE_PUBLICATION_PATH,
                socket,
                ureq::tls::RootCerts::WebPki,
            )?,
        })
    }
    #[cfg(feature = "native-import-fixture")]
    pub fn from_fixture_connected(
        socket: TcpStream,
        identity: NativeTransportIdentity,
    ) -> Result<Self, GatewayError> {
        if !socket
            .local_addr()
            .map_err(|_| unavailable())?
            .ip()
            .is_loopback()
            || !socket
                .peer_addr()
                .map_err(|_| unavailable())?
                .ip()
                .is_loopback()
        {
            return Err(unavailable());
        }
        Ok(Self {
            identity,
            transport: PrivateHttps::from_connected(
                "https://localhost/uos/native/publication",
                NATIVE_PUBLICATION_PATH,
                socket,
                validation_roots(),
            )?,
        })
    }
    #[cfg(test)]
    fn fixture(
        target: &str,
        socket: TcpStream,
        identity: NativeTransportIdentity,
        ca: &[u8],
    ) -> Result<Self, GatewayError> {
        Ok(Self {
            identity,
            transport: PrivateHttps::from_connected(
                target,
                NATIVE_PUBLICATION_PATH,
                socket,
                ureq::tls::RootCerts::new_with_certs(&[
                    ureq::tls::Certificate::from_der(ca).to_owned()
                ]),
            )?,
        })
    }
    pub fn exchange_data(
        &self,
        command: &ImportPublicationCommandData,
        deadline: Instant,
    ) -> Result<AuthenticatedImportPublicationReply, GatewayError> {
        let body = command.bytes()?;
        let mut random = [0u8; 32];
        getrandom::fill(&mut random).map_err(|_| unavailable())?;
        let nonce = base64url(&random);
        let now = gateway_auth_native_proof::native_context::now_ms()?;
        let mac = self
            .identity
            .sign(NATIVE_PUBLICATION_PATH, now, &nonce, &body)?;
        let headers = vec![
            (NATIVE_VERSION_HEADER.into(), "1".into()),
            (NATIVE_KEY_HEADER.into(), self.identity.key_id().into()),
            (NATIVE_TIME_HEADER.into(), now.to_string()),
            (NATIVE_NONCE_HEADER.into(), nonce.clone()),
            (NATIVE_MAC_HEADER.into(), mac),
        ];
        let response = self
            .transport
            .send_import_publication(&headers, &body, deadline)?;
        let signatures: Vec<_> = response
            .headers
            .iter()
            .filter(|(k, _)| k.eq_ignore_ascii_case(NATIVE_RESPONSE_MAC_HEADER))
            .collect();
        if signatures.len() != 1 {
            return Err(unavailable());
        }
        self.identity
            .verify_publication_response(
                &nonce,
                &body,
                response.status,
                &response.body,
                &signatures[0].1,
            )
            .map_err(|_| unavailable())?;
        if response.status != 200 {
            return Err(unavailable());
        }
        let publication = NativeImportPublication::parse_for(&response.body, &body)?;
        Ok(AuthenticatedImportPublicationReply {
            publication,
            request_digest: sha256_hex(&body),
        })
    }
}

#[cfg(test)]
mod tests;
fn validation_roots() -> ureq::tls::RootCerts {
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
