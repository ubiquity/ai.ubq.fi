//! One synthetic signed recheck peer; no native context, proof or currentness factory.
use crate::live_tls;
use gateway_auth_native_proof::native_auth::NativeTransportIdentity;
use gateway_auth_native_wire::native_read::{NATIVE_NONCE_HEADER, NATIVE_RESPONSE_MAC_HEADER};
use gateway_native_authority_request::NativeRenewalAuthorityRequest as Request;
use rustls::{ServerConnection, StreamOwned};
use serde::Serialize;
use std::{
    io::{Read, Write},
    net::TcpStream,
    thread::{self, JoinHandle},
    time::Duration,
};

type PeerResult = Result<(), Box<dyn std::error::Error + Send + Sync>>;
fn request_bytes(
    stream: &mut impl Read,
) -> Result<(String, Vec<u8>), Box<dyn std::error::Error + Send + Sync>> {
    let mut bytes = Vec::new();
    loop {
        if let Some(end) = bytes.windows(4).position(|v| v == b"\r\n\r\n") {
            let head = std::str::from_utf8(&bytes[..end])?;
            let header = |name: &str| -> Result<String, Box<dyn std::error::Error + Send + Sync>> {
                Ok(head
                    .lines()
                    .filter_map(|line| line.split_once(':'))
                    .find(|(key, _)| key.eq_ignore_ascii_case(name))
                    .ok_or("missing fixture request header")?
                    .1
                    .trim()
                    .to_owned())
            };
            let size = header("content-length")?.parse::<usize>()?;
            if bytes.len() >= end + 4 + size {
                return Ok((
                    header(NATIVE_NONCE_HEADER)?,
                    bytes[end + 4..end + 4 + size].to_vec(),
                ));
            }
        }
        assert!(bytes.len() < 100_000);
        let mut block = [0; 4096];
        let n = stream.read(&mut block)?;
        assert!(n > 0);
        bytes.extend_from_slice(&block[..n]);
    }
}
/// The one existing fixed recheck operation and raw request-bound MAC, with plain fixture DATA.
pub fn signed_recheck<T: Serialize + Send + 'static>(
    peer: TcpStream,
    identity: NativeTransportIdentity,
    value: T,
    replayed_body: bool,
    response_version: u8,
) -> std::io::Result<JoinHandle<PeerResult>> {
    peer.set_read_timeout(Some(Duration::from_secs(3)))?;
    peer.set_write_timeout(Some(Duration::from_secs(3)))?;
    Ok(thread::spawn(move || -> PeerResult {
        let (config, _) = live_tls::configs()?;
        let mut tls = StreamOwned::new(ServerConnection::new(config)?, peer);
        let (nonce, body) = request_bytes(&mut tls)?;
        assert!(matches!(
            Request::parse(&body)?,
            Request::Recheck { v: 1, .. }
        ));
        #[derive(Serialize)]
        struct RecheckFrame<T> {
            v: u8,
            operation: &'static str,
            fence: T,
        }
        let response = serde_json::to_vec(&RecheckFrame {
            v: response_version,
            operation: "recheck",
            fence: value,
        })?;
        let old = serde_json::to_vec(&Request::Recheck {
            v: 1,
            operation_id: "old-opaque-operation".into(),
            claim_ref: "old-opaque-claim".into(),
        })?;
        let mac = identity.sign_authority_response(
            &nonce,
            if replayed_body { &old } else { &body },
            200,
            &response,
        )?;
        tls.write_all(format!("HTTP/1.1 200 OK\r\nContent-Length: {}\r\n{NATIVE_RESPONSE_MAC_HEADER}: {mac}\r\nConnection: close\r\n\r\n",response.len()).as_bytes())?;
        tls.write_all(&response)?;
        tls.flush()?;
        Ok(())
    }))
}
