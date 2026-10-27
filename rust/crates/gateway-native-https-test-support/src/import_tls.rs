//! Synthetic models/OAuth TLS socket and finite redirect fixture.
use crate::observation::Observed;
use rustls::{
    ServerConfig, ServerConnection, StreamOwned,
    pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer},
};
use std::{
    collections::BTreeMap,
    io::{Read, Write},
    net::{TcpListener, TcpStream},
    sync::Arc,
    thread,
    time::Duration,
};
pub struct Peer {
    socket: Option<TcpStream>,
    pub task: thread::JoinHandle<Option<Observed>>,
}
pub fn roots(import: bool) -> ureq::tls::RootCerts {
    let bytes: &[u8] = if import {
        include_bytes!("../fixtures/import-tls/ca.der")
    } else {
        include_bytes!("../fixtures/native-tls/ca.der")
    };
    ureq::tls::RootCerts::new_with_certs(&[ureq::tls::Certificate::from_der(bytes).to_owned()])
}
impl Peer {
    pub fn start(import: bool, redirect: bool) -> Self {
        let listener = TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, 0)).unwrap();
        let socket = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
        let task = thread::spawn(move || {
            let (socket, _) = listener.accept().ok()?;
            drop(listener);
            socket.set_read_timeout(Some(Duration::from_secs(3))).ok()?;
            socket
                .set_write_timeout(Some(Duration::from_secs(3)))
                .ok()?;
            let (cert, key): (&[u8], &[u8]) = if import {
                (
                    include_bytes!("../fixtures/import-tls/leaf.der"),
                    include_bytes!("../fixtures/import-tls/leaf.pk8.der"),
                )
            } else {
                (
                    include_bytes!("../fixtures/native-tls/leaf.der"),
                    include_bytes!("../fixtures/native-tls/leaf.pk8.der"),
                )
            };
            let config = ServerConfig::builder()
                .with_no_client_auth()
                .with_single_cert(
                    vec![CertificateDer::from(cert.to_vec())],
                    PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(key.to_vec())),
                )
                .ok()?;
            let mut tls = StreamOwned::new(ServerConnection::new(Arc::new(config)).ok()?, socket);
            let mut bytes = Vec::new();
            let (line, headers, request_body) = loop {
                if let Some(end) = bytes.windows(4).position(|part| part == b"\r\n\r\n") {
                    let header = std::str::from_utf8(&bytes[..end]).ok()?;
                    let mut lines = header.split("\r\n");
                    let line = lines.next()?.to_owned();
                    let headers: BTreeMap<String, String> = lines
                        .filter_map(|l| l.split_once(':'))
                        .map(|(k, v)| (k.to_ascii_lowercase(), v.trim().to_owned()))
                        .collect();
                    let length: usize = headers
                        .get("content-length")
                        .map(|v| v.parse().ok())
                        .unwrap_or(Some(0))?;
                    if bytes.len() >= end + 4 + length {
                        break (line, headers, bytes[end + 4..end + 4 + length].to_vec());
                    }
                }
                if bytes.len() >= 65_536 {
                    return None;
                }
                let mut part = [0; 4096];
                let n = tls.read(&mut part).ok()?;
                if n == 0 {
                    return None;
                }
                bytes.extend_from_slice(&part[..n]);
            };
            let sni = tls.conn.server_name()?.to_owned();
            let body = br#"{"models":[{"slug":"synthetic_model","display_name":"Synthetic"}]}"#;
            let head = if redirect {
                format!(
                    "HTTP/1.1 302 Found\r\nLocation: https://forbidden.invalid/collect\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                    body.len()
                )
            } else {
                format!(
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                    body.len()
                )
            };
            tls.write_all(head.as_bytes()).ok()?;
            tls.write_all(body).ok()?;
            tls.flush().ok()?;
            Some(Observed {
                sni,
                line,
                headers,
                body: request_body,
            })
        });
        Self {
            socket: Some(socket),
            task,
        }
    }
    pub fn socket(&mut self) -> TcpStream {
        self.socket.take().unwrap()
    }
}
