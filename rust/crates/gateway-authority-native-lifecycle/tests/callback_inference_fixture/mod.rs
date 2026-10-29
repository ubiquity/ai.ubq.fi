//! Target-owned offline Passport, native signing and inference DATA fixtures; no provider I/O.
use gateway_auth::resolve::AuthService;
use gateway_auth_contracts::{
    types::{AuthConfig, AuthHost, AuthRequest},
    upstream::{AuthUpstreamRequest, AuthUpstreamResponse},
    webauthn::{CosePublicKey, SignatureInput, WebAuthnCrypto},
};
use gateway_auth_credentials::{
    native_credentials::parse_native_document,
    provider_credentials::{CodexNativeOwner, codex_source_pool_key, parse_codex_source_pool},
};
use gateway_auth_native_contracts::transport::{NativeAuthHeaders, NativeCapability};
use gateway_auth_native_proof::native_auth::{
    AuthenticatedNativeOwner, NativeAuthenticator, NativeTransportIdentity,
};
use gateway_auth_native_protocol::native_replay::StoreNonceAuthority;
use gateway_auth_native_wire::native_read::NATIVE_RENEWAL_AUTHORITY_PATH;
use gateway_auth_passport_contracts::{
    passport::{PASSPORT_COOKIE, PassportConfig},
    session::PassportDate,
};
use gateway_auth_passport_sessions::sessions::PassportSessions;
use gateway_auth_primitives::crypto::base64url;
use gateway_authority_contracts::{AuthorityClock, NativeRenewalCancellation, TokenSource};
use gateway_authority_keys as keys;
use gateway_authority_native_lifecycle::SourceNativeLifecycleService;
use gateway_authority_paid_control_records::{
    PAID_CONTROL_CONFIG_VERSION, PaidControlConfig, PaidControlOwner, PaidSourceCheck,
};
use gateway_authority_routing_records::types::SubscriptionPool;
use gateway_core::state::AuthorityStore;
use gateway_native_authority_request::NativeRenewalAuthorityRequest as Request;
use gateway_native_authority_wire::source_response::SourceNativeRenewalAuthorityResponseV2 as Response;
use gateway_provider_policy::policy::HopFailure;
use gateway_routing_runtime::types::{AttemptFailure, FailureOrigin, TransportState};
use gateway_routing_runtime_test_support::{
    input::{NOW as RUNTIME_NOW, input},
    seed::seed_pool,
    store::Store as RuntimeStore,
};
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use std::{
    cell::Cell,
    collections::BTreeMap,
    future::Future,
    sync::atomic::{AtomicI64, Ordering},
    task::{Context, Poll, Waker},
};

pub(super) const NOW: i64 = 1_793_059_200_000;
pub(super) type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;
pub(super) struct Clock(pub(super) AtomicI64);
pub(super) struct Host {
    pub(super) status: Cell<u16>,
    pub(super) hold: Cell<bool>,
    pub(super) admin: Cell<bool>,
    pub(super) calls: Cell<usize>,
    pub(super) now: Clock,
}
impl Default for Host {
    fn default() -> Self {
        Self {
            status: Cell::new(200),
            hold: Cell::new(false),
            admin: Cell::new(true),
            calls: Cell::new(0),
            now: Clock(AtomicI64::new(NOW)),
        }
    }
}
pub(super) fn unavailable() -> GatewayError {
    GatewayError::unavailable("Fixture unavailable", "fixture_unavailable")
}
impl PassportDate for Host {
    fn parse_date_ms(&self, value: &JsString16) -> f64 {
        if value == &JsString16::from_utf8("10/27/2026") {
            1793059200000.0
        } else {
            f64::NAN
        }
    }
}
impl WebAuthnCrypto for Host {
    async fn validate_key(&self, _: &CosePublicKey) -> Result<(), GatewayError> {
        Err(unavailable())
    }
    async fn verify_signature(&self, _: SignatureInput) -> Result<bool, GatewayError> {
        Err(unavailable())
    }
}
impl AuthHost for Host {
    fn now_ms(&self) -> i64 {
        self.now.0.load(Ordering::SeqCst)
    }
    fn random_bytes(&self, n: usize) -> Result<Vec<u8>, GatewayError> {
        Ok(vec![7; n])
    }
    async fn kernel_principal(
        &self,
        _: &str,
        _: &str,
        _: &str,
        _: &str,
    ) -> Result<String, GatewayError> {
        Err(unavailable())
    }
    async fn fetch_auth(
        &self,
        request: AuthUpstreamRequest,
    ) -> Result<AuthUpstreamResponse, GatewayError> {
        let AuthUpstreamRequest::PassportSession { app_session_token } = request else {
            return Err(unavailable());
        };
        assert_eq!(
            app_session_token.expose(),
            format!("uos_app_session_{}", "A".repeat(43))
        );
        self.calls.set(self.calls.get() + 1);
        std::future::poll_fn(|_| {
            if self.hold.get() {
                Poll::Pending
            } else {
                Poll::Ready(())
            }
        })
        .await;
        let body = format!(
            r#"{{"authenticated":true,"session":{{"subject":"\ud800\u0000","clientId":"ai","audience":"ai","origin":"https://ai.ubq.fi","redirectUri":"https://ai.ubq.fi/auth/callback","createdAt":"10/27/2026"}},"user":{{"id":"\ud800\u0000","handle":"","isAdmin":{},"scopes":[]}}}}"#,
            self.admin.get()
        );
        Ok(AuthUpstreamResponse {
            status: self.status.get(),
            body: body.into_bytes(),
        })
    }
}
pub(super) fn run<F: Future>(future: F) -> F::Output {
    let mut future = std::pin::pin!(future);
    let mut cx = Context::from_waker(Waker::noop());
    loop {
        if let Poll::Ready(v) = future.as_mut().poll(&mut cx) {
            return v;
        }
        std::thread::yield_now();
    }
}
pub(super) fn identity() -> Result<NativeTransportIdentity, GatewayError> {
    NativeTransportIdentity::new(
        "key".into(),
        "owner".into(),
        "epoch".into(),
        [73; 32],
        std::collections::BTreeSet::from([NativeCapability::OwnerRenewal]),
    )
}
impl AuthorityClock for Clock {
    fn now_ms(&self) -> Result<i64, GatewayError> {
        Ok(self.0.load(Ordering::SeqCst))
    }
}
pub(super) struct Cancel(pub(super) bool);
impl NativeRenewalCancellation for Cancel {
    fn cancelled(&self) -> bool {
        self.0
    }
}
pub(super) fn control_body(action: &str, operation: &str) -> Result<Vec<u8>, GatewayError> {
    serde_json::to_vec(&gateway_auth_native_wire::control::NativeControlEnvelope {
        v: 1,
        operation_id: operation.into(),
        request: gateway_auth_native_wire::control::NativeControlRequest::CodexRenewAccess {
            action_ref: action.into(),
        },
    })
    .map_err(|_| unavailable())
}
pub(super) async fn peer<S: AuthorityStore>(
    identity: &NativeTransportIdentity,
    replay: &StoreNonceAuthority<'_, S>,
    path: &str,
    bytes: &[u8],
    now: i64,
    n: u8,
) -> Result<AuthenticatedNativeOwner, GatewayError> {
    let nonce = base64url(&[n; 32]);
    let mac = identity.sign(path, now, &nonce, bytes)?;
    NativeAuthenticator::new(identity, replay)
        .authenticate(
            path,
            NativeAuthHeaders {
                version: "1",
                key_id: identity.key_id(),
                time_ms: &now.to_string(),
                nonce: &nonce,
                mac: &mac,
            },
            bytes,
            now,
        )
        .await
}
pub(super) async fn execute<S: AuthorityStore, I: TokenSource, C: NativeRenewalCancellation>(
    service: &SourceNativeLifecycleService<'_, S, I, Host, C>,
    store: &S,
    host: &Host,
    identity: &NativeTransportIdentity,
    request: &Request,
    n: u8,
) -> Result<Response, GatewayError> {
    let bytes = serde_json::to_vec(request).map_err(|_| unavailable())?;
    let replay = StoreNonceAuthority::new(store);
    let proof = peer(
        identity,
        &replay,
        NATIVE_RENEWAL_AUTHORITY_PATH,
        &bytes,
        AuthorityClock::now_ms(&host.now)?,
        n,
    )
    .await?;
    gateway_authority_native_callback::execute_source(
        service, &proof, identity, &replay, &host.now, &bytes,
    )
    .await
}
pub(super) async fn inference_input(
    store: &RuntimeStore,
    host: &Host,
    id: &str,
) -> TestResult<(
    gateway_routing_runtime::types::RoutingInput,
    AuthRequest,
    gateway_auth_native_proof::native_auth::NativeTransportIdentity,
    AuthConfig,
)> {
    seed_pool(store);
    host.now.0.store(RUNTIME_NOW, Ordering::SeqCst);
    let native = identity()?;
    let raw = store
        .read(&codex_source_pool_key())
        .await?
        .ok_or("source")?;
    let mut pool = parse_codex_source_pool(raw.json_value()?)?.ok_or("pool")?;
    let account = &mut pool.accounts[0];
    let payload = serde_json::json!({"https://api.openai.com/auth":{
        "chatgpt_user_id":"native-user","chatgpt_account_id":account.account_id
    }});
    let access = format!("e30.{}.fixture", base64url(&serde_json::to_vec(&payload)?));
    let credentials = parse_native_document(&serde_json::json!({
        "auth_mode":"chatgptAuthTokens", "OPENAI_API_KEY":null,
        "tokens":{"account_id":account.account_id,"access_token":access,
            "id_token":access,"refresh_token":""}
    }))?;
    let generation_hash = credentials.generation_hash();
    account.access_token = credentials.access_token;
    account.refresh_token = credentials.refresh_token;
    account.native_owner = Some(CodexNativeOwner {
        codex_home: "/tmp/native-source".into(),
        generation_hash,
        native_refreshed_at: credentials.native_refreshed_at,
        external_user_id: credentials.external_user_id,
        managed_source: None,
    });
    let account_identity = account.identity();
    let source = pool.private_source_value();
    parse_codex_source_pool(&source)?.ok_or("external pool")?;
    store.seed(codex_source_pool_key(), source);
    let source_revision = store
        .read(&codex_source_pool_key())
        .await?
        .ok_or("new source")?
        .revision;
    let raw = store.read(&keys::pool()).await?.ok_or("normalized")?;
    let mut normalized: SubscriptionPool = serde_json::from_value(raw.json_value()?.clone())?;
    normalized.accounts[0].account_id_hash = account_identity.routing_account_hash;
    normalized.accounts[0].admin_account_hash = account_identity.administrative_account_hash;
    normalized.accounts[0].credential_version = account_identity.credential_version;
    normalized.pool_versionstamp.0 = source_revision;
    store.seed(keys::pool(), normalized);
    let runtime = gateway_schema::catalog_store::runtime_config_key();
    store.seed(runtime.clone(), serde_json::json!({}));
    let runtime_revision = store.read(&runtime).await?.ok_or("runtime")?.revision;
    store.seed(
        keys::paid_control_owner(),
        PaidControlOwner {
            v: 1,
            owner_id: native.owner_id().into(),
            owner_epoch: native.owner_epoch().into(),
            key_fingerprint: native.key_fingerprint().into(),
        },
    );
    store.seed(
        keys::paid_control_config(),
        PaidControlConfig {
            v: PAID_CONTROL_CONFIG_VERSION,
            owner_id: native.owner_id().into(),
            owner_epoch: native.owner_epoch().into(),
            generation: "configuration".into(),
            configuration_fingerprint: "fingerprint".into(),
            surplus_binding: None,
            openlux_discovery_binding: None,
            openlux_binding: None,
            source_checks: vec![PaidSourceCheck {
                key: runtime.0,
                expected_revision: Some(runtime_revision),
            }],
        },
    );
    let grant = SourceJson::parse(&format!(
        r#"{{"subject":"\ud800\u0000","clientId":"ai","audience":"ai","origin":"https://ai.ubq.fi","redirectUri":"https://ai.ubq.fi/auth/callback","createdAt":"10/27/2026","token":"uos_app_session_{}"}}"#,
        "A".repeat(43)
    ))?;
    let cookie = PassportSessions { store, host }
        .create(&grant, &JsString16::from_units(vec![0xd800, 0]), None)
        .await?;
    let request = AuthRequest {
        method: "POST".into(),
        url: "https://ai.ubq.fi/v1/responses".into(),
        headers: BTreeMap::from([("cookie".into(), format!("{PASSPORT_COOKIE}={cookie}"))]),
        ..Default::default()
    };
    let config = AuthConfig {
        passport: Some(PassportConfig::production()),
        ..Default::default()
    };
    let principal = AuthService::new(store, host, &config)
        .authenticate_client(&request)
        .await?;
    let mut input = input(id, "gpt-test", &[]);
    input.context.principal = principal.method.principal_id(
        principal
            .token
            .as_ref()
            .map(gateway_auth_primitives::crypto::Secret::expose),
    );
    input.context.key_id = None;
    input.principal = principal;
    Ok((input, request, native, config))
}
pub(super) fn refusal() -> AttemptFailure {
    AttemptFailure {
        origin: FailureOrigin::Provider,
        status: Some(401),
        headers: BTreeMap::new(),
        body: vec![],
        body_complete: true,
        failure: HopFailure::Status(401),
        transport: TransportState::Started,
        semantic_committed: false,
        cancelled: false,
    }
}
