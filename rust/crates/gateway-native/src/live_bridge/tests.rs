type TestResult<T> = Result<T, Box<dyn std::error::Error + Send + Sync>>;
use super::socket::LiveDialContext;
use super::{authority::LiveAuthorization, *};
use gateway_auth::{
    native_auth::{
        NativeAuthHeaders, NativeCapability, NativeNonceAuthority, NativeTransportIdentity,
    },
    native_live::{NATIVE_LIVE_ACTION_HEADER, NativeLiveAuthenticator},
};
use gateway_auth_native_wire::native_read::{
    NATIVE_KEY_HEADER, NATIVE_MAC_HEADER, NATIVE_NONCE_HEADER, NATIVE_TIME_HEADER,
    NATIVE_VERSION_HEADER,
};
use gateway_authority::native_live::wire::NativeLiveRetirement;
use gateway_authority::native_live::wire::{NativeLiveAccess, NativeLiveFence};
use gateway_native_https_test_support::live_tls::configs;
use rustls::{ServerConnection, StreamOwned, pki_types::ServerName};
use std::{
    collections::BTreeSet,
    io::Read,
    net::{SocketAddr, TcpListener},
    sync::atomic::AtomicUsize,
};
use std::{sync::atomic::Ordering, thread};
use tungstenite::protocol::frame::coding::CloseCode;
use tungstenite::{
    client::IntoClientRequest,
    handshake::server::{Callback, ErrorResponse, Request, Response},
};

struct Nonces;
impl NativeNonceAuthority for Nonces {
    async fn consume_nonce(&self, _: &str, _: &str, _: i64, _: i64) -> Result<(), GatewayError> {
        Ok(())
    }
}
fn identity() -> Result<NativeTransportIdentity, GatewayError> {
    NativeTransportIdentity::new(
        "fixture-live".into(),
        "owner".into(),
        "epoch".into(),
        [37; 32],
        BTreeSet::from([NativeCapability::LiveSideband]),
    )
}
fn access() -> Result<NativeLiveAccess, serde_json::Error> {
    serde_json::from_str(r#"{"access_token":"synthetic-access"}"#)
}
fn fence() -> NativeLiveFence {
    NativeLiveFence {
        call_id: "rtc_call/one".into(),
        account_id: "synthetic-account".into(),
        account_id_hash: "hash".into(),
        credential_version: "generation".into(),
        source_revision: "source".into(),
        normalized_revision: "normalized".into(),
        mapping_revision: "mapping".into(),
        owner_id: "owner".into(),
        owner_epoch: "epoch".into(),
        owner_revision: "owner-revision".into(),
        principal: "api-key:fixture".into(),
        principal_policy_digest: "policy".into(),
        access_expires_at_ms: None,
    }
}
#[derive(Default)]
struct Authority {
    retired: AtomicUsize,
}
impl NativeLiveAuthority for Authority {
    fn authorize(
        &self,
        action_ref: &str,
        connection_id: &str,
        _: Instant,
    ) -> Result<LiveAuthorization, GatewayError> {
        Ok(LiveAuthorization {
            action_ref: action_ref.into(),
            connection_id: connection_id.into(),
            authorization_ref: "authorization".into(),
            fence: fence(),
            access: Some(access().map_err(|_| unavailable())?),
        })
    }
    fn recheck(
        &self,
        _: &LiveAuthorization,
        _: Instant,
    ) -> Result<(NativeLiveFence, NativeLiveAccess), GatewayError> {
        Ok((fence(), access().map_err(|_| unavailable())?))
    }
    fn retire(
        &self,
        _: &LiveAuthorization,
        status: u16,
        _: Instant,
    ) -> Result<NativeLiveRetirement, GatewayError> {
        assert!(status == 404 || status == 410);
        self.retired.fetch_add(1, Ordering::SeqCst);
        Ok(NativeLiveRetirement::Retired)
    }
}
struct Connector {
    address: SocketAddr,
    calls: Arc<AtomicUsize>,
}
impl NativeLiveConnector for Connector {
    fn connect(&self, context: &LiveDialContext) -> Result<TcpStream, GatewayError> {
        context.check()?;
        self.calls.fetch_add(1, Ordering::SeqCst);
        TcpStream::connect_timeout(&self.address, context.remaining()?).map_err(|_| unavailable())
    }
}
fn read_head(stream: &mut TcpStream) -> TestResult<(Vec<(String, String)>, Vec<u8>)> {
    stream.set_read_timeout(Some(Duration::from_secs(5)))?;
    let mut bytes = Vec::new();
    loop {
        if let Some(end) = bytes.windows(4).position(|s| s == b"\r\n\r\n") {
            let head = std::str::from_utf8(&bytes[..end])?;
            assert_eq!(head.lines().next(), Some("GET /v1/native/live HTTP/1.1"));
            let headers = head
                .lines()
                .skip(1)
                .map(|line| -> TestResult<_> {
                    let (k, v) = line
                        .split_once(':')
                        .ok_or_else(|| std::io::Error::other("fixture header delimiter"))?;
                    Ok((k.to_lowercase(), v.trim().into()))
                })
                .collect::<TestResult<Vec<_>>>()?;
            return Ok((headers, bytes[end + 4..].to_vec()));
        }
        let mut chunk = [0; 4096];
        let count = stream.read(&mut chunk)?;
        assert!(count > 0);
        bytes.extend_from_slice(&chunk[..count]);
        assert!(bytes.len() < 65_536);
    }
}
fn start(
    service: NativeLiveService,
) -> TestResult<(SocketAddr, thread::JoinHandle<TestResult<()>>)> {
    let listener = TcpListener::bind("127.0.0.1:0")?;
    let address = listener.local_addr()?;
    let handle = thread::spawn(move || -> TestResult<()> {
        let (mut stream, _) = listener.accept()?;
        let (headers, buffered) = read_head(&mut stream)?;
        let one = |name: &str| -> Result<&str, std::io::Error> {
            headers
                .iter()
                .find(|(key, _)| key == name)
                .map(|(_, value)| value.as_str())
                .ok_or_else(|| std::io::Error::other("fixture header missing"))
        };
        let action_ref = one(NATIVE_LIVE_ACTION_HEADER)?.to_owned();
        let identity = identity()?;
        let proof = futures_executor::block_on(
            NativeLiveAuthenticator::new(&identity, &Nonces).authenticate_upgrade(
                NativeAuthHeaders {
                    version: one(NATIVE_VERSION_HEADER)?,
                    key_id: one(NATIVE_KEY_HEADER)?,
                    time_ms: one(NATIVE_TIME_HEADER)?,
                    nonce: one(NATIVE_NONCE_HEADER)?,
                    mac: one(NATIVE_MAC_HEADER)?,
                },
                &action_ref,
                gateway_native_http_framing::response::now_ms()?,
            ),
        )?;
        assert!(
            service
                .handle(
                    &mut stream,
                    NativeLiveUpgradeRequest {
                        action_ref,
                        headers,
                        buffered
                    },
                    proof
                )
                .is_ok()
        );
        Ok(())
    });
    Ok((address, handle))
}
fn request(address: SocketAddr) -> TestResult<tungstenite::handshake::client::Request> {
    let mut request = format!("ws://{address}/v1/native/live").into_client_request()?;
    let now = gateway_native_http_framing::response::now_ms()?;
    let nonce = base64url(&[19; 32]);
    let identity = identity()?;
    let mac = identity.sign_live_request(now, &nonce, "action-ref")?;
    for (name, value) in [
        (NATIVE_VERSION_HEADER, "1".into()),
        (NATIVE_KEY_HEADER, identity.key_id().into()),
        (NATIVE_TIME_HEADER, now.to_string()),
        (NATIVE_NONCE_HEADER, nonce),
        (NATIVE_MAC_HEADER, mac),
        (NATIVE_LIVE_ACTION_HEADER, "action-ref".into()),
    ] {
        request.headers_mut().insert(
            tungstenite::http::HeaderName::from_bytes(name.as_bytes())?,
            value.parse()?,
        );
    }
    Ok(request)
}

struct FixedUpstreamHeaders;
impl Callback for FixedUpstreamHeaders {
    fn on_request(self, request: &Request, response: Response) -> Result<Response, ErrorResponse> {
        assert_eq!(request.uri().path(), "/v1/live/rtc_call%2Fone");
        assert_eq!(
            request.headers()["authorization"],
            "Bearer synthetic-access"
        );
        assert_eq!(request.headers()["chatgpt-account-id"], "synthetic-account");
        assert_eq!(request.headers()["openai-alpha"], "quicksilver=v2");
        assert!(request.headers().get("sec-websocket-extensions").is_none());
        Ok(response)
    }
}

#[test]
fn actual_tls_upgrade_preserves_fixed_headers_immediate_binary_text_and_close_without_retiring()
-> TestResult<()> {
    let listener = TcpListener::bind("127.0.0.1:0")?;
    let upstream = listener.local_addr()?;
    let (server, tls) = configs()?;
    let peer = thread::spawn(move || -> TestResult<()> {
        let (stream, _) = listener.accept()?;
        stream.set_read_timeout(Some(Duration::from_secs(5)))?;
        thread::sleep(Duration::from_millis(100));
        let tls = StreamOwned::new(ServerConnection::new(server)?, stream);
        let mut socket = tungstenite::accept_hdr_with_config(
            tls,
            FixedUpstreamHeaders,
            Some(gateway_native_live_buffer::queue::websocket_config()),
        )?;
        socket.send(Message::Binary(b"ignored".to_vec().into()))?;
        socket.send(Message::Text("upstream".into()))?;
        assert_eq!(socket.read()?, Message::Text("first".into()));
        socket.send(Message::Text("echo:first".into()))?;
        socket.close(Some(CloseFrame {
            code: CloseCode::Library(3001),
            reason: "closed".into(),
        }))?;
        Ok(())
    });
    let authority = Arc::new(Authority::default());
    let calls = Arc::new(AtomicUsize::new(0));
    let service = NativeLiveService {
        authority: authority.clone(),
        connector: Arc::new(Connector {
            address: upstream,
            calls: calls.clone(),
        }),
        tls,
        server_name: ServerName::try_from("127.0.0.1")?,
    };
    let (address, bridge) = start(service)?;
    let stream = TcpStream::connect(address)?;
    stream.set_read_timeout(Some(Duration::from_secs(5)))?;
    let (mut client, response) = tungstenite::client(request(address)?, stream)?;
    assert_eq!(response.status(), 101);
    client.send(Message::Binary(
        vec![0xef, 0xbb, 0xbf, b'f', b'i', b'r', b's', b't'].into(),
    ))?;
    assert_eq!(client.read()?, Message::Text("upstream".into()));
    assert_eq!(client.read()?, Message::Text("echo:first".into()));
    assert!(
        matches!(client.read()?,Message::Close(Some(frame)) if u16::from(frame.code)==3001&&frame.reason=="closed")
    );
    bridge
        .join()
        .map_err(|_| std::io::Error::other("fixture thread panicked"))??;
    peer.join()
        .map_err(|_| std::io::Error::other("fixture thread panicked"))??;
    assert_eq!(calls.load(Ordering::SeqCst), 1);
    assert_eq!(authority.retired.load(Ordering::SeqCst), 0);
    Ok(())
}

#[test]
fn actual_tls_terminal_404_retires_before_failure_close() -> TestResult<()> {
    let listener = TcpListener::bind("127.0.0.1:0")?;
    let upstream = listener.local_addr()?;
    let (server, tls) = configs()?;
    let peer = thread::spawn(move || -> TestResult<()> {
        let (stream, _) = listener.accept()?;
        stream.set_read_timeout(Some(Duration::from_secs(5)))?;
        let mut tls = StreamOwned::new(ServerConnection::new(server)?, stream);
        let mut bytes = Vec::new();
        while !bytes.windows(4).any(|s| s == b"\r\n\r\n") {
            let mut chunk = [0; 1024];
            let count = tls.read(&mut chunk)?;
            assert!(count > 0);
            bytes.extend_from_slice(&chunk[..count]);
        }
        tls.write_all(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")?;
        tls.flush()?;
        Ok(())
    });
    let authority = Arc::new(Authority::default());
    let service = NativeLiveService {
        authority: authority.clone(),
        connector: Arc::new(Connector {
            address: upstream,
            calls: Arc::new(AtomicUsize::new(0)),
        }),
        tls,
        server_name: ServerName::try_from("127.0.0.1")?,
    };
    let (address, bridge) = start(service)?;
    let stream = TcpStream::connect(address)?;
    stream.set_read_timeout(Some(Duration::from_secs(5)))?;
    let (mut client, _) = tungstenite::client(request(address)?, stream)?;
    assert!(matches!(client.read()?,Message::Close(Some(frame))if u16::from(frame.code)==1011));
    assert_eq!(authority.retired.load(Ordering::SeqCst), 1);
    bridge
        .join()
        .map_err(|_| std::io::Error::other("fixture thread panicked"))??;
    peer.join()
        .map_err(|_| std::io::Error::other("fixture thread panicked"))??;
    Ok(())
}

#[test]
fn actual_source_version_8_pipelined_close_before_dial_makes_zero_upstream_connections()
-> TestResult<()> {
    let (_, tls) = configs()?;
    let calls = Arc::new(AtomicUsize::new(0));
    let service = NativeLiveService {
        authority: Arc::new(Authority::default()),
        connector: Arc::new(Connector {
            address: "127.0.0.1:9".parse()?,
            calls: calls.clone(),
        }),
        tls,
        server_name: ServerName::try_from("127.0.0.1")?,
    };
    let (address, bridge) = start(service)?;
    let mut request = request(address)?;
    request
        .headers_mut()
        .insert("sec-websocket-version", "8".parse()?);
    let mut raw = b"GET /v1/native/live HTTP/1.1\r\n".to_vec();
    for (name, value) in request.headers() {
        raw.extend_from_slice(format!("{name}: {}\r\n", value.to_str()?).as_bytes());
    }
    raw.extend_from_slice(b"\r\n\x88\x80\x01\x02\x03\x04");
    let mut client = TcpStream::connect(address)?;
    client.write_all(&raw)?;
    client.set_read_timeout(Some(Duration::from_secs(5)))?;
    let mut byte = [0; 1];
    let mut head = Vec::new();
    while !head.ends_with(b"\r\n\r\n") {
        assert_eq!(client.read(&mut byte)?, 1);
        head.push(byte[0]);
    }
    assert!(head.starts_with(b"HTTP/1.1 101"));
    bridge
        .join()
        .map_err(|_| std::io::Error::other("fixture thread panicked"))??;
    assert_eq!(calls.load(Ordering::SeqCst), 0);
    Ok(())
}

#[test]
fn actual_socket_error_after_101_is_transport_unknown_without_a_second_http_response()
-> TestResult<()> {
    let listener = TcpListener::bind("127.0.0.1:0")?;
    let address = listener.local_addr()?;
    let server = thread::spawn(move || -> TestResult<()> {
        let (mut stream, _) = listener.accept()?;
        let outcome = response_committed(&mut stream, |stream| {
            stream
                .write_all(b"HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n")
                .map_err(|_| unavailable())?;
            stream.flush().map_err(|_| unavailable())?;
            Err(failure("controlled post-upgrade transport failure"))
        });
        assert_eq!(outcome, NativeLiveOutcome::TransportUnknown);
        Ok(())
    });
    let mut client = TcpStream::connect(address)?;
    client.set_read_timeout(Some(Duration::from_secs(5)))?;
    let mut received = Vec::new();
    client.read_to_end(&mut received)?;
    assert!(received.starts_with(b"HTTP/1.1 101 "));
    assert_eq!(
        received
            .windows(8)
            .filter(|bytes| *bytes == b"HTTP/1.1")
            .count(),
        1
    );
    assert!(received.ends_with(b"\r\n\r\n"));
    server
        .join()
        .map_err(|_| std::io::Error::other("fixture thread panicked"))??;
    Ok(())
}
