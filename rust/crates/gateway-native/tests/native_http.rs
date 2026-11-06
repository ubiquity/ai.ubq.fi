#![cfg(target_os = "linux")]

use gateway_auth::{
    crypto::base64url,
    native::{NATIVE_FOLLOW_PATH, NATIVE_MAX_BODY_BYTES, NATIVE_RPC_PATH},
    native_auth::{NativeCapability, NativeTransportIdentity},
};
use serde_json::{Value, json};
use std::{
    collections::BTreeSet,
    fs,
    io::{BufRead, BufReader, Read, Write},
    net::{SocketAddr, TcpStream},
    os::unix::net::{UnixListener, UnixStream},
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicUsize, Ordering},
        mpsc,
    },
    thread,
    time::Duration,
};

type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;
/// Converts a missing value or a failed result into the test's error instead of a panic.
trait Must<T> {
    fn must(self) -> TestResult<T>;
}
impl<T> Must<T> for Option<T> {
    fn must(self) -> TestResult<T> {
        self.ok_or_else(|| "missing value".into())
    }
}
impl<T, E: std::fmt::Debug> Must<T> for Result<T, E> {
    fn must(self) -> TestResult<T> {
        self.map_err(|error| format!("{error:?}").into())
    }
}
/// Fixture threads report failures as text because a boxed error cannot cross threads.
type ThreadResult = Result<(), String>;
fn text(error: impl std::fmt::Debug) -> String {
    format!("{error:?}")
}

struct Fixture(PathBuf);
impl Fixture {
    fn new() -> std::io::Result<Self> {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let path = std::env::temp_dir().join(format!(
            "uos-http-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&path)?;
        fs::create_dir(path.join("replay"))?;
        Ok(Self(path))
    }
    fn configuration(&self) -> Value {
        json!({"v":1,"listen":"127.0.0.1:0","identity":{"key_id":"fixture-key","owner_id":"fixture-owner","owner_epoch":"fixture-epoch","key_base64url":base64url(&[11;32])},"nonce_database":self.0.join("replay/nonces.sqlite3"),"sources":[{"id":"fixture","name":"Synthetic Machine","socket_path":self.0.join("source.sock"),"codex_home":null}]})
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

struct Peer {
    socket: PathBuf,
    stop: Arc<AtomicBool>,
    follow_calls: Arc<AtomicUsize>,
    join: Option<thread::JoinHandle<ThreadResult>>,
}
impl Peer {
    fn start(path: &Path) -> std::io::Result<Self> {
        let listener = UnixListener::bind(path)?;
        let stop = Arc::new(AtomicBool::new(false));
        let stopped = stop.clone();
        let follow_calls = Arc::new(AtomicUsize::new(0));
        let calls = follow_calls.clone();
        let join = thread::spawn(move || {
            let mut connections = Vec::new();
            for stream in listener.incoming() {
                let stream = stream.map_err(text)?;
                if stopped.load(Ordering::Acquire) {
                    break;
                }
                let calls = calls.clone();
                connections.push(thread::spawn(move || serve_peer(stream, &calls)));
            }
            for connection in connections {
                connection.join().map_err(text)??;
            }
            Ok(())
        });
        Ok(Self {
            socket: path.into(),
            stop,
            follow_calls,
            join: Some(join),
        })
    }
}
fn serve_peer(stream: UnixStream, calls: &AtomicUsize) -> ThreadResult {
    stream
        .set_read_timeout(Some(Duration::from_secs(3)))
        .map_err(text)?;
    stream
        .set_write_timeout(Some(Duration::from_secs(3)))
        .map_err(text)?;
    let Ok(mut socket) = tungstenite::accept(stream) else {
        return Ok(());
    };
    while let Ok(message) = socket.read() {
        if message.is_close() {
            let _ = socket.flush();
            break;
        }
        let Ok(value) = serde_json::from_slice::<Value>(&message.into_data()) else {
            continue;
        };
        let Some(id) = value.get("id") else {
            continue;
        };
        let method = value["method"]
            .as_str()
            .ok_or("native fixture method is missing")?;
        let result = peer_result(method, calls)?;
        let reply = json!({"id":id,"result":result}).to_string();
        if socket
            .send(tungstenite::Message::Text(reply.into()))
            .is_err()
        {
            break;
        }
    }
    Ok(())
}
fn peer_result(method: &str, calls: &AtomicUsize) -> Result<Value, String> {
    Ok(match method {
        "initialize" => json!({"protocolVersion":1}),
        "thread/loaded/list" => json!({"data":["thread-a"]}),
        "account/rateLimits/read" => {
            json!({"rateLimits":{"usedPercent":35,"windowDurationMins":300,"resetsAt":1700000100}})
        }
        "thread/list" => {
            json!({"data":[{"id":"thread-a","name":"fixture session","source":"cli","updatedAt":1700000001000i64}],"nextCursor":null})
        }
        "thread/read" => {
            json!({"thread":{"id":"thread-a","status":{"type":"active"},"updatedAt":1700000002000i64}})
        }
        "thread/turns/list" => {
            calls.fetch_add(1, Ordering::AcqRel);
            json!({"data":[{"id":"turn-a","status":"inProgress","items":[{"type":"agentMessage","id":"visible-message","text":"fixture-visible password=synthetic-hidden-value"},{"type":"reasoning","id":"private-message","text":"fixture-private-reasoning"}]}],"nextCursor":null})
        }
        other => return Err(format!("unexpected native fixture method: {other}")),
    })
}
impl Drop for Peer {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
        let _ = UnixStream::connect(&self.socket);
        let outcome = self.join.take().map(thread::JoinHandle::join);
        assert!(
            matches!(outcome, Some(Ok(Ok(())))),
            "native fixture peer failed: {outcome:?}"
        );
    }
}

struct Daemon {
    child: Child,
    address: Option<SocketAddr>,
    reader: Option<thread::JoinHandle<()>>,
}
impl Daemon {
    fn start(configuration: &Value) -> TestResult<Self> {
        let child = Command::new(env!("CARGO_BIN_EXE_uos-native"))
            .env_clear()
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()?;
        let mut daemon = Self {
            child,
            address: None,
            reader: None,
        };
        daemon
            .child
            .stdin
            .take()
            .must()?
            .write_all(configuration.to_string().as_bytes())?;
        let stdout = daemon.child.stdout.take().must()?;
        let (send, receive) = mpsc::channel();
        daemon.reader = Some(thread::spawn(move || {
            // An unreadable line stays empty and fails the readiness receipt below.
            let mut line = String::new();
            let _ = BufReader::new(stdout).read_line(&mut line);
            let _ = send.send(line);
        }));
        let line = receive.recv_timeout(Duration::from_secs(10)).must()?;
        let event: Value = serde_json::from_str(&line)?;
        assert_eq!(event["event"], "native_listener_ready");
        daemon.address = Some(event["address"].as_str().must()?.parse()?);
        daemon.reader.take().must()?.join().must()?;
        Ok(daemon)
    }
    fn connect(&self) -> TestResult<TcpStream> {
        let stream = TcpStream::connect(self.address.must()?)?;
        stream.set_read_timeout(Some(Duration::from_secs(5)))?;
        stream.set_write_timeout(Some(Duration::from_secs(5)))?;
        Ok(stream)
    }
    fn exchange(&self, request: &[u8]) -> TestResult<Vec<u8>> {
        let mut stream = self.connect()?;
        stream.write_all(request)?;
        let mut response = Vec::new();
        stream.read_to_end(&mut response)?;
        Ok(response)
    }
}
impl Drop for Daemon {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        if let Some(reader) = self.reader.take() {
            // The reader thread cannot panic; its line is consumed by `start`.
            let _ = reader.join();
        }
    }
}

fn signed_request(path: &str, body: &Value, nonce: u8, chunked: bool) -> TestResult<Vec<u8>> {
    let identity = NativeTransportIdentity::new(
        "fixture-key".into(),
        "fixture-owner".into(),
        "fixture-epoch".into(),
        [11; 32],
        BTreeSet::from([NativeCapability::SupervisorRead]),
    )
    .must()?;
    let body = serde_json::to_vec(body)?;
    let now = gateway_native::http_server::now_ms().must()?;
    let nonce = base64url(&[nonce; 32]);
    let mac = identity.sign(path, now, &nonce, &body).must()?;
    let framing = if chunked {
        "Transfer-Encoding: chunked".into()
    } else {
        format!("Content-Length: {}", body.len())
    };
    let mut request = format!("POST {path} HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\n{framing}\r\nx-uos-native-version: 1\r\nx-uos-native-key-id: fixture-key\r\nx-uos-native-time-ms: {now}\r\nx-uos-native-nonce: {nonce}\r\nx-uos-native-mac: {mac}\r\n\r\n").into_bytes();
    if chunked {
        for chunk in body.chunks(17) {
            request.extend_from_slice(format!("{:x}\r\n", chunk.len()).as_bytes());
            request.extend_from_slice(chunk);
            request.extend_from_slice(b"\r\n");
        }
        request.extend_from_slice(b"0\r\n\r\n");
    } else {
        request.extend_from_slice(&body);
    }
    Ok(request)
}
fn response(bytes: &[u8]) -> TestResult<(u16, Value)> {
    let boundary = bytes
        .windows(4)
        .position(|part| part == b"\r\n\r\n")
        .must()?;
    let head = std::str::from_utf8(&bytes[..boundary])?;
    let status = head.split_ascii_whitespace().nth(1).must()?.parse()?;
    Ok((status, serde_json::from_slice(&bytes[boundary + 4..])?))
}

#[test]
fn actual_confined_daemon_serves_authenticated_http_sse_and_persistent_replay_fences() -> TestResult
{
    let fixture = Fixture::new()?;
    let peer = Peer::start(&fixture.0.join("source.sock"))?;
    let configuration = fixture.configuration();
    let daemon = Daemon::start(&configuration)?;
    let sessions = json!({"v":1,"request":{"operation":"supervisor_sessions"}});
    let original = signed_request(NATIVE_RPC_PATH, &sessions, 1, true)?;
    let (status, body) = response(&daemon.exchange(&original)?)?;
    assert_eq!(status, 200);
    assert_eq!(body["status"], 200);
    assert!(body["body"].to_string().contains("thread-a"));
    let (status, replay) = response(&daemon.exchange(&original)?)?;
    assert_ne!(status, 200);
    assert_eq!(replay["error"]["code"], "native_nonce_replayed");

    let output = json!({"v":1,"request":{"operation":"supervisor_output","source_id":"fixture","thread_id":"thread-a","cursor":null}});
    let (status, output) =
        response(&daemon.exchange(&signed_request(NATIVE_RPC_PATH, &output, 2, false)?)?)?;
    assert_eq!(status, 200);
    assert_eq!(output["status"], 200);
    assert_eq!(output["stream"]["kind"], "supervisor_follow");
    let mut follow = daemon.connect()?;
    follow
        .write_all(&signed_request(
            NATIVE_FOLLOW_PATH,
            &output["stream"],
            3,
            true,
        )?)
        .must()?;
    let mut observed = Vec::new();
    while !String::from_utf8_lossy(&observed)
        .split("event: entries")
        .nth(1)
        .is_some_and(|event| event.contains("\n\n"))
    {
        let mut buffer = [0; 4096];
        let length = follow.read(&mut buffer).must()?;
        assert_ne!(length, 0, "follow must provide actual SSE entries");
        observed.extend_from_slice(&buffer[..length]);
        assert!(observed.len() < 65_536);
    }
    let events = String::from_utf8(observed).must()?;
    assert!(events.starts_with("HTTP/1.1 200"));
    assert!(events.contains("Content-Type: text/event-stream"));
    assert!(events.contains("event: ready"));
    assert!(events.contains("fixture-visible"));
    assert!(!events.contains("synthetic-hidden-value"));
    assert!(!events.contains("fixture-private-reasoning"));
    drop(follow);
    thread::sleep(Duration::from_millis(200));
    let stopped = peer.follow_calls.load(Ordering::Acquire);
    thread::sleep(Duration::from_millis(2_200));
    assert_eq!(
        peer.follow_calls.load(Ordering::Acquire),
        stopped,
        "disconnect must stop follow polling"
    );

    let request = signed_request(NATIVE_RPC_PATH, &sessions, 4, false)?;
    let duplicate = String::from_utf8(request.clone()).must()?.replacen(
        "\r\n\r\n",
        "\r\nx-uos-native-nonce: duplicate\r\n\r\n",
        1,
    );
    assert_ne!(response(&daemon.exchange(duplicate.as_bytes())?)?.0, 200);
    assert_eq!(response(&daemon.exchange(&request)?)?.0, 200);
    let oversized = format!(
        "POST {NATIVE_RPC_PATH} HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\n\r\n",
        NATIVE_MAX_BODY_BYTES + 1
    );
    assert_eq!(response(&daemon.exchange(oversized.as_bytes())?)?.0, 413);
    let unknown = json!({"v":1,"request":{"operation":"supervisor_output","source_id":"not-configured","thread_id":"thread-a","cursor":null}});
    let (_, refusal) =
        response(&daemon.exchange(&signed_request(NATIVE_RPC_PATH, &unknown, 5, false)?)?)?;
    assert_ne!(refusal["status"], 200);
    drop(daemon);
    let restarted = Daemon::start(&configuration)?;
    let (_, replay) = response(&restarted.exchange(&original)?)?;
    assert_eq!(replay["error"]["code"], "native_nonce_replayed");
    assert_eq!(
        response(&restarted.exchange(&signed_request(NATIVE_RPC_PATH, &sessions, 6, false)?)?)?.0,
        200
    );
    Ok(())
}

use gateway_auth::{
    native::NATIVE_CONTROL_PATH,
    native_auth::AuthenticatedNativeOwner,
    native_control::{
        NativeControlDisposition, NativeControlEnvelope, NativeControlReason, NativeControlRequest,
        NativeControlResponse,
    },
};
use gateway_core::error::GatewayError;
use gateway_native::{
    http_server::NativeHttpService,
    nonce_sqlite::SqliteNonceStore,
    owner_control::{NativeControlContext, NativeOwnerRenewalHandler},
    supervisor_data::SupervisorData,
};
use gateway_native_runtime::lifecycle::Recovery;
use std::{net::TcpListener, sync::Mutex};

#[test]
fn owner_loss_drains_started_control_once_before_closing_listener() -> TestResult {
    struct Pending {
        entered: mpsc::Sender<()>,
        release: Mutex<mpsc::Receiver<()>>,
        calls: Arc<AtomicUsize>,
    }
    impl NativeOwnerRenewalHandler for Pending {
        fn renew(
            &self,
            _: &AuthenticatedNativeOwner,
            request: &NativeControlEnvelope,
            _: &[u8],
            _: &NativeControlContext<'_>,
        ) -> Result<NativeControlResponse, GatewayError> {
            self.calls.fetch_add(1, Ordering::SeqCst);
            let failed =
                || GatewayError::unavailable("Fixture channel failed", "fixture_channel_failed");
            self.entered.send(()).map_err(|_| failed())?;
            self.release
                .lock()
                .map_err(|_| failed())?
                .recv()
                .map_err(|_| failed())?;
            Ok(NativeControlResponse {
                v: 1,
                operation_id: request.operation_id.clone(),
                disposition: NativeControlDisposition::Unknown,
                publication_revision: None,
                reason: Some(NativeControlReason::OutcomeUnknown),
            })
        }
    }
    let fixture = Fixture::new()?;
    let auth = fixture.0.join("auth.synthetic");
    fs::write(&auth, "synthetic")?;
    let upstream_listener = TcpListener::bind("127.0.0.1:0")?;
    let upstream = TcpStream::connect(upstream_listener.local_addr()?)?;
    let peer = upstream_listener.accept()?.0;
    let recovery = Recovery::prepare(&[auth], &[&upstream])?;
    let listener = TcpListener::bind("127.0.0.1:0")?;
    let address = listener.local_addr()?;
    let identity = NativeTransportIdentity::new(
        "fixture-key".into(),
        "fixture-owner".into(),
        "fixture-epoch".into(),
        [11; 32],
        BTreeSet::from([NativeCapability::OwnerRenewal]),
    )
    .must()?;
    let (entered, ready) = mpsc::channel();
    let (release, wait) = mpsc::channel();
    let calls = Arc::new(AtomicUsize::new(0));
    let service = NativeHttpService::configured(
        identity,
        SqliteNonceStore::open(&fixture.0.join("replay/nonces.sqlite3")).must()?,
        SupervisorData::configured(vec![]).must()?,
    )
    .with_owner_renewal(Pending {
        entered,
        release: Mutex::new(wait),
        calls: calls.clone(),
    });
    let (finished, result) = mpsc::channel();
    let server = thread::spawn(move || {
        let _ = finished.send(service.serve_recoverable(listener, Some(recovery)));
    });
    let frame = serde_json::to_value(NativeControlEnvelope {
        v: 1,
        operation_id: "fixture-started-operation".into(),
        request: NativeControlRequest::CodexRenewAccess {
            action_ref: "fixture-action".into(),
        },
    })?;
    let request = signed_request(NATIVE_CONTROL_PATH, &frame, 91, false)?;
    let client = thread::spawn(move || -> std::io::Result<Vec<u8>> {
        let mut stream = TcpStream::connect(address)?;
        stream.set_read_timeout(Some(Duration::from_secs(5)))?;
        stream.write_all(&request)?;
        let mut bytes = Vec::new();
        stream.read_to_end(&mut bytes)?;
        Ok(bytes)
    });
    ready.recv_timeout(Duration::from_secs(3)).must()?;
    drop(peer);
    assert!(matches!(
        result.recv_timeout(Duration::from_millis(250)),
        Err(mpsc::RecvTimeoutError::Timeout)
    ));
    assert!(TcpStream::connect(address).is_err());
    release.send(()).must()?;
    let bytes = client.join().must()?.must()?;
    assert_eq!(response(&bytes)?.1["disposition"], "unknown");
    let error = result
        .recv_timeout(Duration::from_secs(3))
        .must()?
        .err()
        .must()?;
    assert_eq!(
        error.envelope.error.code.as_deref(),
        Some("native_owner_lifetime_lost")
    );
    server.join().must()?;
    assert!(TcpStream::connect(address).is_err());
    assert_eq!(calls.load(Ordering::SeqCst), 1);
    Ok(())
}
