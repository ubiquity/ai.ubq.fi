//! Fixed-path synthetic signed publication TLS peer; replies remain plain DATA.
use crate::observation::Observed;
use gateway_auth::{crypto::base64url, native_auth::NativeTransportIdentity};
use gateway_auth_native_wire::native_read::{
    NATIVE_MAC_HEADER, NATIVE_NONCE_HEADER, NATIVE_PUBLICATION_PATH, NATIVE_RESPONSE_MAC_HEADER,
    NATIVE_TIME_HEADER,
};
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

#[derive(Clone, Copy)]
pub enum Response {
    Valid,
    LargeValid,
    ForgedMac,
    Dropped,
    Redirect,
    OversizedError,
}
pub struct Peer {
    pub socket: TcpStream,
    pub task: thread::JoinHandle<Option<Observed>>,
    pub listener: TcpListener,
}
/// Owns the original preopened fixture sockets while private reply DATA is prepared.
pub struct PendingPeer {
    // Preserve the original locals' reverse-declaration drop order on setup failure.
    server: TcpListener,
    socket: TcpStream,
    listener: TcpListener,
}
impl PendingPeer {
    pub fn bind() -> Result<Self, Box<dyn std::error::Error>> {
        let listener = TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, 0))?;
        let socket = TcpStream::connect(listener.local_addr()?)?;
        let server = listener.try_clone()?;
        Ok(Self {
            socket,
            listener,
            server,
        })
    }
    pub fn start(
        self,
        expected: Vec<u8>,
        valid_reply: Vec<u8>,
        auth: NativeTransportIdentity,
        response: Response,
    ) -> Peer {
        let listener = self.listener;
        let socket = self.socket;
        let server = self.server;
        let task = thread::spawn(move || {
            let (socket, _) = server.accept().ok()?;
            socket.set_read_timeout(Some(Duration::from_secs(3))).ok()?;
            socket
                .set_write_timeout(Some(Duration::from_secs(3)))
                .ok()?;
            let cert = include_bytes!("../fixtures/native-tls/leaf.der");
            let key = include_bytes!("../fixtures/native-tls/leaf.pk8.der");
            let config = ServerConfig::builder()
                .with_no_client_auth()
                .with_single_cert(
                    vec![CertificateDer::from(cert.to_vec())],
                    PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(key.to_vec())),
                )
                .ok()?;
            let mut tls = StreamOwned::new(ServerConnection::new(Arc::new(config)).ok()?, socket);
            let (line, headers, body) = read_request(&mut tls)?;
            if body != expected {
                return None;
            }
            let nonce = headers.get(NATIVE_NONCE_HEADER)?;
            let time: i64 = headers.get(NATIVE_TIME_HEADER)?.parse().ok()?;
            if headers.get(NATIVE_MAC_HEADER)?
                != &auth
                    .sign(NATIVE_PUBLICATION_PATH, time, nonce, &body)
                    .ok()?
            {
                return None;
            }
            let observed = Observed {
                sni: tls.conn.server_name()?.into(),
                line,
                headers,
                body,
            };
            if matches!(response, Response::Dropped) {
                return Some(observed);
            }
            let status = if matches!(response, Response::Redirect) {
                302
            } else if matches!(response, Response::OversizedError) {
                503
            } else {
                200
            };
            let payload = if matches!(response, Response::OversizedError) {
                vec![b'x'; 65_537]
            } else {
                valid_reply
            };
            let signature = if matches!(response, Response::Valid | Response::LargeValid) {
                auth.sign_publication_response(
                    observed.headers.get(NATIVE_NONCE_HEADER)?,
                    &observed.body,
                    status,
                    &payload,
                )
                .ok()?
            } else {
                base64url(&[0; 32])
            };
            let head = format!(
                "HTTP/1.1 {status} Fixture\r\nContent-Type: application/json\r\nLocation: https://forbidden.invalid/collect\r\n{NATIVE_RESPONSE_MAC_HEADER}: {signature}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                payload.len()
            );
            let _ = tls
                .write_all(head.as_bytes())
                .and_then(|_| tls.write_all(&payload))
                .and_then(|_| tls.flush());
            Some(observed)
        });
        Peer {
            socket,
            task,
            listener,
        }
    }
}

fn read_request(
    tls: &mut StreamOwned<ServerConnection, TcpStream>,
) -> Option<(String, BTreeMap<String, String>, Vec<u8>)> {
    let mut bytes = Vec::new();
    loop {
        if let Some(end) = bytes.windows(4).position(|part| part == b"\r\n\r\n") {
            let header = std::str::from_utf8(&bytes[..end]).ok()?;
            let mut lines = header.split("\r\n");
            let line = lines.next()?.to_owned();
            let headers: BTreeMap<_, _> = lines
                .map(|line| line.split_once(':'))
                .collect::<Option<Vec<_>>>()?
                .into_iter()
                .map(|(name, value)| (name.to_ascii_lowercase(), value.trim().to_owned()))
                .collect();
            let length: usize = headers.get("content-length")?.parse().ok()?;
            if bytes.len() >= end + 4 + length {
                return Some((line, headers, bytes[end + 4..end + 4 + length].to_vec()));
            }
        }
        if bytes.len()
            > gateway_auth_native_wire::publication_command::import_publication_max_bytes() + 16_384
        {
            return None;
        }
        let mut chunk = [0; 8192];
        let n = tls.read(&mut chunk).ok()?;
        if n == 0 {
            return None;
        }
        bytes.extend_from_slice(&chunk[..n]);
    }
}
