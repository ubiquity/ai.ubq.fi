//! Pure fixed synthetic TLS configuration DATA; no private service or authority factory.
use rustls::{
    ServerConfig,
    pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer},
};
use std::sync::Arc;
pub fn configs() -> (Arc<ServerConfig>, Arc<rustls::ClientConfig>) {
    let cert = CertificateDer::from(include_bytes!("../fixtures/native-tls/leaf.der").to_vec());
    let key = PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(
        include_bytes!("../fixtures/native-tls/leaf.pk8.der").to_vec(),
    ));
    let server = ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(vec![cert], key)
        .unwrap();
    let mut roots = rustls::RootCertStore::empty();
    roots
        .add(CertificateDer::from(
            include_bytes!("../fixtures/native-tls/ca.der").to_vec(),
        ))
        .unwrap();
    (
        Arc::new(server),
        Arc::new(
            rustls::ClientConfig::builder()
                .with_root_certificates(roots)
                .with_no_client_auth(),
        ),
    )
}
