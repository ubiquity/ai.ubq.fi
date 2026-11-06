use super::*;
use crate::confinement::{NativeConfinement, enforce};
use crate::owned_case::{
    Must, TestResult, ThreadResult, observe_kernel_contract, run_owned_case, text,
};
use gateway_auth::native_auth::{NativeAuthHeaders, NativeCapability};
use rustls::{ServerConfig, ServerConnection, StreamOwned, pki_types::*};
use std::{
    collections::{BTreeMap, BTreeSet},
    io::{Read, Write},
    net::TcpListener,
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
    thread,
    time::Duration,
};

struct Fixture(PathBuf);
impl Fixture {
    fn new() -> std::io::Result<Self> {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let path = std::env::temp_dir().join(format!(
            "uos-reverse-tls-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir(&path)?;
        std::fs::create_dir(path.join("replay"))?;
        std::fs::write(path.join("outside"), b"synthetic-outside")?;
        Ok(Self(path))
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

fn identity() -> TestResult<NativeTransportIdentity> {
    NativeTransportIdentity::new(
        "fixture-key".into(),
        "fixture-owner".into(),
        "fixture-epoch".into(),
        [17; 32],
        BTreeSet::from([NativeCapability::OwnerRenewal]),
    )
    .must()
}
fn read_request(stream: &mut impl Read) -> Option<(BTreeMap<String, String>, Vec<u8>)> {
    let mut bytes = Vec::new();
    loop {
        if bytes.len() > 110_000 {
            return None;
        }
        if let Some(end) = bytes.windows(4).position(|part| part == b"\r\n\r\n") {
            let head = std::str::from_utf8(&bytes[..end]).ok()?;
            let mut lines = head.split("\r\n");
            assert_eq!(
                lines.next()?,
                format!("POST {NATIVE_RENEWAL_AUTHORITY_PATH} HTTP/1.1")
            );
            let headers: BTreeMap<_, _> = lines
                .filter_map(|line| line.split_once(':'))
                .map(|(key, value)| (key.to_ascii_lowercase(), value.trim().to_owned()))
                .collect();
            let length = headers.get("content-length")?.parse::<usize>().ok()?;
            if bytes.len() >= end + 4 + length {
                return Some((headers, bytes[end + 4..end + 4 + length].to_vec()));
            }
        }
        let mut buffer = [0u8; 4096];
        let count = stream.read(&mut buffer).ok()?;
        if count == 0 {
            return None;
        }
        bytes.extend_from_slice(&buffer[..count]);
    }
}

#[test]
fn actual_reverse_tls_uses_preopened_fd_after_os_network_denial_and_verifies_mac_domain()
-> TestResult {
    for scenario in [(false, false), (true, false), (true, true)] {
        run_owned_case(
            "authority_http::tests::isolated_reverse_tls_scenario_child",
            &serde_json::to_vec(&scenario)?,
            "REVERSE_TLS_CHILD_ACCEPTED",
        )?;
    }
    Ok(())
}
#[test]
#[ignore = "fixed actual scenario child selected only by the original acceptance parent"]
fn isolated_reverse_tls_scenario_child() -> TestResult {
    let (trusted, wrong_domain): (bool, bool) = serde_json::from_reader(std::io::stdin().lock())?;
    let fixture = Fixture::new()?;
    let listener = TcpListener::bind("127.0.0.1:0")?;
    let address = listener.local_addr()?;
    let socket = TcpStream::connect(address)?;
    let peer = thread::spawn(move || -> ThreadResult<bool> {
        let (socket, _) = listener.accept().map_err(text)?;
        socket
            .set_read_timeout(Some(Duration::from_secs(3)))
            .map_err(text)?;
        socket
            .set_write_timeout(Some(Duration::from_secs(3)))
            .map_err(text)?;
        let cert = CertificateDer::from(
            include_bytes!("../../tests/fixtures/native-tls/leaf.der").to_vec(),
        );
        let key = PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(
            include_bytes!("../../tests/fixtures/native-tls/leaf.pk8.der").to_vec(),
        ));
        let config = ServerConfig::builder()
            .with_no_client_auth()
            .with_single_cert(vec![cert], key)
            .map_err(text)?;
        let mut tls = StreamOwned::new(
            ServerConnection::new(Arc::new(config)).map_err(text)?,
            socket,
        );
        let Some((headers, body)) = read_request(&mut tls) else {
            return Ok(false);
        };
        assert!(matches!(
            Request::parse(&body).map_err(text)?,
            Request::Unknown { .. }
        ));
        let nonce = headers
            .get(NATIVE_NONCE_HEADER)
            .ok_or("nonce header is missing")?;
        let reply = serde_json::to_vec(&Response::Unknown { v: 1 }).map_err(text)?;
        let mac = if wrong_domain {
            identity()
                .map_err(text)?
                .sign_control_response(nonce, &body, 200, &reply)
        } else {
            identity()
                .map_err(text)?
                .sign_authority_response(nonce, &body, 200, &reply)
        }
        .map_err(text)?;
        let header = format!(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\n{NATIVE_RESPONSE_MAC_HEADER}: {mac}\r\nConnection: close\r\n\r\n",
            reply.len()
        );
        tls.write_all(header.as_bytes()).map_err(text)?;
        tls.write_all(&reply).map_err(text)?;
        tls.flush().map_err(text)?;
        Ok(true)
    });
    let ca =
        ureq::tls::Certificate::from_der(include_bytes!("../../tests/fixtures/native-tls/ca.der"))
            .to_owned();
    let roots = if trusted {
        ureq::tls::RootCerts::new_with_certs(&[ca])
    } else {
        ureq::tls::RootCerts::WebPki
    };
    let carrier = NativeAuthorityHttp::with_roots(
        &format!("https://{address}{NATIVE_RENEWAL_AUTHORITY_PATH}"),
        socket,
        identity()?,
        roots,
    )
    .must()?;
    let outside = fixture.0.join("outside");
    let replay = fixture.0.join("replay");
    let client = thread::spawn(move || -> ThreadResult<bool> {
        assert!(std::fs::read(&outside).is_ok());
        observe_kernel_contract();
        enforce(&NativeConfinement {
            readonly_paths: vec![],
            list_directories: vec![],
            replay_directory: replay,
            unix_sockets: vec![],
        })
        .map_err(text)?;
        assert!(std::fs::read(&outside).is_err());
        assert!(TcpStream::connect(address).is_err());
        assert!(std::process::Command::new("/bin/true").status().is_err());
        let disconnected = || false;
        let context = NativeControlContext::admitted(
            Instant::now() + Duration::from_secs(5),
            &disconnected,
            NativeAuthHeaders {
                version: "1",
                key_id: "fixture-key",
                time_ms: "1",
                nonce: "fixture-only",
                mac: "fixture-only",
            },
        );
        Ok(carrier
            .exchange(
                &Request::Unknown {
                    v: 1,
                    operation_id: "fixture-operation".into(),
                    claim_ref: "fixture-claim".into(),
                },
                &context,
            )
            .is_ok())
    });
    assert_eq!(client.join().must()??, trusted && !wrong_domain);
    assert_eq!(peer.join().must()??, trusted);
    println!("REVERSE_TLS_CHILD_ACCEPTED");
    Ok(())
}
