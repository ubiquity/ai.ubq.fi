#![cfg(target_os = "linux")]
use serde_json::{Value, json};
use std::{
    fs,
    io::{Read, Write},
    net::{TcpListener, TcpStream},
    os::unix::{
        fs::PermissionsExt,
        net::{UnixListener, UnixStream},
    },
    path::PathBuf,
    process::{Command, Stdio},
    sync::atomic::{AtomicUsize, Ordering},
    thread,
};
use tungstenite::Message;
type TestResult<T = ()> = Result<T, Box<dyn std::error::Error + Send + Sync>>;
trait Must<T> {
    fn must(self) -> TestResult<T>;
}
impl<T> Must<T> for Option<T> {
    fn must(self) -> TestResult<T> {
        self.ok_or_else(|| "missing fixture value".into())
    }
}
impl<T, E: std::fmt::Debug> Must<T> for Result<T, E> {
    fn must(self) -> TestResult<T> {
        self.map_err(|e| format!("{e:?}").into())
    }
}
struct Fixture(PathBuf);
impl Fixture {
    fn new() -> TestResult<Self> {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let root = std::env::temp_dir().join(format!(
            "p{:x}-{:x}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&root)?;
        fs::set_permissions(&root, fs::Permissions::from_mode(0o700))?;
        fs::create_dir(root.join("app-server-control"))?;
        fs::set_permissions(
            root.join("app-server-control"),
            fs::Permissions::from_mode(0o700),
        )?;
        fs::write(root.join("auth.json"), "PRIVATE_FIXTURE_CANARY")?;
        Ok(Self(fs::canonicalize(root)?))
    }
    fn socket(&self) -> PathBuf {
        self.0.join("app-server-control/app-server-control.sock")
    }
    fn input(&self) -> Value {
        json!({"v":1,"codex_home":self.0,"expected_uid":rustix::process::geteuid().as_raw()})
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
fn invoke(input: &[u8], args: &[&str]) -> TestResult<std::process::Output> {
    let mut child = Command::new(env!("CARGO_BIN_EXE_uos-native-owner-probe"))
        .args(args)
        .env_clear()
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()?;
    child.stdin.take().must()?.write_all(input)?;
    Ok(child.wait_with_output()?)
}
fn peer(
    fixture: &Fixture,
    account: Value,
    requires: Value,
    wrong_home: bool,
) -> TestResult<thread::JoinHandle<TestResult<Vec<Value>>>> {
    let listener = UnixListener::bind(fixture.socket())?;
    fs::set_permissions(fixture.socket(), fs::Permissions::from_mode(0o600))?;
    let home = if wrong_home {
        fixture.0.join("different")
    } else {
        fixture.0.clone()
    };
    Ok(thread::spawn(move || {
        let (stream, _) = listener.accept()?;
        drop(listener);
        stream.set_read_timeout(Some(std::time::Duration::from_secs(5)))?;
        let mut ws = tungstenite::accept(stream).must()?;
        let mut messages = Vec::new();
        while let Ok(frame) = ws.read() {
            let Message::Text(text) = frame else {
                break;
            };
            let value: Value = serde_json::from_str(&text)?;
            let id = value.get("id").cloned();
            let method = value
                .get("method")
                .and_then(Value::as_str)
                .must()?
                .to_owned();
            messages.push(value.clone());
            let result = match method.as_str() {
                "initialize" => json!({"codexHome":home}),
                "initialized" => continue,
                "account/read" => {
                    assert_eq!(value["params"], json!({"refreshToken":false}));
                    json!({"account":account,"requiresOpenaiAuth":requires})
                }
                _ => return Err("unexpected RPC".into()),
            };
            ws.send(Message::Text(
                json!({"id":id.must()?,"result":result}).to_string().into(),
            ))
            .must()?;
        }
        Ok(messages)
    }))
}
#[test]
fn same_fd_passive_receipt_is_closed_and_truthful() -> TestResult {
    for (account, requires, kind) in [
        (
            json!({"type":"chatgpt","email":"PRIVATE_FIXTURE_EMAIL"}),
            true,
            "chatgpt",
        ),
        (json!({"type":"chatgpt"}), false, "chatgpt"),
        (Value::Null, false, "masked"),
    ] {
        let fixture = Fixture::new()?;
        let server = peer(&fixture, account, json!(requires), false)?;
        let output = invoke(&serde_json::to_vec(&fixture.input())?, &[])?;
        assert!(
            output.status.success(),
            "{}",
            String::from_utf8_lossy(&output.stderr)
        );
        let receipt: Value = serde_json::from_slice(&output.stdout)?;
        assert_eq!(receipt.as_object().must()?.len(), 6);
        assert_eq!(receipt["v"], 1);
        assert_eq!(receipt["codex_home"], json!(fixture.0));
        assert_eq!(receipt["peer_uid"], rustix::process::geteuid().as_raw());
        assert_eq!(receipt["peer_pid"], std::process::id());
        assert_eq!(receipt["account_kind"], kind);
        assert_eq!(receipt["requires_openai_auth"], requires);
        assert!(!String::from_utf8_lossy(&output.stdout).contains("PRIVATE_FIXTURE"));
        let messages = server.join().must()??;
        assert_eq!(
            messages
                .iter()
                .map(|v| v["method"].as_str())
                .collect::<Vec<_>>(),
            vec![
                Some("initialize"),
                Some("initialized"),
                Some("account/read")
            ]
        );
        assert_eq!(
            fs::read_to_string(fixture.0.join("auth.json"))?,
            "PRIVATE_FIXTURE_CANARY"
        );
    }
    Ok(())
}
#[test]
fn refuses_other_modes_malformed_metadata_and_wrong_home() -> TestResult {
    for (account, requires, wrong_home) in [
        (json!({"type":"apiKey"}), json!(true), false),
        (json!({"type":"other"}), json!(false), false),
        (Value::Null, json!(true), false),
        (json!({"type":"chatgpt"}), Value::Null, false),
        (json!({"type":"chatgpt"}), json!(true), true),
    ] {
        let fixture = Fixture::new()?;
        let server = peer(&fixture, account, requires, wrong_home)?;
        let output = invoke(&serde_json::to_vec(&fixture.input())?, &[])?;
        assert!(!output.status.success());
        assert!(output.stdout.is_empty());
        let messages = server.join().must()??;
        assert!(messages.iter().all(|v| matches!(
            v["method"].as_str(),
            Some("initialize" | "initialized" | "account/read")
        )));
    }
    Ok(())
}
#[test]
fn closed_input_refuses_unknown_fields_uid_and_cli_arguments() -> TestResult {
    let fixture = Fixture::new()?;
    let listener = UnixListener::bind(fixture.socket())?;
    fs::set_permissions(fixture.socket(), fs::Permissions::from_mode(0o600))?;
    listener.set_nonblocking(true)?;
    let mut unknown = fixture.input();
    unknown["method"] = json!("account/login/start");
    let mut uid = fixture.input();
    uid["expected_uid"] = json!(rustix::process::geteuid().as_raw() + 1);
    for value in [unknown, uid, json!({"v":1})] {
        let output = invoke(&serde_json::to_vec(&value)?, &[])?;
        assert!(!output.status.success());
        assert!(output.stdout.is_empty());
        assert!(matches!(
            listener.accept(),
            Err(error) if error.kind() == std::io::ErrorKind::WouldBlock
        ));
    }
    assert!(
        !invoke(&serde_json::to_vec(&fixture.input())?, &["--help"])?
            .status
            .success()
    );
    assert!(!invoke(&[b' '; 4097], &[])?.status.success());
    Ok(())
}
#[test]
fn readonly_profile_keeps_exact_socket_but_denies_files_tcp_and_exec() -> TestResult {
    let fixture = Fixture::new()?;
    let allowed = UnixListener::bind(fixture.socket())?;
    let other_path = fixture.0.join("other.sock");
    let _other = UnixListener::bind(&other_path)?;
    let tcp = TcpListener::bind("127.0.0.1:0")?;
    let input = json!({"home":fixture.0,"socket":fixture.socket(),"other":other_path,"tcp":tcp.local_addr()?.to_string()});
    let mut child = Command::new(std::env::current_exe()?)
        .args([
            "--ignored",
            "--exact",
            "readonly_probe_child",
            "--nocapture",
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()?;
    child
        .stdin
        .take()
        .must()?
        .write_all(&serde_json::to_vec(&input)?)?;
    let output = child.wait_with_output()?;
    assert!(
        output.status.success(),
        "{} {}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(String::from_utf8_lossy(&output.stdout).contains("READONLY_OWNER_PROBE_ACCEPTED"));
    drop(allowed);
    assert_eq!(
        fs::read_to_string(fixture.0.join("auth.json"))?,
        "PRIVATE_FIXTURE_CANARY"
    );
    Ok(())
}
#[test]
#[ignore = "entered only by the matching kernel acceptance parent"]
fn readonly_probe_child() -> TestResult {
    let mut bytes = Vec::new();
    std::io::stdin().read_to_end(&mut bytes)?;
    let input: Value = serde_json::from_slice(&bytes)?;
    let path =
        |name: &str| -> TestResult<PathBuf> { Ok(PathBuf::from(input[name].as_str().must()?)) };
    let home = path("home")?;
    let socket = path("socket")?;
    let other = path("other")?;
    let tcp = input["tcp"].as_str().must()?;
    assert!(fs::read(home.join("auth.json")).is_ok());
    assert!(UnixStream::connect(&other).is_ok());
    assert!(TcpStream::connect(tcp).is_ok());
    gateway_native::confinement::enforce_readonly_socket(&socket).must()?;
    assert!(fs::read(home.join("auth.json")).is_err());
    assert!(fs::write(home.join("auth.json"), "must not write").is_err());
    assert!(fs::write(home.join("new"), "must not create").is_err());
    assert!(UnixStream::connect(&socket).is_ok());
    assert!(UnixStream::connect(&other).is_err());
    assert!(TcpStream::connect(tcp).is_err());
    assert!(Command::new("/bin/true").status().is_err());
    println!("READONLY_OWNER_PROBE_ACCEPTED");
    Ok(())
}
