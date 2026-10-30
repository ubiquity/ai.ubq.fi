use gateway_auth_contracts::{
    passkeys::{PasskeySession, PasskeyUser},
    types::AuthRequest,
};
use gateway_auth_credentials::provider_credentials::{
    CodexNativeOwner, CodexSourceAccount, CodexSourcePool, MANAGED_NATIVE_SOURCE_VERSION,
    ManagedNativeSource, codex_source_pool_key,
};
use gateway_auth_native_contracts::native_import::catalog_seed::ImportCatalogSeedRequestData;
use gateway_auth_native_contracts::native_import::{NativeImportMetadata, NativeImportTarget};
use gateway_auth_native_contracts::transport::{NativeAuthHeaders, NativeCapability};
use gateway_auth_native_import_data::native_import::authority::ImportAuthorityRequestData;
use gateway_auth_native_proof::native_auth::NativeTransportIdentity;
use gateway_auth_native_wire::native_read::{
    NATIVE_KEY_HEADER, NATIVE_MAC_HEADER, NATIVE_MAX_BODY_BYTES, NATIVE_NONCE_HEADER,
    NATIVE_PUBLICATION_PATH, NATIVE_RESPONSE_MAC_HEADER, NATIVE_TIME_HEADER, NATIVE_VERSION_HEADER,
};
use gateway_auth_passkeys::{passkeys, session_index};
use gateway_auth_primitives::crypto::{Secret, base64url, sha256_hex};
use gateway_authority::native_control::actor;
use gateway_authority::native_control::receiver::NativeRenewalAuthority;
use gateway_authority::store::Authority;
use gateway_authority_keys as keys;
use gateway_authority_native_records::owner::RegisteredRenewalOwner;
use gateway_authority_paid_control_records::PaidControlOwner;
use gateway_authority_routing_records::{types::PoolAccount, types::SubscriptionPool};
use gateway_contracts::{
    identity::PoolVersionstamp, identity::PrincipalId, identity::RequestContext,
    identity::RequestId,
};
use gateway_core::state::AuthorityStore;
use gateway_native_import_test_support::fixture as base;
use gateway_publication_policy::control_publication::{
    ConfiguredPaidInputs, PreparedConfigurationPublication,
};
use gateway_publication_policy::protocol::{OwnedNativeAuthHeaders, PublicationRequest};
use gateway_publication_transport::transport::PublicationReceiver;
use gateway_source::{error::GatewayError, source_json::SourceJson};
use rustls::{ServerConfig, ServerConnection, StreamOwned, pki_types::*};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs,
    io::{Read, Write},
    net::{TcpListener, TcpStream},
    os::unix::{
        fs::{OpenOptionsExt, PermissionsExt},
        process::CommandExt,
    },
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicUsize, Ordering},
    },
    thread,
    time::{Duration, Instant},
};

use std::sync::Condvar;
pub struct FlowTokens;
impl gateway_authority_contracts::TokenSource for FlowTokens {
    fn next_token(&self) -> Result<String, GatewayError> {
        static NEXT: AtomicUsize = AtomicUsize::new(1);
        Ok(format!(
            "native-flow-token-{}",
            NEXT.fetch_add(1, Ordering::SeqCst)
        ))
    }
}
pub fn authority(store: &base::Store) -> Authority<base::Store, FlowTokens> {
    Authority::with_clock(store.clone(), FlowTokens, store.clone())
}
pub const MODELS: &[u8] = br#"{"models":[{"slug":"synthetic-model","display_name":"Synthetic","supported_reasoning_levels":[{"effort":"none","description":"None"}]}]}"#;
pub fn key(parts: &[&str]) -> gateway_kv_codec::key::RecordKey {
    gateway_kv_codec::tuple_key::TupleKey::from_scalar_parts(
        &parts
            .iter()
            .map(|part| gateway_kv_codec::tuple_key::ScalarKeyPart::String(part))
            .collect::<Vec<_>>(),
    )
    .to_record_key()
}
pub const OLD: &[u8] = br#"{"tokens":{"access_token":"SYNTHETIC_OLD_ACCESS","refresh_token":"SYNTHETIC_OLD_REFRESH","account_id":"synthetic-account"},"OPENAI_API_KEY":"SYNTHETIC_INDEPENDENT","unrelated":"\ud800"}"#;
pub const NEW: &[u8] = br#"{"auth":{"tokens":{"access_token":"SYNTHETIC_NEW_ACCESS","refresh_token":"SYNTHETIC_NEW_REFRESH","account_id":"synthetic-account"}},"models":{"client_version":"0.163.0"}}"#;
pub fn now() -> i64 {
    gateway_auth_native_proof::native_context::now_ms().unwrap()
}
pub fn id() -> NativeTransportIdentity {
    NativeTransportIdentity::new(
        "native-flow".into(),
        "fixture-host".into(),
        "epoch".into(),
        [83; 32],
        BTreeSet::from([
            NativeCapability::CodexAuthImport,
            NativeCapability::AccessPublish,
        ]),
    )
    .unwrap()
}
pub fn credentials() -> gateway_auth_credentials::native_credentials::NativeCredentials {
    gateway_native_import_source::documents::credentials(&SourceJson::parse_bytes(OLD).unwrap())
        .unwrap()
}
pub fn inputs() -> ConfiguredPaidInputs {
    ConfiguredPaidInputs::new(
        None,
        None,
        None,
        serde_json::json!({"fixture":"genuine-native-flow"}),
    )
    .unwrap()
}
pub fn setup(store: &base::Store, home: &Path) -> Result<String, GatewayError> {
    let identity = id();
    store.seed(
        keys::paid_control_owner(),
        "owner-before",
        PaidControlOwner {
            v: 1,
            owner_id: identity.owner_id().into(),
            owner_epoch: identity.owner_epoch().into(),
            key_fingerprint: identity.key_fingerprint().into(),
        },
    )?;
    let old = credentials();
    let mut account = CodexSourceAccount {
        account_id: old.account_id.clone(),
        access_token: old.access_token.clone(),
        refresh_token: Secret::new(String::new()),
        updated_at_ms: now() as f64,
        native_owner: Some(CodexNativeOwner {
            codex_home: home.to_str().ok_or_else(base::fail)?.into(),
            generation_hash: String::new(),
            native_refreshed_at: None,
            external_user_id: None,
            managed_source: Some(ManagedNativeSource {
                v: MANAGED_NATIVE_SOURCE_VERSION,
                generation_hash: old.generation_hash(),
            }),
        }),
    };
    let projected = account.identity();
    account
        .native_owner
        .as_mut()
        .ok_or_else(base::fail)?
        .generation_hash = projected.native_generation_hash.clone();
    let hash = projected.routing_account_hash.0.clone();
    store.seed(
        codex_source_pool_key(),
        "source-before",
        CodexSourcePool {
            accounts: vec![account],
            updated_at_ms: now() as f64,
        }
        .private_source_value(),
    )?;
    store.seed(
        keys::pool(),
        "pool-before",
        SubscriptionPool {
            v: 1,
            pool_versionstamp: PoolVersionstamp("source-before".into()),
            accounts: vec![PoolAccount {
                account_id_hash: projected.routing_account_hash,
                admin_account_hash: projected.administrative_account_hash,
                credential_version: projected.credential_version,
                enabled: true,
            }],
        },
    )?;
    let captured = futures_executor::block_on(PreparedConfigurationPublication::capture(
        store,
        &identity,
        &inputs(),
        "config-before",
        now(),
    ))
    .map_err(|_| base::fail())?;
    store.seed(
        keys::paid_control_config(),
        "config-before",
        captured.configuration(),
    )?;
    Ok(hash)
}
pub fn admin_actor(store: &base::Store) -> Result<actor::NativeRenewalActor, GatewayError> {
    let token = "uos_ai_session_SYNTHETIC_FLOW";
    store.seed(
        passkeys::key("users", "flow-user"),
        "user-before",
        PasskeyUser {
            id: "flow-user".into(),
            handle: "operator".into(),
            is_admin: true,
            credential_ids: vec!["credential".into()],
            created_at_ms: 1,
            updated_at_ms: 1,
        },
    )?;
    let session = PasskeySession {
        token: token.into(),
        user_id: "flow-user".into(),
        created_at_ms: 1,
        expires_at_ms: now() + 600_000,
        audience_origin: None,
    };
    store.seed(passkeys::key("sessions", token), "session-before", &session)?;
    store.rows.lock().map_err(|_| base::fail())?.insert(
        session_index::index_key(&session_index::digest(token))
            .map_err(|_| base::fail())?
            .0,
        session_index::record_for_session(&session, "session-before", None)
            .map_err(|_| base::fail())?,
    );
    let authority = authority(store);
    let config = base::config();
    let request = AuthRequest {
        method: "POST".into(),
        url: "https://ai.ubq.fi/v1/responses".into(),
        headers: BTreeMap::from([("authorization".into(), format!("Bearer {token}"))]),
        ..Default::default()
    };
    let context = RequestContext {
        request_id: RequestId("native-flow-original-request".into()),
        principal: PrincipalId("passkey-user:flow-user".into()),
        key_id: None,
        route: "responses".into(),
        started_at_ms: now(),
    };
    futures_executor::block_on(actor::authenticate_admin_actor(
        &authority,
        &base::Host,
        &config,
        &request,
        &context,
    ))
    .map_err(|_| base::fail())
}
#[derive(Default)]
pub struct Traffic {
    pub authority: Mutex<Vec<Vec<u8>>>,
    pub models: AtomicUsize,
    pub oauth: AtomicUsize,
    pub drop_publication: AtomicBool,
    pub pause: Mutex<Option<Arc<PhasePause>>>,
    pub accepted: [AtomicUsize; 3],
    pub accepted_nonblocking: [AtomicUsize; 3],
    pub tls_complete: [AtomicUsize; 3],
    pub requests: [AtomicUsize; 3],
    pub phases: Mutex<Vec<&'static str>>,
    pub errors: Mutex<Vec<String>>,
}
impl Traffic {
    pub fn retain(&self, path: &Path) {
        let counts = |values: &[AtomicUsize; 3]| {
            values
                .iter()
                .map(|n| n.load(Ordering::SeqCst))
                .collect::<Vec<_>>()
        };
        let value = serde_json::json!({"accepted":counts(&self.accepted),"accepted_nonblocking":counts(&self.accepted_nonblocking),"tls_complete":counts(&self.tls_complete),"requests":counts(&self.requests),
            "closed_phases":self.phases.lock().unwrap().iter().take(64).copied().collect::<Vec<_>>(),"error_codes":self.errors.lock().unwrap().iter().take(64).cloned().collect::<Vec<_>>()});
        let mut file = fs::OpenOptions::new()
            .create_new(true)
            .write(true)
            .mode(0o600)
            .open(path)
            .unwrap();
        file.write_all(&serde_json::to_vec(&value).unwrap())
            .unwrap();
    }
}
pub struct Servers {
    pub authority: std::net::SocketAddr,
    pub models: std::net::SocketAddr,
    pub oauth: std::net::SocketAddr,
    stop: Arc<AtomicBool>,
    joins: Vec<thread::JoinHandle<()>>,
    pub traffic: Arc<Traffic>,
    structural_path: PathBuf,
}
impl Servers {
    pub fn start(store: base::Store, home: PathBuf) -> Self {
        let stop = Arc::new(AtomicBool::new(false));
        let traffic = Arc::new(Traffic::default());
        let mut joins = Vec::new();
        let mut addresses = Vec::new();
        for role in 0..3 {
            let listener = TcpListener::bind((std::net::Ipv4Addr::LOCALHOST, 0)).unwrap();
            addresses.push(listener.local_addr().unwrap());
            listener.set_nonblocking(true).unwrap();
            let stop = stop.clone();
            let traffic = traffic.clone();
            let store = store.clone();
            let home = home.clone();
            joins.push(thread::spawn(move || {
                let mut tasks = Vec::new();
                while !stop.load(Ordering::Acquire) {
                    match listener.accept() {
                        Ok((socket, _)) => {
                            traffic.accepted[role as usize].fetch_add(1, Ordering::SeqCst);
                            if rustix::fs::fcntl_getfl(&socket)
                                .unwrap()
                                .contains(rustix::fs::OFlags::NONBLOCK)
                            {
                                traffic.accepted_nonblocking[role as usize]
                                    .fetch_add(1, Ordering::SeqCst);
                            }
                            let store = store.clone();
                            let home = home.clone();
                            let traffic = traffic.clone();
                            tasks.push(thread::spawn(move || {
                                serve_tls(socket, role, store, &home, traffic)
                            }));
                        }
                        Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                            thread::sleep(Duration::from_millis(2))
                        }
                        Err(e) => panic!("fixture listener {e}"),
                    }
                }
                for task in tasks {
                    task.join().unwrap();
                }
            }));
        }
        Self {
            authority: addresses[0],
            models: addresses[1],
            oauth: addresses[2],
            stop,
            joins,
            traffic,
            structural_path: home.parent().unwrap().join("server-structural.json"),
        }
    }
}
impl Drop for Servers {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
        for join in self.joins.drain(..) {
            join.join().unwrap();
        }
        self.traffic.retain(&self.structural_path);
    }
}
pub fn tls(socket: TcpStream) -> StreamOwned<ServerConnection, TcpStream> {
    socket.set_nonblocking(false).unwrap();
    assert!(
        !rustix::fs::fcntl_getfl(&socket)
            .unwrap()
            .contains(rustix::fs::OFlags::NONBLOCK),
        "actual accepted fixture TLS socket must be blocking before handshake"
    );
    socket
        .set_read_timeout(Some(Duration::from_secs(55)))
        .unwrap();
    socket
        .set_write_timeout(Some(Duration::from_secs(5)))
        .unwrap();
    let cert = include_bytes!("../fixtures/import-tls/leaf.der");
    let key = include_bytes!("../fixtures/import-tls/leaf.pk8.der");
    let config = ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(
            vec![CertificateDer::from(cert.to_vec())],
            PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(key.to_vec())),
        )
        .unwrap();
    StreamOwned::new(ServerConnection::new(Arc::new(config)).unwrap(), socket)
}
pub fn read_request(
    tls: &mut StreamOwned<ServerConnection, TcpStream>,
    traffic: &Traffic,
    role: usize,
    tls_recorded: &mut bool,
) -> Option<(String, BTreeMap<String, String>, Vec<u8>)> {
    let mut bytes = Vec::new();
    loop {
        if let Some(end) = bytes.windows(4).position(|p| p == b"\r\n\r\n") {
            let text = std::str::from_utf8(&bytes[..end]).ok()?;
            let mut lines = text.split("\r\n");
            let path = lines.next()?.split_whitespace().nth(1)?.to_owned();
            let headers: BTreeMap<String, String> = lines
                .map(|line| line.split_once(':'))
                .collect::<Option<Vec<_>>>()?
                .into_iter()
                .map(|(k, v)| (k.to_ascii_lowercase(), v.trim().to_owned()))
                .collect();
            let len: usize = headers
                .get("content-length")
                .map(|v| v.parse().ok())
                .unwrap_or(Some(0))?;
            if len
                > gateway_auth_native_wire::publication_command::import_publication_max_bytes()
                    .max(gateway_auth_native_wire::native_read::NATIVE_MAX_BODY_BYTES)
            {
                return None;
            }
            if bytes.len() >= end + 4 + len {
                return Some((path, headers, bytes[end + 4..end + 4 + len].to_vec()));
            }
        }
        let mut part = [0u8; 8192];
        let read = tls.read(&mut part);
        if !*tls_recorded && !tls.conn.is_handshaking() {
            traffic.tls_complete[role].fetch_add(1, Ordering::SeqCst);
            *tls_recorded = true;
        }
        let n = match read {
            Ok(n) => n,
            Err(error) => {
                let code = match error
                    .get_ref()
                    .and_then(|e| e.downcast_ref::<rustls::Error>())
                {
                    Some(rustls::Error::AlertReceived(alert)) => format!("tls_alert_{alert:?}"),
                    _ => format!("tls_io_{:?}", error.kind()),
                };
                traffic.errors.lock().unwrap().push(code);
                return None;
            }
        };
        if n == 0 {
            return None;
        }
        bytes.extend_from_slice(&part[..n]);
    }
}
pub fn serve_tls(
    socket: TcpStream,
    role: u8,
    store: base::Store,
    home: &Path,
    traffic: Arc<Traffic>,
) {
    let mut stream = tls(socket);
    let mut tls_recorded = false;
    while let Some((path, headers, body)) =
        read_request(&mut stream, &traffic, role as usize, &mut tls_recorded)
    {
        traffic.requests[role as usize].fetch_add(1, Ordering::SeqCst);
        let phase = if path == NATIVE_PUBLICATION_PATH {
            "publication"
        } else if let Ok(request) = ImportAuthorityRequestData::parse(&body) {
            use gateway_auth_native_import_data::native_import::authority::ImportAuthorityCommand as C;
            match request.command {
                C::Admit { .. } => "admit",
                C::FinishCurrent { .. } => "finish",
                C::Current { .. } => "current",
                C::Claim { .. } => "claim",
                C::Start { .. } => "start",
                C::Unknown { .. } => "unknown",
                C::Terminal { .. } => "terminal",
                C::CustodyTerminal { .. } => "custody_terminal",
            }
        } else if ImportCatalogSeedRequestData::parse(&body).is_ok() {
            "catalog_seed"
        } else if role == 1 {
            "models"
        } else if role == 2 {
            "oauth"
        } else {
            "started_handler"
        };
        traffic.phases.lock().unwrap().push(phase);
        let pause = traffic.pause.lock().unwrap().clone();
        if let Some(pause) = pause {
            pause.reached(phase);
        }
        let (status, mut response, response_headers) = if role == 0 {
            traffic.authority.lock().unwrap().push(body.clone());
            let identity = id();
            let authority = authority(&store);
            let config = base::config();
            let owner = RegisteredRenewalOwner::configured(
                "native-owner".into(),
                home.to_str().unwrap().into(),
            )
            .unwrap();
            let cancel = base::Cancel::default();
            let control =
                NativeRenewalAuthority::configured(&authority, &identity, &owner, &config, &cancel);
            let owned = OwnedNativeAuthHeaders::from_pairs(headers.clone()).unwrap();
            if path == NATIVE_PUBLICATION_PATH {
                let receiver =
                    PublicationReceiver::configured(id(), inputs(), Vec::new(), now).unwrap();
                let reply = futures_executor::block_on(receiver.handle_import(
                    &control,
                    PublicationRequest {
                        method: "POST".into(),
                        path: path.clone(),
                        headers: owned,
                        raw_body: body.clone(),
                    },
                ));
                if traffic.drop_publication.swap(false, Ordering::SeqCst) && reply.status == 200 {
                    return;
                }
                (reply.status, reply.raw_body, reply.headers)
            } else {
                let result = futures_executor::block_on(async {
                    let peer = control
                        .authenticate_import_authority(
                            NativeAuthHeaders {
                                version: &headers[NATIVE_VERSION_HEADER],
                                key_id: &headers[NATIVE_KEY_HEADER],
                                time_ms: &headers[NATIVE_TIME_HEADER],
                                nonce: &headers[NATIVE_NONCE_HEADER],
                                mac: &headers[NATIVE_MAC_HEADER],
                            },
                            &body,
                        )
                        .await?;
                    if ImportCatalogSeedRequestData::parse(&body).is_ok() {
                        control.handle_import_catalog_seed(&peer, &body).await
                    } else if ImportAuthorityRequestData::parse(&body).is_ok() {
                        control.handle_import_authority(&peer, &body).await
                    } else {
                        control.handle_import_started(&peer, &body).await
                    }
                });
                let (status, reply) = match result {
                    Ok(reply) => (200, reply),
                    Err(error) => {
                        traffic.errors.lock().unwrap().push(
                            error
                                .envelope
                                .error
                                .code
                                .clone()
                                .unwrap_or_else(|| "no_code".into()),
                        );
                        (error.status, serde_json::to_vec(&error.envelope).unwrap())
                    }
                };
                let nonce = headers.get(NATIVE_NONCE_HEADER).unwrap();
                let signature = identity
                    .sign_authority_response(nonce, &body, status, &reply)
                    .unwrap();
                (
                    status,
                    reply,
                    vec![(NATIVE_RESPONSE_MAC_HEADER.into(), signature)],
                )
            }
        } else if role == 1 {
            traffic.models.fetch_add(1, Ordering::SeqCst);
            (200, MODELS.to_vec(), Vec::new())
        } else {
            traffic.oauth.fetch_add(1, Ordering::SeqCst);
            (200,br#"{"access_token":"SYNTHETIC_ROTATED_ACCESS","refresh_token":"SYNTHETIC_ROTATED_REFRESH"}"#.to_vec(),Vec::new())
        };
        let mut head = format!(
            "HTTP/1.1 {status} Fixture\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: {}\r\nConnection: keep-alive\r\n",
            response.len()
        );
        for (k, v) in response_headers {
            head.push_str(&format!("{k}: {v}\r\n"));
        }
        head.push_str("\r\n");
        if stream
            .write_all(head.as_bytes())
            .and_then(|_| stream.write_all(&response))
            .and_then(|_| stream.flush())
            .is_err()
        {
            return;
        }
        response.fill(0);
    }
}
pub struct PhasePause {
    phase: &'static str,
    occurrence: usize,
    seen: AtomicUsize,
    state: Mutex<(bool, bool)>,
    changed: Condvar,
}
pub struct PauseHandle(Arc<PhasePause>);
impl PhasePause {
    pub fn reached(&self, phase: &'static str) {
        if phase != self.phase || self.seen.fetch_add(1, Ordering::SeqCst) + 1 != self.occurrence {
            return;
        }
        let mut state = self.state.lock().unwrap();
        state.0 = true;
        self.changed.notify_all();
        let (state, limit) = self
            .changed
            .wait_timeout_while(state, Duration::from_secs(55), |state| !state.1)
            .unwrap();
        assert!(
            !limit.timed_out() && state.1,
            "controlled server pause was not released"
        );
    }
}
impl PauseHandle {
    pub fn install(servers: &Servers, phase: &'static str, occurrence: usize) -> Self {
        let pause = Arc::new(PhasePause {
            phase,
            occurrence,
            seen: AtomicUsize::new(0),
            state: Mutex::new((false, false)),
            changed: Condvar::new(),
        });
        *servers.traffic.pause.lock().unwrap() = Some(pause.clone());
        Self(pause)
    }
    pub fn wait(&self) {
        let (state, timeout) = self
            .0
            .changed
            .wait_timeout_while(
                self.0.state.lock().unwrap(),
                Duration::from_secs(35),
                |state| !state.0,
            )
            .unwrap();
        assert!(
            !timeout.timed_out() && state.0,
            "real native flow did not reach the selected authority phase"
        );
    }
    pub fn resume(&self) {
        self.0.state.lock().unwrap().1 = true;
        self.0.changed.notify_all();
    }
}
impl Drop for PauseHandle {
    fn drop(&mut self) {
        self.resume();
    }
}
