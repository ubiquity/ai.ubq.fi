//! Real loopback bytes and real AuthService/M22 proofs. Recording only; no credential effects.
use super::*;
use crate::native_import::{NativeImportContext, NativeImportHandler};
use gateway_auth_contracts::{
    passkeys::{PasskeySession, PasskeyUser},
    types::{AuthConfig, AuthRequest},
};
use gateway_auth_credentials::provider_credentials::{
    CodexNativeOwner, CodexSourceAccount, CodexSourcePool, MANAGED_NATIVE_SOURCE_VERSION,
    ManagedNativeSource, codex_source_pool_key,
};
use gateway_auth_native_contracts::native_import::{
    NATIVE_IMPORT_ACTION_HEADER, NATIVE_IMPORT_BODY_HEADER, NATIVE_IMPORT_OPERATION_HEADER,
    NativeImportKind, NativeImportMetadata, NativeImportTarget,
};
use gateway_auth_native_wire::control::NATIVE_CONTROL_MAX_BYTES;
use gateway_auth_passkeys::{passkeys, session_index};
use gateway_auth_primitives::crypto::{Secret, base64url, sha256_hex};
use gateway_authority::native_control::{
    actor,
    import::AuthenticatedNativeImport,
    receiver::{NativeRenewalAuthority, RegisteredRenewalOwner},
};
use gateway_authority::store::Authority;
use gateway_authority_keys as keys;
use gateway_authority_paid_control_records::{
    PAID_CONTROL_CONFIG_VERSION, PaidControlConfig, PaidControlOwner, PaidSourceCheck,
};
use gateway_authority_routing_records::{types::PoolAccount, types::SubscriptionPool};
use gateway_contracts::{
    identity::PoolVersionstamp, identity::PrincipalId, identity::RequestContext,
    identity::RequestId,
};
use gateway_native_import_source::result::{NativeImportCompletion, NativeImportResult};
use gateway_source::js_string::JsString16;
use gateway_source::source_json::SourceJson;
use gateway_source::source_serialization;
use std::{
    collections::{BTreeMap, BTreeSet},
    fs,
    io::Read,
    net::Shutdown,
    path::PathBuf,
    sync::Mutex,
    thread,
};
const RAW: &[u8] = br#"{"tokens":{"access_token":"SYNTHETIC_ACCESS","refresh_token":"SYNTHETIC_REFRESH","account_id":"synthetic-account"},"extra":"\ud800"}"#;
const HOME: &str = "/tmp/native-import-http-synthetic-owner";
use gateway_native_import_test_support::fixture::{Cancel, Host, Store, Tokens, fail};
#[derive(Default)]
struct Observed {
    documents: Vec<Vec<u8>>,
    source_result: bool,
    client_versions: Vec<Option<JsString16>>,
    account_ids: Vec<JsString16>,
}
struct Recording(Arc<Mutex<Observed>>);
impl NativeImportHandler for Recording {
    fn import_document(
        &self,
        proof: &AuthenticatedNativeImport,
        metadata: &NativeImportMetadata,
        body: &[u8],
        context: &NativeImportContext<'_>,
        original: &crate::owner_control::NativeControlContext<'_>,
    ) -> Result<NativeImportResult, GatewayError> {
        context.require_original(original)?;
        context.check_document(proof, metadata, body)?;
        let mut observed = self.0.lock().map_err(|_| fail())?;
        observed.documents.push(body.to_vec());
        let auth = context.auth_data();
        let tokens = source_serialization::member(auth.auth(), "tokens").ok_or_else(fail)?;
        let account = source_serialization::member(&tokens, "account_id").ok_or_else(fail)?;
        observed
            .account_ids
            .push(source_serialization::string(&account).ok_or_else(fail)?);
        observed
            .client_versions
            .push(auth.client_version().cloned());
        if observed.source_result {
            return Ok(NativeImportResult {
                status: 503,
                headers: BTreeMap::from([("x-uos-upstream".into(), "fixture-native-only".into())]),
                body: SourceJson::parse(r#"{"error":{"message":"native-only\ud800","code":"fixture_effect_unavailable"}}"#).map_err(|_|fail())?,
                completion: NativeImportCompletion::Unavailable,
            });
        }
        Err(GatewayError::unavailable(
            "Recording fixture has no import effect owner",
            "fixture_import_effect_unavailable",
        ))
    }
}
pub(crate) fn config() -> AuthConfig {
    AuthConfig {
        production: true,
        hosted: true,
        git_sha_known: true,
        ..Default::default()
    }
}
pub(crate) fn authority(store: &Store) -> Authority<Store, Tokens> {
    Authority::with_clock(store.clone(), Tokens::default(), store.clone())
}
fn seed(store: &Store, id: &NativeTransportIdentity) -> Result<String, GatewayError> {
    store.seed(
        keys::paid_control_owner(),
        "owner-1",
        PaidControlOwner {
            v: 1,
            owner_id: id.owner_id().into(),
            owner_epoch: id.owner_epoch().into(),
            key_fingerprint: id.key_fingerprint().into(),
        },
    )?;
    let runtime = gateway_auth_primitives::store::key(json!(["uos_ai", "runtime_config", "v2"]));
    store.seed(
        keys::paid_control_config(),
        "config-1",
        PaidControlConfig {
            v: PAID_CONTROL_CONFIG_VERSION,
            owner_id: id.owner_id().into(),
            owner_epoch: id.owner_epoch().into(),
            generation: "fixture-config-generation".into(),
            configuration_fingerprint: "fixture-config".into(),
            surplus_binding: None,
            openlux_discovery_binding: None,
            openlux_binding: None,
            source_checks: vec![PaidSourceCheck {
                key: runtime.0,
                expected_revision: None,
            }],
        },
    )?;
    let mut account = CodexSourceAccount {
        account_id: "synthetic-account".into(),
        access_token: Secret::new("SYNTHETIC_ACCESS".into()),
        refresh_token: Secret::new(String::new()),
        updated_at_ms: 1_000_000.0,
        native_owner: Some(CodexNativeOwner {
            codex_home: HOME.into(),
            generation_hash: sha256_hex(b"native-generation"),
            native_refreshed_at: None,
            external_user_id: None,
            managed_source: Some(ManagedNativeSource {
                v: MANAGED_NATIVE_SOURCE_VERSION,
                generation_hash: sha256_hex(b"physical-native-generation"),
            }),
        }),
    };
    let identity = account.identity();
    account
        .native_owner
        .as_mut()
        .ok_or_else(fail)?
        .generation_hash = identity.native_generation_hash.clone();
    let hash = identity.routing_account_hash.0.clone();
    store.seed(
        codex_source_pool_key(),
        "source-1",
        CodexSourcePool {
            accounts: vec![account],
            updated_at_ms: 1_000_000.0,
        }
        .private_source_value(),
    )?;
    store.seed(
        keys::pool(),
        "pool-1",
        SubscriptionPool {
            v: 1,
            pool_versionstamp: PoolVersionstamp("source-1".into()),
            accounts: vec![PoolAccount {
                account_id_hash: identity.routing_account_hash,
                admin_account_hash: identity.administrative_account_hash,
                credential_version: identity.credential_version,
                enabled: true,
            }],
        },
    )?;
    Ok(hash)
}
fn prepare(
    store: &Store,
    id: &NativeTransportIdentity,
    account: String,
) -> Result<NativeImportMetadata, GatewayError> {
    let token = "uos_ai_session_SYNTHETIC_HTTP_ACTOR";
    store.seed(
        passkeys::key("users", "user-fixture"),
        "user-1",
        PasskeyUser {
            id: "user-fixture".into(),
            handle: "operator".into(),
            is_admin: true,
            credential_ids: vec!["credential".into()],
            created_at_ms: 1,
            updated_at_ms: 1,
        },
    )?;
    let session = PasskeySession {
        token: token.into(),
        user_id: "user-fixture".into(),
        created_at_ms: 1,
        expires_at_ms: now_ms().map_err(|_| fail())? + 600_000,
        audience_origin: None,
    };
    store.seed(passkeys::key("sessions", token), "session-1", &session)?;
    store.rows.lock().map_err(|_| fail())?.insert(
        session_index::index_key(&session_index::digest(token))
            .map_err(|_| fail())?
            .0,
        session_index::record_for_session(&session, "session-1", None).map_err(|_| fail())?,
    );
    let a = authority(store);
    let cfg = config();
    let host = Host;
    let request = AuthRequest {
        method: "POST".into(),
        url: "https://ai.ubq.fi/v1/responses".into(),
        headers: BTreeMap::from([("authorization".into(), format!("Bearer {token}"))]),
        ..Default::default()
    };
    let context = RequestContext {
        request_id: RequestId("synthetic-http-original-request".into()),
        principal: PrincipalId("passkey-user:user-fixture".into()),
        key_id: None,
        route: "responses".into(),
        started_at_ms: now_ms().map_err(|_| fail())?,
    };
    let actor = futures_executor::block_on(actor::authenticate_admin_actor(
        &a, &host, &cfg, &request, &context,
    ))
    .map_err(|_| fail())?;
    let owner = RegisteredRenewalOwner::configured("native-owner".into(), HOME.into())
        .map_err(|_| fail())?;
    let cancel = Cancel::default();
    let control = NativeRenewalAuthority::configured(&a, id, &owner, &cfg, &cancel);
    let prepared = futures_executor::block_on(control.prepare_import(
        &actor,
        NativeImportTarget::ExistingAccount {
            account_id_hash: account,
        },
    ))
    .map_err(|_| fail())?
    .metadata()
    .clone();
    *store.action_key.lock().map_err(|_| fail())? =
        Some(gateway_auth_primitives::store::key(json!([
            "uos_ai",
            "authority",
            "native_owner_renewal",
            "v1",
            "action",
            prepared.action_ref
        ])));
    Ok(prepared)
}
struct Fixture {
    root: PathBuf,
    identity: NativeTransportIdentity,
    metadata: NativeImportMetadata,
    store: Store,
    observed: Arc<Mutex<Observed>>,
    service: Arc<NativeHttpService>,
}
impl Fixture {
    fn new(configured: bool) -> Result<Self, GatewayError> {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let root = std::env::temp_dir().join(format!(
            "uos-import-http-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::SeqCst)
        ));
        fs::create_dir(&root).map_err(|_| fail())?;
        let identity = NativeTransportIdentity::new(
            "import-http-test".into(),
            "owner".into(),
            "epoch".into(),
            [53; 32],
            BTreeSet::from([
                NativeCapability::CodexAuthImport,
                NativeCapability::NativeProfileImport,
                NativeCapability::SupervisorRead,
                NativeCapability::OwnerRenewal,
            ]),
        )
        .map_err(|_| fail())?;
        let store = Store::default();
        let hash = seed(&store, &identity)?;
        let metadata = prepare(&store, &identity, hash)?;
        let observed = Arc::new(Mutex::new(Observed::default()));
        let mut service = NativeHttpService::configured(
            identity.clone(),
            SqliteNonceStore::open(&root.join("nonces.sqlite3")).map_err(|_| fail())?,
            SupervisorData::configured(Vec::new()).map_err(|_| fail())?,
        );
        if configured {
            service = service.with_imports(
                authority(&store),
                RegisteredRenewalOwner::configured("native-owner".into(), HOME.into())
                    .map_err(|_| fail())?,
                config(),
                Recording(observed.clone()),
            );
        }
        Ok(Self {
            root,
            identity,
            metadata,
            store,
            observed,
            service: Arc::new(service),
        })
    }
    fn head(
        &self,
        kind: NativeImportKind,
        nonce_byte: u8,
        body: &[u8],
        framing: &str,
    ) -> Result<Vec<u8>, GatewayError> {
        let time = now_ms().map_err(|_| fail())?;
        let nonce = base64url(&[nonce_byte; 32]);
        let digest = sha256_hex(body);
        let mac = self
            .identity
            .sign_import(kind, &self.metadata, &digest, time, &nonce)
            .map_err(|_| fail())?;
        Ok(format!("POST {} HTTP/1.1\r\nHost: localhost\r\n{NATIVE_VERSION_HEADER}: 1\r\n{NATIVE_KEY_HEADER}: {}\r\n{NATIVE_TIME_HEADER}: {time}\r\n{NATIVE_NONCE_HEADER}: {nonce}\r\n{NATIVE_MAC_HEADER}: {mac}\r\n{NATIVE_IMPORT_OPERATION_HEADER}: {}\r\n{NATIVE_IMPORT_ACTION_HEADER}: {}\r\n{NATIVE_IMPORT_BODY_HEADER}: {digest}\r\n{framing}\r\n", kind.path(), self.identity.key_id(), self.metadata.operation_id, self.metadata.action_ref).into_bytes())
    }
    fn start(
        &self,
    ) -> Result<(TcpStream, thread::JoinHandle<Result<(), GatewayError>>), GatewayError> {
        let listener = TcpListener::bind("127.0.0.1:0").map_err(|_| fail())?;
        let address = listener.local_addr().map_err(|_| fail())?;
        let service = self.service.clone();
        let server = thread::spawn(move || -> Result<(), GatewayError> {
            let (mut stream, _) = listener.accept().map_err(|_| fail())?;
            if let Err(error) = service.handle(&mut stream) {
                let _ = write_error(&mut stream, error);
            }
            Ok(())
        });
        let client = TcpStream::connect(address).map_err(|_| fail())?;
        client
            .set_read_timeout(Some(Duration::from_secs(3)))
            .map_err(|_| fail())?;
        Ok((client, server))
    }
    fn exchange(&self, request: &[u8]) -> Result<Vec<u8>, GatewayError> {
        let (mut client, server) = self.start()?;
        client.write_all(request).map_err(|_| fail())?;
        let mut response = Vec::new();
        read_response(&mut client, &mut response)?;
        server.join().map_err(|_| fail())??;
        Ok(response)
    }
    fn document(&self, salt: u8, body: &[u8]) -> Result<Vec<u8>, GatewayError> {
        let mut bytes = self.head(
            NativeImportKind::CodexAuth,
            salt,
            body,
            &format!("Content-Length: {}\r\n", body.len()),
        )?;
        bytes.extend_from_slice(body);
        Ok(bytes)
    }
    fn calls(&self) -> Result<usize, GatewayError> {
        Ok(self.observed.lock().map_err(|_| fail())?.documents.len())
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
}
fn read_response(client: &mut TcpStream, response: &mut Vec<u8>) -> Result<(), GatewayError> {
    while !response.ends_with(b"\r\n\r\n") {
        let mut byte = [0u8; 1];
        client.read_exact(&mut byte).map_err(|_| fail())?;
        response.push(byte[0]);
        assert!(response.len() <= 16_384);
    }
    let length = std::str::from_utf8(response)
        .map_err(|_| fail())?
        .lines()
        .find_map(|line| {
            line.split_once(':')
                .filter(|(name, _)| name.eq_ignore_ascii_case("content-length"))
                .map(|(_, value)| value.trim())
        })
        .ok_or_else(fail)?
        .parse::<usize>()
        .map_err(|_| fail())?;
    let start = response.len();
    response.resize(start + length, 0);
    client
        .read_exact(&mut response[start..])
        .map_err(|_| fail())?;
    Ok(())
}
fn status(response: &[u8]) -> Result<u16, GatewayError> {
    std::str::from_utf8(response)
        .map_err(|_| fail())?
        .split_whitespace()
        .nth(1)
        .ok_or_else(fail)?
        .parse()
        .map_err(|_| fail())
}
fn error(response: &[u8]) -> Result<Value, GatewayError> {
    let at = response
        .windows(4)
        .position(|part| part == b"\r\n\r\n")
        .ok_or_else(fail)?
        + 4;
    serde_json::from_slice(&response[at..]).map_err(|_| fail())
}
#[test]
fn current_authenticated_raw_bytes_dispatch_once_without_claiming_import_success()
-> Result<(), GatewayError> {
    let fixture = Fixture::new(true)?;
    let request = fixture.document(1, RAW)?;
    let response = fixture.exchange(&request)?;
    assert_eq!(status(&response)?, 503);
    assert_eq!(
        error(&response)?["error"]["code"],
        "fixture_import_effect_unavailable"
    );
    assert_eq!(
        fixture.observed.lock().map_err(|_| fail())?.documents,
        [RAW.to_vec()]
    );
    assert_ne!(status(&fixture.exchange(&request)?)?, 200);
    assert_eq!(fixture.calls()?, 1);
    Ok(())
}
#[test]
fn header_auth_and_missing_action_refuse_before_waiting_for_document() -> Result<(), GatewayError> {
    let mut fixture = Fixture::new(true)?;
    let mut wrong = fixture.head(
        NativeImportKind::CodexAuth,
        2,
        RAW,
        "Content-Length: 1024\r\n",
    )?;
    let at = wrong
        .windows(NATIVE_MAC_HEADER.len())
        .position(|part| part == NATIVE_MAC_HEADER.as_bytes())
        .ok_or_else(fail)?
        + NATIVE_MAC_HEADER.len()
        + 2;
    wrong[at] = if wrong[at] == b'A' { b'B' } else { b'A' };
    assert_eq!(status(&fixture.exchange(&wrong)?)?, 401);
    fixture.metadata.action_ref = "a".repeat(64);
    let absent = fixture.head(
        NativeImportKind::CodexAuth,
        3,
        RAW,
        "Content-Length: 1024\r\n",
    )?;
    assert_eq!(status(&fixture.exchange(&absent)?)?, 503);
    assert_eq!(fixture.calls()?, 0);
    Ok(())
}

#[test]
fn stale_owner_and_revoked_actor_stop_before_raw_read() -> Result<(), GatewayError> {
    let fixture = Fixture::new(true)?;
    fixture
        .store
        .rows
        .lock()
        .map_err(|_| fail())?
        .remove(&keys::paid_control_owner().0);
    assert_eq!(
        status(&fixture.exchange(&fixture.head(
            NativeImportKind::CodexAuth,
            4,
            RAW,
            "Content-Length: 1024\r\n"
        )?)?)?,
        503
    );
    assert_eq!(fixture.calls()?, 0);
    let fixture = Fixture::new(true)?;
    fixture
        .store
        .rows
        .lock()
        .map_err(|_| fail())?
        .remove(&passkeys::key("users", "user-fixture").0);
    assert_eq!(
        status(&fixture.exchange(&fixture.head(
            NativeImportKind::CodexAuth,
            5,
            RAW,
            "Content-Length: 1024\r\n"
        )?)?)?,
        503
    );
    assert_eq!(fixture.calls()?, 0);
    Ok(())
}
#[test]
fn source_change_after_header_admission_cannot_reach_handler() -> Result<(), GatewayError> {
    let fixture = Fixture::new(true)?;
    let (mut client, server) = fixture.start()?;
    client
        .write_all(&fixture.head(
            NativeImportKind::CodexAuth,
            6,
            RAW,
            &format!("Content-Length: {}\r\n", RAW.len()),
        )?)
        .map_err(|_| fail())?;
    fixture.store.wait_header_admitted()?;
    fixture
        .store
        .rows
        .lock()
        .map_err(|_| fail())?
        .remove(&codex_source_pool_key().0);
    client.write_all(RAW).map_err(|_| fail())?;
    let mut response = Vec::new();
    client.read_to_end(&mut response).map_err(|_| fail())?;
    server.join().map_err(|_| fail())??;
    assert_eq!(status(&response)?, 503);
    assert_eq!(fixture.calls()?, 0);
    Ok(())
}
#[test]
fn wrong_body_digest_and_actual_source_shape_fail_before_recording() -> Result<(), GatewayError> {
    let fixture = Fixture::new(true)?;
    let mut request = fixture.head(
        NativeImportKind::CodexAuth,
        7,
        b"different",
        &format!("Content-Length: {}\r\n", RAW.len()),
    )?;
    request.extend_from_slice(RAW);
    assert_eq!(status(&fixture.exchange(&request)?)?, 403);
    let response = fixture.exchange(&fixture.document(8, b"{")?)?;
    assert_eq!(status(&response)?, 400);
    assert_eq!(
        error(&response)?["error"]["message"],
        "Body does not look like a Codex auth.json"
    );
    assert_eq!(fixture.calls()?, 0);
    Ok(())
}
#[test]
fn declared_and_chunked_oversize_auth_use_source400_without_body_or_handler()
-> Result<(), GatewayError> {
    let fixture = Fixture::new(true)?;
    let request = fixture.head(
        NativeImportKind::CodexAuth,
        9,
        RAW,
        "Content-Length: 33554433\r\n",
    )?;
    let response = fixture.exchange(&request)?;
    assert_eq!(status(&response)?, 400);
    assert_eq!(
        error(&response)?["error"]["message"],
        "Body does not look like a Codex auth.json"
    );
    let mut request = fixture.head(
        NativeImportKind::CodexAuth,
        10,
        RAW,
        "Transfer-Encoding: chunked\r\n",
    )?;
    request.extend_from_slice(b"2000001\r\n");
    assert_eq!(status(&fixture.exchange(&request)?)?, 400);
    assert_eq!(fixture.calls()?, 0);
    Ok(())
}
#[test]
fn canonical_length_and_ambiguous_framing_refuse_without_raw_read() -> Result<(), GatewayError> {
    let fixture = Fixture::new(true)?;
    for (salt, framing) in [
        (11, "Content-Length: 01\r\n"),
        (12, "Content-Length: 1\r\nTransfer-Encoding: chunked\r\n"),
        (13, "Content-Length: 1\r\nContent-Length: 1\r\n"),
    ] {
        assert_eq!(
            status(&fixture.exchange(&fixture.head(
                NativeImportKind::CodexAuth,
                salt,
                RAW,
                framing
            )?)?)?,
            400
        );
    }
    assert_eq!(fixture.calls()?, 0);
    Ok(())
}
#[test]
fn chunked_original_document_reaches_same_guarded_handler() -> Result<(), GatewayError> {
    let fixture = Fixture::new(true)?;
    let mut request = fixture.head(
        NativeImportKind::CodexAuth,
        14,
        RAW,
        "Transfer-Encoding: chunked\r\n",
    )?;
    request.extend_from_slice(format!("{:x}\r\n", RAW.len()).as_bytes());
    request.extend_from_slice(RAW);
    request.extend_from_slice(b"\r\n0\r\n\r\n");
    assert_eq!(status(&fixture.exchange(&request)?)?, 503);
    assert_eq!(
        fixture.observed.lock().map_err(|_| fail())?.documents,
        [RAW.to_vec()]
    );
    Ok(())
}
#[test]
fn profile_without_real_prepared_authority_and_unconfigured_service_fail_closed()
-> Result<(), GatewayError> {
    let fixture = Fixture::new(true)?;
    let request = fixture.head(
        NativeImportKind::NativeProfile,
        15,
        b"",
        "Content-Length: 0\r\n",
    )?;
    assert_eq!(status(&fixture.exchange(&request)?)?, 503);
    assert_eq!(fixture.calls()?, 0);
    let fixture = Fixture::new(false)?;
    assert_eq!(
        status(&fixture.exchange(&fixture.document(16, RAW)?)?)?,
        503
    );
    assert_eq!(fixture.calls()?, 0);
    Ok(())
}
#[test]
fn original_disconnect_after_header_does_not_invoke_handler() -> Result<(), GatewayError> {
    let fixture = Fixture::new(true)?;
    let (mut client, server) = fixture.start()?;
    client
        .write_all(&fixture.head(
            NativeImportKind::CodexAuth,
            17,
            RAW,
            &format!("Content-Length: {}\r\n", RAW.len()),
        )?)
        .map_err(|_| fail())?;
    fixture.store.wait_header_admitted()?;
    client.shutdown(Shutdown::Both).map_err(|_| fail())?;
    drop(client);
    server.join().map_err(|_| fail())??;
    assert_eq!(fixture.calls()?, 0);
    Ok(())
}
#[test]
fn legacy_rpc_and_control_limits_and_responses_remain_separate() -> Result<(), GatewayError> {
    let fixture = Fixture::new(true)?;
    for (path, size) in [
        (NATIVE_RPC_PATH, NATIVE_MAX_BODY_BYTES + 1),
        (NATIVE_CONTROL_PATH, NATIVE_CONTROL_MAX_BYTES + 1),
    ] {
        let response = fixture.exchange(
            format!("POST {path} HTTP/1.1\r\nHost: localhost\r\nContent-Length: {size}\r\n\r\n")
                .as_bytes(),
        )?;
        assert_eq!(status(&response)?, 413);
        assert_eq!(error(&response)?["error"]["code"], "native_body_too_large");
    }
    let body = serde_json::to_vec(&NativeRpcEnvelope {
        v: 1,
        request: NativeReadRequest::SupervisorSessions,
    })
    .map_err(|_| fail())?;
    let time = now_ms().map_err(|_| fail())?;
    let nonce = base64url(&[18; 32]);
    let mac = fixture
        .identity
        .sign(NATIVE_RPC_PATH, time, &nonce, &body)
        .map_err(|_| fail())?;
    let mut request = format!("POST {NATIVE_RPC_PATH} HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\n{NATIVE_VERSION_HEADER}: 1\r\n{NATIVE_KEY_HEADER}: {}\r\n{NATIVE_TIME_HEADER}: {time}\r\n{NATIVE_NONCE_HEADER}: {nonce}\r\n{NATIVE_MAC_HEADER}: {mac}\r\n\r\n", body.len(), fixture.identity.key_id()).into_bytes();
    request.extend_from_slice(&body);
    assert_eq!(status(&fixture.exchange(&request)?)?, 200);
    assert_eq!(fixture.calls()?, 0);
    Ok(())
}

#[test]
fn import_headers_keep_sixteen_kib_and_sixty_four_field_limits() -> Result<(), GatewayError> {
    let fixture = Fixture::new(true)?;
    for extra in [
        format!("X-Oversized: {}\r\n", "x".repeat(16_384)),
        (0..65)
            .map(|i| format!("X-Field-{i}: value\r\n"))
            .collect::<String>(),
    ] {
        let mut request = fixture.head(
            NativeImportKind::CodexAuth,
            19,
            RAW,
            "Content-Length: 100\r\n",
        )?;
        request.truncate(request.len() - 2);
        request.extend_from_slice(extra.as_bytes());
        request.extend_from_slice(b"\r\n");
        assert_eq!(status(&fixture.exchange(&request)?)?, 400);
    }
    assert_eq!(fixture.calls()?, 0);
    Ok(())
}
#[test]
fn buffered_unsigned_trailers_or_second_requests_never_reach_handler() -> Result<(), GatewayError> {
    let fixture = Fixture::new(true)?;
    let mut request = fixture.document(20, RAW)?;
    request.extend_from_slice(b"POST /unexpected HTTP/1.1\r\n\r\n");
    assert_eq!(status(&fixture.exchange(&request)?)?, 400);
    let mut request = fixture.head(
        NativeImportKind::CodexAuth,
        21,
        RAW,
        "Transfer-Encoding: chunked\r\n",
    )?;
    request.extend_from_slice(format!("{:x}\r\n", RAW.len()).as_bytes());
    request.extend_from_slice(RAW);
    request.extend_from_slice(b"\r\n0\r\nX-Trailer: unsupported\r\n\r\n");
    assert_eq!(status(&fixture.exchange(&request)?)?, 400);
    assert_eq!(fixture.calls()?, 0);
    Ok(())
}

#[test]
fn native_only_source_result_preserves_status_headers_and_utf16_without_success_claim()
-> Result<(), GatewayError> {
    let fixture = Fixture::new(true)?;
    fixture.observed.lock().map_err(|_| fail())?.source_result = true;
    let response = fixture.exchange(&fixture.document(22, RAW)?)?;
    assert_eq!(status(&response)?, 503);
    assert!(
        std::str::from_utf8(&response)
            .map_err(|_| fail())?
            .contains("x-uos-upstream: fixture-native-only\r\n")
    );
    let start = response
        .windows(4)
        .position(|part| part == b"\r\n\r\n")
        .ok_or_else(fail)?
        + 4;
    let body = SourceJson::parse(std::str::from_utf8(&response[start..]).map_err(|_| fail())?)
        .map_err(|_| fail())?;
    let error = source_serialization::member(&body, "error").ok_or_else(fail)?;
    let message = source_serialization::member(&error, "message").ok_or_else(fail)?;
    let expected = JsString16::from_units("native-only".encode_utf16().chain([0xd800]).collect());
    assert_eq!(source_serialization::string(&message), Some(expected));
    assert_eq!(fixture.calls()?, 1);
    assert!(
        !std::str::from_utf8(&response)
            .map_err(|_| fail())?
            .contains("SYNTHETIC_REFRESH")
    );
    Ok(())
}

#[test]
fn actual_wrapped_auth_data_and_source_client_version_are_bound_to_handler_context()
-> Result<(), GatewayError> {
    let fixture = Fixture::new(true)?;
    let body = br#"{"auth":{"tokens":{"access_token":"SYNTHETIC_ACCESS","refresh_token":"SYNTHETIC_REFRESH","account_id":"synthetic-account"}},"models":{"client_version":"0.126.0","clientVersion":"0.127.0"}}"#;
    let response = fixture.exchange(&fixture.document(23, body)?)?;
    assert_eq!(status(&response)?, 503);
    let observed = fixture.observed.lock().map_err(|_| fail())?;
    assert_eq!(observed.documents, [body.to_vec()]);
    assert_eq!(
        observed.client_versions,
        [Some(JsString16::from_utf8("0.126.0"))]
    );
    assert_eq!(
        observed.account_ids,
        [JsString16::from_utf8("synthetic-account")]
    );
    assert!(
        !std::str::from_utf8(&response)
            .map_err(|_| fail())?
            .contains("SYNTHETIC_REFRESH")
    );
    Ok(())
}
