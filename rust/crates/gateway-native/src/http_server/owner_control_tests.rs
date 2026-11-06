//! Real confined HTTP/parser/MAC/capability fixture. This synthetic handler
//! deliberately returns unknown and does not pretend to publish credentials.
use super::*;
use crate::confinement::{NativeConfinement, enforce};
use crate::owned_case::{
    Must, TestResult, ThreadResult, observe_kernel_contract, run_owned_case, text,
};
use gateway_auth::{crypto::base64url, native_auth::AuthenticatedNativeOwner};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::{collections::BTreeSet, path::PathBuf, sync::atomic::AtomicU64};

struct Fixture {
    root: PathBuf,
    pairs: std::sync::Mutex<std::collections::VecDeque<(TcpStream, TcpStream)>>,
    worker: std::sync::Mutex<Option<ControlWorker>>,
}
struct Exchange {
    service: Arc<NativeHttpService>,
    stream: TcpStream,
    done: std::sync::mpsc::Sender<()>,
}
struct ControlWorker {
    sender: std::sync::mpsc::Sender<Exchange>,
    join: std::thread::JoinHandle<ThreadResult>,
}
impl Fixture {
    fn new() -> TestResult<Self> {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let path = std::env::temp_dir().join(format!(
            "uos-owner-control-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir(&path)?;
        std::fs::create_dir(path.join("replay"))?;
        std::fs::write(path.join("outside"), b"synthetic-outside-file")?;
        let listener = TcpListener::bind("127.0.0.1:0")?;
        let mut pairs = std::collections::VecDeque::new();
        for _ in 0..2 {
            let client = TcpStream::connect(listener.local_addr()?)?;
            let (server, _) = listener.accept()?;
            pairs.push_back((client, server));
        }
        Ok(Self {
            root: path,
            pairs: std::sync::Mutex::new(pairs),
            worker: std::sync::Mutex::new(None),
        })
    }
    fn service(&self, identity: NativeTransportIdentity) -> TestResult<NativeHttpService> {
        Ok(NativeHttpService::configured(
            identity,
            SqliteNonceStore::open(&self.root.join("replay/nonces.sqlite3")).must()?,
            SupervisorData::configured(Vec::new()).must()?,
        ))
    }
    fn exchange(&self, service: Arc<NativeHttpService>, wire: &[u8]) -> TestResult<Vec<u8>> {
        // The original fixture uses at most two exchanges.
        let (mut client, stream) = self.pairs.lock().must()?.pop_front().must()?;
        let mut worker = self.worker.lock().must()?;
        if worker.is_none() {
            let private = self.root.join("replay");
            let outside = self.root.join("outside");
            let (sender, receiver) = std::sync::mpsc::channel::<Exchange>();
            let join = std::thread::spawn(move || control_worker(&outside, private, &receiver));
            *worker = Some(ControlWorker { sender, join });
        }
        let (done, completed) = std::sync::mpsc::channel();
        let exchange = Exchange {
            service,
            stream,
            done,
        };
        worker.as_ref().must()?.sender.send(exchange).must()?;
        drop(worker);
        client.set_read_timeout(Some(Duration::from_secs(5)))?;
        client.write_all(wire)?;
        let mut response = Vec::new();
        client.read_to_end(&mut response)?;
        completed.recv_timeout(Duration::from_secs(5))?;
        Ok(response)
    }
}
fn control_worker(
    outside: &std::path::Path,
    private: PathBuf,
    receiver: &std::sync::mpsc::Receiver<Exchange>,
) -> ThreadResult {
    assert!(std::fs::read(outside).is_ok());
    observe_kernel_contract();
    enforce(&NativeConfinement {
        readonly_paths: vec![],
        list_directories: vec![],
        replay_directory: private,
        unix_sockets: vec![],
    })
    .map_err(text)?;
    assert!(std::fs::read(outside).is_err());
    assert!(std::process::Command::new("/bin/true").status().is_err());
    while let Ok(exchange) = receiver.recv() {
        let Exchange {
            service,
            mut stream,
            done,
        } = exchange;
        if let Err(error) = service.handle(&mut stream) {
            write_error(&mut stream, error).map_err(text)?;
        }
        drop(stream);
        done.send(()).map_err(text)?;
    }
    Ok(())
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let worker = self.worker.get_mut().ok().and_then(Option::take);
        if let Some(ControlWorker { sender, join }) = worker {
            drop(sender);
            let result = join.join();
            assert!(
                std::thread::panicking() || matches!(result, Ok(Ok(()))),
                "control worker failed: {result:?}"
            );
        }
        let _ = std::fs::remove_dir_all(&self.root);
    }
}

fn identity(capability: NativeCapability) -> TestResult<NativeTransportIdentity> {
    NativeTransportIdentity::new(
        "fixture-key".into(),
        "fixture-owner".into(),
        "fixture-epoch".into(),
        [11; 32],
        BTreeSet::from([capability]),
    )
    .must()
}
fn request(identity: &NativeTransportIdentity, nonce: &str, body: &[u8]) -> TestResult<Vec<u8>> {
    let now = now_ms().must()?;
    let mac = identity
        .sign(NATIVE_CONTROL_PATH, now, nonce, body)
        .must()?;
    let mut request = format!("POST {NATIVE_CONTROL_PATH} HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: {}\r\n{NATIVE_VERSION_HEADER}: 1\r\n{NATIVE_KEY_HEADER}: fixture-key\r\n{NATIVE_TIME_HEADER}: {now}\r\n{NATIVE_NONCE_HEADER}: {nonce}\r\n{NATIVE_MAC_HEADER}: {mac}\r\n\r\n", body.len()).into_bytes();
    request.extend_from_slice(body);
    Ok(request)
}
const BODY: &[u8] = br#"{"v":1,"operation_id":"fixture-operation","request":{"operation":"codex_renew_access","action_ref":"fixture-action"}}"#;

struct UnknownHandler(Arc<AtomicUsize>);
impl NativeOwnerRenewalHandler for UnknownHandler {
    fn renew(
        &self,
        proof: &AuthenticatedNativeOwner,
        request: &NativeControlEnvelope,
        raw: &[u8],
        context: &NativeControlContext<'_>,
    ) -> Result<NativeControlResponse, GatewayError> {
        context.check()?;
        proof.require_request(
            NativeCapability::OwnerRenewal,
            NATIVE_CONTROL_PATH,
            raw,
            context.now_ms()?,
        )?;
        assert_eq!(request.operation_id, "fixture-operation");
        self.0.fetch_add(1, Ordering::Relaxed);
        Err(GatewayError::unavailable(
            "synthetic-secret-must-not-escape",
            "fixture-unclassified-error",
        ))
    }
}

#[test]
fn confined_control_http_authenticates_unknown_result_and_redacts_handler_error() -> TestResult {
    run_owned_case(
        "http_server::owner_control_tests::isolated_control_case_child",
        b"0",
        "CONTROL_CHILD_ACCEPTED",
    )?;
    Ok(())
}
fn confined_control_http_authenticates_unknown_result_and_redacts_handler_error_body() -> TestResult
{
    let fixture = Fixture::new()?;
    let identity = identity(NativeCapability::OwnerRenewal)?;
    let calls = Arc::new(AtomicUsize::new(0));
    let service = Arc::new(
        fixture
            .service(identity.clone())?
            .with_owner_renewal(UnknownHandler(calls.clone())),
    );
    let nonce = base64url(&[7; 32]);
    let request = request(&identity, &nonce, BODY)?;
    let wire = fixture.exchange(service.clone(), &request)?;
    let end = wire
        .windows(4)
        .position(|part| part == b"\r\n\r\n")
        .must()?;
    let headers = std::str::from_utf8(&wire[..end]).must()?;
    assert!(headers.starts_with("HTTP/1.1 200 "));
    let mac = headers
        .lines()
        .find_map(|line| {
            line.split_once(": ")
                .filter(|(name, _)| name.eq_ignore_ascii_case(NATIVE_RESPONSE_MAC_HEADER))
                .map(|(_, value)| value)
        })
        .must()?;
    let body = &wire[end + 4..];
    identity
        .verify_control_response(&nonce, BODY, 200, body, mac)
        .must()?;
    assert!(
        identity
            .verify_publication_response(&nonce, BODY, 200, body, mac)
            .is_err()
    );
    let result: NativeControlResponse = serde_json::from_slice(body).must()?;
    result
        .validate_for(&NativeControlEnvelope::parse(BODY).must()?)
        .must()?;
    assert_eq!(result.disposition, NativeControlDisposition::Unknown);
    assert!(!String::from_utf8_lossy(&wire).contains("synthetic-secret"));
    assert_eq!(calls.load(Ordering::Relaxed), 1);
    let replay = fixture.exchange(service, &request)?;
    assert!(String::from_utf8_lossy(&replay).starts_with("HTTP/1.1 409 "));
    assert_eq!(calls.load(Ordering::Relaxed), 1);
    Ok(())
}

#[test]
fn confined_supervisor_capability_and_strict_control_body_refuse_before_handler() -> TestResult {
    run_owned_case(
        "http_server::owner_control_tests::isolated_control_case_child",
        b"1",
        "CONTROL_CHILD_ACCEPTED",
    )?;
    Ok(())
}
fn confined_supervisor_capability_and_strict_control_body_refuse_before_handler_body() -> TestResult
{
    let fixture = Fixture::new()?;
    let read_identity = identity(NativeCapability::SupervisorRead)?;
    let calls = Arc::new(AtomicUsize::new(0));
    let service = Arc::new(
        fixture
            .service(read_identity.clone())?
            .with_owner_renewal(UnknownHandler(calls.clone())),
    );
    let wire = fixture.exchange(
        service,
        &request(&read_identity, &base64url(&[8; 32]), BODY)?,
    )?;
    assert!(String::from_utf8_lossy(&wire).starts_with("HTTP/1.1 403 "));
    let renew_identity = identity(NativeCapability::OwnerRenewal)?;
    let service = Arc::new(
        fixture
            .service(renew_identity.clone())?
            .with_owner_renewal(UnknownHandler(calls.clone())),
    );
    let invalid = br#"{"v":1,"operation_id":"fixture-operation","request":{"operation":"codex_renew_access","action_ref":"fixture-action","authorized":true}}"#;
    let wire = fixture.exchange(
        service,
        &request(&renew_identity, &base64url(&[9; 32]), invalid)?,
    )?;
    assert!(String::from_utf8_lossy(&wire).starts_with("HTTP/1.1 400 "));
    assert_eq!(calls.load(Ordering::Relaxed), 0);
    Ok(())
}

#[test]
fn confined_control_rejects_content_length_before_reading_an_oversized_body() -> TestResult {
    run_owned_case(
        "http_server::owner_control_tests::isolated_control_case_child",
        b"2",
        "CONTROL_CHILD_ACCEPTED",
    )?;
    Ok(())
}
fn confined_control_rejects_content_length_before_reading_an_oversized_body_body() -> TestResult {
    let fixture = Fixture::new()?;
    let service = Arc::new(fixture.service(identity(NativeCapability::OwnerRenewal)?)?);
    let wire = fixture.exchange(
        service,
        format!(
            "POST {NATIVE_CONTROL_PATH} HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\n\r\n",
            NATIVE_CONTROL_MAX_BYTES + 1
        )
        .as_bytes(),
    )?;
    assert!(String::from_utf8_lossy(&wire).starts_with("HTTP/1.1 413 "));
    Ok(())
}

#[test]
#[ignore = "fixed actual case child selected only by the original acceptance parents"]
fn isolated_control_case_child() -> TestResult {
    let case: u8 = serde_json::from_reader(std::io::stdin().lock()).must()?;
    match case {
        0 => confined_control_http_authenticates_unknown_result_and_redacts_handler_error_body()?,
        1 => confined_supervisor_capability_and_strict_control_body_refuse_before_handler_body()?,
        2 => confined_control_rejects_content_length_before_reading_an_oversized_body_body()?,
        _ => panic!("unknown fixed native control fixture case"),
    }
    println!("CONTROL_CHILD_ACCEPTED");
    Ok(())
}
