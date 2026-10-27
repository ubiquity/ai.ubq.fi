//! Pure fixed synthetic TLS configuration DATA; no private service or authority factory.
use rustls::{
    ServerConfig,
    pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer},
};
use std::sync::Arc;
pub fn configs() -> Result<(Arc<ServerConfig>, Arc<rustls::ClientConfig>), rustls::Error> {
    let cert = CertificateDer::from(include_bytes!("../fixtures/native-tls/leaf.der").to_vec());
    let key = PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(
        include_bytes!("../fixtures/native-tls/leaf.pk8.der").to_vec(),
    ));
    let server = ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(vec![cert], key)?;
    let mut roots = rustls::RootCertStore::empty();
    roots.add(CertificateDer::from(
        include_bytes!("../fixtures/native-tls/ca.der").to_vec(),
    ))?;
    Ok((
        Arc::new(server),
        Arc::new(
            rustls::ClientConfig::builder()
                .with_root_certificates(roots)
                .with_no_client_auth(),
        ),
    ))
}
