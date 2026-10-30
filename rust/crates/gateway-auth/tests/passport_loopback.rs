//! Existing AuthService ceremony with synthetic central HTTP and a real RS256 fixture.
use gateway_auth::resolve::AuthService;
use gateway_auth_contracts::{
    types::{AuthConfig, AuthHost, AuthMethod, AuthRequest, AuthResponse},
    upstream::{AuthUpstreamRequest, AuthUpstreamResponse},
    webauthn::{CosePublicKey, SignatureInput, WebAuthnCrypto},
};
use gateway_auth_passport::passport::reset_jwks_cache;
use gateway_auth_passport_contracts::{
    actor_data::{PassportActorDataV2, PassportSessionData},
    passport::{PASSPORT_COOKIE, PASSPORT_STATE_COOKIE, PassportConfig},
    session::{PASSPORT_COOKIE_MAX_AGE_SECONDS, PASSPORT_VERIFIER_COOKIE, PassportDate},
};
use gateway_auth_passport_sessions::sessions::session_key;
use gateway_auth_primitives::crypto::{base64url, sha256_base64url, sha256_hex};
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, StoredRecord,
};
use gateway_kv_codec::key::RecordKey;
use gateway_source::{error::GatewayError, js_string::JsString16};
use rsa::{
    RsaPrivateKey,
    pkcs1v15::SigningKey,
    pkcs8::DecodePrivateKey,
    signature::{SignatureEncoding, Signer},
    traits::PublicKeyParts,
};
use serde_json::{Value, json};
use std::{
    cell::{Cell, RefCell},
    collections::{BTreeMap, BTreeSet},
    future::Future,
    task::{Context, Poll, Waker},
};

type Result<T = ()> = std::result::Result<T, Box<dyn std::error::Error>>;
const LOCAL: &str = "http://127.0.0.1:7998";
const CREATED: &str = "10/27/2026";

fn run<F: Future>(future: F) -> F::Output {
    let mut context = Context::from_waker(Waker::noop());
    let mut future = std::pin::pin!(future);
    loop {
        if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
            return value;
        }
        std::thread::yield_now();
    }
}
fn failure() -> GatewayError {
    GatewayError::unavailable("Synthetic Passport fixture failure", "fixture_denied")
}
fn binding(origin: &str) -> PassportConfig {
    PassportConfig {
        client_id: "ai".into(),
        audience: "ai".into(),
        origin: origin.into(),
        redirect_uri: format!("{origin}/auth/callback"),
    }
}
fn config(origin: &str) -> AuthConfig {
    AuthConfig {
        production: true,
        hosted: true,
        git_sha_known: true,
        passport: Some(binding(origin)),
        ..Default::default()
    }
}
fn values(origin: &str) -> BTreeMap<String, String> {
    BTreeMap::from([
        ("UOS_AI_PASSPORT_CLIENT_ID".into(), "ai".into()),
        ("UOS_AI_PASSPORT_AUDIENCE".into(), "ai".into()),
        ("UOS_AI_PASSPORT_ORIGIN".into(), origin.into()),
        (
            "UOS_AI_PASSPORT_REDIRECT_URI".into(),
            format!("{origin}/auth/callback"),
        ),
    ])
}
fn names(origin: &str) -> Result<[String; 3]> {
    if origin == "https://ai.ubq.fi" {
        return Ok([
            PASSPORT_COOKIE.into(),
            PASSPORT_STATE_COOKIE.into(),
            PASSPORT_VERIFIER_COOKIE.into(),
        ]);
    }
    let tuple = serde_json::to_vec(&[
        "uos-ai-passport-cookie-v1",
        "ai",
        "ai",
        origin,
        &format!("{origin}/auth/callback"),
    ])?;
    let digest = sha256_hex(&tuple);
    Ok(["session", "state", "verifier"]
        .map(|purpose| format!("uos_ai_passport_{digest}_{purpose}")))
}
#[derive(Default)]
struct Store(RefCell<BTreeMap<String, StoredRecord>>);
impl AuthorityStore for Store {
    async fn read(
        &self,
        key: &RecordKey,
    ) -> std::result::Result<Option<StoredRecord>, GatewayError> {
        Ok(self.0.borrow().get(&key.0).cloned())
    }
    async fn commit(
        &self,
        tx: AtomicTransition,
    ) -> std::result::Result<CommitOutcome, GatewayError> {
        let mut rows = self.0.borrow_mut();
        if tx.checks.iter().any(|check| {
            rows.get(&check.key.0).map(|row| &row.revision) != check.expected_revision.as_ref()
        }) {
            return Ok(CommitOutcome::Conflict);
        }
        for mutation in tx.mutations {
            match mutation {
                RecordMutation::Put { key, record } => {
                    rows.insert(key.0, record);
                }
                RecordMutation::Delete { key } => {
                    rows.remove(&key.0);
                }
            }
        }
        Ok(CommitOutcome::Committed)
    }
}
struct Host {
    binding: RefCell<PassportConfig>,
    random: Cell<u8>,
    now: Cell<i64>,
    admin: Cell<bool>,
    status: Cell<u16>,
    exchange_status: Cell<u16>,
    agent: Cell<bool>,
    malformed: Cell<bool>,
    requests: RefCell<Vec<AuthUpstreamRequest>>,
    codes: RefCell<BTreeSet<String>>,
}
impl Host {
    fn new(origin: &str) -> Self {
        reset_jwks_cache();
        Self {
            binding: RefCell::new(binding(origin)),
            random: Cell::new(0),
            now: Cell::new(1_500_000),
            admin: Cell::new(true),
            status: Cell::new(200),
            exchange_status: Cell::new(200),
            agent: Cell::new(false),
            malformed: Cell::new(false),
            requests: RefCell::default(),
            codes: RefCell::default(),
        }
    }
    fn session(&self, grant: bool) -> Value {
        let b = self.binding.borrow();
        let mut value = json!({"clientId":b.client_id,"audience":b.audience,"origin":b.origin,"redirectUri":b.redirect_uri,"subject":"user-123","createdAt":CREATED});
        if grant {
            value["token"] = json!(format!("uos_app_session_{}", "A".repeat(43)));
        }
        value
    }
    fn outbound(&self, kind: &str) -> Result<Vec<Value>> {
        self.requests
            .borrow()
            .iter()
            .filter(|request| match kind {
                "exchange" => matches!(request, AuthUpstreamRequest::PassportPkceExchange { .. }),
                "session" => matches!(request, AuthUpstreamRequest::PassportSession { .. }),
                "revoke" => matches!(request, AuthUpstreamRequest::PassportRevoke { .. }),
                _ => false,
            })
            .map(|request| Ok(serde_json::from_slice(&request.body()?)?))
            .collect()
    }
}
impl PassportDate for Host {
    fn parse_date_ms(&self, value: &JsString16) -> f64 {
        if value == &JsString16::from_utf8(CREATED) {
            1_793_059_200_000.0
        } else {
            f64::NAN
        }
    }
}
impl WebAuthnCrypto for Host {
    async fn validate_key(&self, _: &CosePublicKey) -> std::result::Result<(), GatewayError> {
        Err(failure())
    }
    async fn verify_signature(&self, _: SignatureInput) -> std::result::Result<bool, GatewayError> {
        Err(failure())
    }
}
impl AuthHost for Host {
    fn now_ms(&self) -> i64 {
        self.now.get()
    }
    fn random_bytes(&self, length: usize) -> std::result::Result<Vec<u8>, GatewayError> {
        let value = self.random.get().wrapping_add(1);
        self.random.set(value);
        Ok(vec![value; length])
    }
    async fn kernel_principal(
        &self,
        _: &str,
        _: &str,
        _: &str,
        _: &str,
    ) -> std::result::Result<String, GatewayError> {
        Err(failure())
    }
    async fn fetch_auth(
        &self,
        request: AuthUpstreamRequest,
    ) -> std::result::Result<AuthUpstreamResponse, GatewayError> {
        self.requests.borrow_mut().push(request.clone());
        let (status, body) = match request {
            AuthUpstreamRequest::PassportJwks => {
                (200, json!({"keys":[public_jwk().map_err(|_| failure())?]}))
            }
            AuthUpstreamRequest::PassportPkceExchange { code, .. } => {
                if !self.codes.borrow_mut().insert(code.expose().into()) {
                    return Ok(AuthUpstreamResponse {
                        status: 401,
                        body: Vec::new(),
                    });
                }
                if self.malformed.get() {
                    return Ok(AuthUpstreamResponse {
                        status: 200,
                        body: b"{}".to_vec(),
                    });
                }
                (
                    self.exchange_status.get(),
                    json!({"accessToken":signed_token(self.agent.get()).map_err(|_| failure())?,"appSession":self.session(true)}),
                )
            }
            AuthUpstreamRequest::PassportSession { .. } => (
                self.status.get(),
                json!({"authenticated":true,"session":self.session(false),"user":{"id":"user-123","handle":"alice","isAdmin":self.admin.get(),"scopes":["ai"]}}),
            ),
            AuthUpstreamRequest::PassportRevoke { .. } => {
                return Ok(AuthUpstreamResponse {
                    status: 204,
                    body: Vec::new(),
                });
            }
            _ => return Err(failure()),
        };
        Ok(AuthUpstreamResponse {
            status,
            body: serde_json::to_vec(&body).map_err(|_| failure())?,
        })
    }
}
fn public_jwk() -> Result<Value> {
    let key = RsaPrivateKey::from_pkcs8_pem(PRIVATE_PEM)?.to_public_key();
    Ok(
        json!({"kty":"RSA","kid":"loopback-fixture","alg":"RS256","use":"sig","n":base64url(&key.n().to_bytes_be()),"e":base64url(&key.e().to_bytes_be())}),
    )
}
fn signed_token(agent: bool) -> Result<String> {
    let header = base64url(br#"{"alg":"RS256","typ":"JWT","kid":"loopback-fixture"}"#);
    let mut claims = json!({"iss":"https://passport.ubq.fi","sub":"user-123","handle":"alice","isAdmin":true,"scopes":["ai"],"aud":"ai","clientId":"ai","auth":"sso","iat":1000,"exp":2000,"jti":"loopback-jti"});
    if agent {
        claims["agentTokenId"] = json!("agent-fixture");
    }
    let payload = base64url(&serde_json::to_vec(&claims)?);
    let message = format!("{header}.{payload}");
    let key = SigningKey::<sha2::Sha256>::new(RsaPrivateKey::from_pkcs8_pem(PRIVATE_PEM)?);
    Ok(format!(
        "{message}.{}",
        base64url(&key.sign(message.as_bytes()).to_vec())
    ))
}
fn request(origin: &str, path: &str, method: &str, cookies: &str) -> AuthRequest {
    AuthRequest {
        method: method.into(),
        url: format!("{origin}{path}"),
        headers: if cookies.is_empty() {
            BTreeMap::new()
        } else {
            BTreeMap::from([("cookie".into(), cookies.into())])
        },
        ..Default::default()
    }
}
fn route(store: &Store, host: &Host, cfg: &AuthConfig, req: &AuthRequest) -> Result<AuthResponse> {
    Ok(
        run(AuthService::new(store, host, cfg).handle_auth_route(req))?
            .ok_or("missing public Auth route")?,
    )
}
fn login(store: &Store, host: &Host, origin: &str) -> Result<AuthResponse> {
    let response = route(
        store,
        host,
        &config(origin),
        &request(origin, "/auth/login?return_to=%2Fadmin", "GET", ""),
    )?;
    assert_eq!(
        response.status, 302,
        "Explicit trusted loopback login must redirect without an API key"
    );
    Ok(response)
}
fn cookie_pair(response: &AuthResponse, name: &str) -> Result<String> {
    Ok(response
        .set_cookies
        .iter()
        .find(|cookie| cookie.starts_with(&format!("{name}=")))
        .and_then(|cookie| cookie.split(';').next())
        .ok_or("missing selected cookie")?
        .into())
}
fn transient(login: &AuthResponse, origin: &str) -> Result<String> {
    let n = names(origin)?;
    Ok(format!(
        "{}; {}",
        cookie_pair(login, &n[1])?,
        cookie_pair(login, &n[2])?
    ))
}
fn callback(
    store: &Store,
    host: &Host,
    origin: &str,
    login: &AuthResponse,
    code: &str,
) -> Result<AuthResponse> {
    let url = url::Url::parse(
        login
            .headers
            .get("location")
            .ok_or("missing authorize redirect")?,
    )?;
    let state = url
        .query_pairs()
        .find(|(name, _)| name == "state")
        .ok_or("missing state")?
        .1
        .into_owned();
    route(
        store,
        host,
        &config(origin),
        &request(
            origin,
            &format!("/auth/callback?state={state}&code={code}"),
            "GET",
            &transient(login, origin)?,
        ),
    )
}
fn session(store: &Store, host: &Host, origin: &str) -> Result<String> {
    let login = login(store, host, origin)?;
    let response = callback(
        store,
        host,
        origin,
        &login,
        &format!("code-{}", host.random.get()),
    )?;
    assert_eq!(response.status, 302);
    assert_eq!(
        response.headers.get("location").map(String::as_str),
        Some("/admin")
    );
    cookie_pair(&response, &names(origin)?[0])
}
fn assert_binding(body: &Value, origin: &str) {
    assert_eq!(body["clientId"], "ai");
    assert_eq!(body["audience"], "ai");
    assert_eq!(body["origin"], origin);
    assert_eq!(body["redirectUri"], format!("{origin}/auth/callback"));
}

#[test]
fn literal_loopback_binding_requires_complete_exact_configuration() -> Result {
    for origin in [LOCAL, "http://127.0.0.2:7998", "http://[::1]:7998"] {
        assert_eq!(
            PassportConfig::from_values(&values(origin)),
            Some(binding(origin))
        );
    }
    for key in values(LOCAL).keys() {
        let mut partial = values(LOCAL);
        partial.remove(key);
        assert!(PassportConfig::from_values(&partial).is_none());
    }
    for origin in [
        "http://example.com:7998",
        "http://localhost:7998",
        "http://app.local:7998",
        "http://127.0.0.1",
        "http://127.0.0.1:0",
        "http://u@127.0.0.1:7998",
        "http://127.0.0.1:7998/x",
        "http://127.0.0.1:7998?x",
        "http://127.0.0.1:7998#x",
    ] {
        assert!(
            PassportConfig::from_values(&values(origin)).is_none(),
            "{origin}"
        );
    }
    for callback in [
        "http://127.0.0.1:7999/auth/callback",
        "http://localhost:7998/auth/callback",
        "http://127.0.0.1:7998/other",
        "http://127.0.0.1:7998/auth/callback?x",
        "http://127.0.0.1:7998/auth/callback#x",
    ] {
        let mut configured = values(LOCAL);
        configured.insert("UOS_AI_PASSPORT_REDIRECT_URI".into(), callback.into());
        assert!(PassportConfig::from_values(&configured).is_none());
    }
    assert!(PassportConfig::from_values(&BTreeMap::new()).is_none());
    Ok(())
}

#[test]
fn public_loopback_login_has_exact_redirect_pkce_and_no_api_key_requirement() -> Result {
    let store = Store::default();
    let host = Host::new(LOCAL);
    let response = login(&store, &host, LOCAL)?;
    assert_eq!(
        response.headers.get("cache-control").map(String::as_str),
        Some("no-store")
    );
    let authorize = url::Url::parse(&response.headers["location"])?;
    assert_eq!(
        authorize.origin().ascii_serialization(),
        "https://passport.ubq.fi"
    );
    assert_eq!(authorize.path(), "/authorize");
    let query = authorize.query_pairs().collect::<BTreeMap<_, _>>();
    for (key, expected) in [
        ("client_id", "ai"),
        ("audience", "ai"),
        ("origin", LOCAL),
        ("redirect_uri", "http://127.0.0.1:7998/auth/callback"),
        ("prompt", "select_account"),
        ("code_challenge_method", "S256"),
    ] {
        assert_eq!(query.get(key).map(|value| value.as_ref()), Some(expected));
    }
    let n = names(LOCAL)?;
    let state = cookie_pair(&response, &n[1])?
        .split_once('=')
        .ok_or("state cookie")?
        .1
        .to_owned();
    let verifier = cookie_pair(&response, &n[2])?
        .split_once('=')
        .ok_or("verifier cookie")?
        .1
        .to_owned();
    assert_eq!(query["state"], state);
    assert_ne!(state.split('.').next(), Some(verifier.as_str()));
    assert_eq!(
        query["code_challenge"],
        sha256_base64url(verifier.as_bytes())
    );
    assert!(state.ends_with(".admin"));
    assert!(host.requests.borrow().is_empty());
    Ok(())
}

#[test]
fn cookie_namespace_is_stable_binding_derived_and_port_disjoint() -> Result {
    let store = Store::default();
    let host = Host::new(LOCAL);
    let local = login(&store, &host, LOCAL)?;
    assert_eq!(names(LOCAL)?, names(LOCAL)?);
    assert_ne!(names(LOCAL)?, names("http://127.0.0.1:7999")?);
    assert_ne!(names(LOCAL)?, names("https://ai.ubq.fi")?);
    for cookie in &local.set_cookies {
        assert!(
            !cookie.starts_with("__Host-")
                && !cookie.contains("Secure")
                && !cookie.contains("Domain=")
        );
        assert!(
            cookie.contains("HttpOnly")
                && cookie.contains("SameSite=Lax")
                && cookie.contains("Path=/")
                && cookie.contains("Max-Age=600")
        );
    }
    let production = login(&store, &host, "https://ai.ubq.fi")?;
    for (cookie, name) in production
        .set_cookies
        .iter()
        .zip([PASSPORT_STATE_COOKIE, PASSPORT_VERIFIER_COOKIE])
    {
        assert!(cookie.starts_with(&format!("{name}=")) && cookie.contains("; Secure;"));
    }
    let other = login(&store, &host, "http://127.0.0.1:7999")?;
    assert!(other.set_cookies.iter().all(|cookie| {
        !local
            .set_cookies
            .iter()
            .any(|old| old.split('=').next() == cookie.split('=').next())
    }));
    Ok(())
}

#[test]
fn loopback_callback_uses_matching_binding_and_creates_durable_session() -> Result {
    let store = Store::default();
    let host = Host::new(LOCAL);
    let login = login(&store, &host, LOCAL)?;
    let response = callback(&store, &host, LOCAL, &login, "one-use-code")?;
    assert_eq!(response.status, 302);
    assert_eq!(response.headers["location"], "/admin");
    let pair = cookie_pair(&response, &names(LOCAL)?[0])?;
    let token = pair.split_once('=').ok_or("session cookie")?.1;
    let row = run(store.read(&session_key(token)))?.ok_or("missing durable row")?;
    assert_eq!(row.expires_at_ms, None);
    assert_binding(row.json_value()?, LOCAL);
    let body = host
        .outbound("exchange")?
        .pop()
        .ok_or("missing PKCE exchange")?;
    assert_binding(&body, LOCAL);
    assert_eq!(body["appSession"], true);
    assert_eq!(body["code"], "one-use-code");
    assert_eq!(
        body["codeVerifier"],
        cookie_pair(&login, &names(LOCAL)?[2])?
            .split_once('=')
            .ok_or("verifier")?
            .1
    );
    assert_eq!(response.set_cookies.len(), 3);
    Ok(())
}

#[test]
fn loopback_cookie_report_introspects_current_admin_role() -> Result {
    let store = Store::default();
    let host = Host::new(LOCAL);
    let cfg = config(LOCAL);
    let cookie = session(&store, &host, LOCAL)?;
    let req = request(LOCAL, "/admin", "GET", &cookie);
    let principal = run(AuthService::new(&store, &host, &cfg).authenticate_admin(&req))?;
    assert!(principal.is_admin);
    let AuthMethod::PassportSession {
        identity: PassportSessionData::App(app),
    } = principal.method
    else {
        return Err("expected app session".into());
    };
    assert_eq!(app.binding.origin, LOCAL);
    assert!(
        !app.binding.has_valid_shape(),
        "Production/native DATA shape must not widen for local ordinary auth"
    );
    assert!(
        PassportActorDataV2::PassportAppSession(app.binding.clone())
            .to_source_json()
            .is_err()
    );
    let checked =
        run(AuthService::new(&store, &host, &cfg).check_passport_app_session(&app.binding))?;
    assert!(checked.identity.is_admin);
    let report = route(
        &store,
        &host,
        &cfg,
        &request(LOCAL, "/uos/auth", "GET", &cookie),
    )?;
    assert_eq!(report.status, 200);
    assert!(
        report
            .set_cookies
            .iter()
            .any(|cookie| cookie.contains(&format!("Max-Age={PASSPORT_COOKIE_MAX_AGE_SECONDS}")))
    );
    for body in host.outbound("session")? {
        assert_binding(&body, LOCAL);
    }
    Ok(())
}

#[test]
fn loopback_session_restart_preserves_no_expiry_and_currentness() -> Result {
    let store = Store::default();
    let host = Host::new(LOCAL);
    let pair = session(&store, &host, LOCAL)?;
    let token = pair.split_once('=').ok_or("cookie")?.1;
    let key = session_key(token);
    let row = run(store.read(&key))?.ok_or("session row")?;
    for forbidden in ["isAdmin", "handle", "scopes", "exp", "expiresAt", "token"] {
        assert!(row.json_value()?.get(forbidden).is_none());
    }
    assert!(!key.0.contains(token));
    assert_eq!(row.expires_at_ms, None);
    let reopened = Store(RefCell::new(store.0.borrow().clone()));
    let cfg = config(LOCAL);
    host.now.set(1_900_000);
    let req = request(LOCAL, "/admin", "GET", &pair);
    let principal = run(AuthService::new(&reopened, &host, &cfg).authenticate_admin(&req))?;
    assert!(principal.is_admin);
    let AuthMethod::PassportSession {
        identity: PassportSessionData::App(app),
    } = principal.method
    else {
        return Err("expected restarted app session".into());
    };
    assert!(
        run(AuthService::new(&reopened, &host, &cfg).check_passport_app_session(&app.binding))?
            .identity
            .is_admin
    );
    let mut changed = run(reopened.read(&key))?.ok_or("reopened row")?;
    changed.revision.push_str("-replaced");
    reopened.0.borrow_mut().insert(key.0, changed);
    assert!(
        run(AuthService::new(&reopened, &host, &cfg).check_passport_app_session(&app.binding))
            .is_err()
    );
    assert_eq!(store.0.borrow().len(), reopened.0.borrow().len());
    assert!(host.outbound("session")?.len() >= 1);
    Ok(())
}

#[test]
fn local_and_production_cookie_inputs_do_not_authenticate_each_other() -> Result {
    let store = Store::default();
    let host = Host::new(LOCAL);
    let local = session(&store, &host, LOCAL)?;
    *host.binding.borrow_mut() = PassportConfig::production();
    let production = session(&store, &host, "https://ai.ubq.fi")?;
    let prod_cfg = config("https://ai.ubq.fi");
    let local_cfg = config(LOCAL);
    assert!(
        run(
            AuthService::new(&store, &host, &prod_cfg).authenticate_admin(&request(
                "https://ai.ubq.fi",
                "/admin",
                "GET",
                &local
            ))
        )
        .is_err()
    );
    assert!(
        run(
            AuthService::new(&store, &host, &local_cfg).authenticate_admin(&request(
                LOCAL,
                "/admin",
                "GET",
                &production
            ))
        )
        .is_err()
    );
    *host.binding.borrow_mut() = binding(LOCAL);
    let mixed = format!("{production}; {local}");
    let report = route(
        &store,
        &host,
        &local_cfg,
        &request(LOCAL, "/uos/auth", "GET", &mixed),
    )?;
    assert_eq!(report.status, 200);
    let local_session_name = names(LOCAL)?[0].clone();
    assert!(
        report
            .set_cookies
            .iter()
            .all(|cookie| cookie.starts_with(&format!("{local_session_name}=")))
    );
    assert!(
        run(store.read(&session_key(
            production.split_once('=').ok_or("production cookie")?.1
        )))?
        .is_some()
    );
    Ok(())
}

#[test]
fn loopback_transient_failure_and_demotion_keep_existing_rules() -> Result {
    let store = Store::default();
    let host = Host::new(LOCAL);
    let cfg = config(LOCAL);
    let cookie = session(&store, &host, LOCAL)?;
    let token = cookie.split_once('=').ok_or("cookie")?.1;
    let req = request(LOCAL, "/admin", "GET", &cookie);
    host.status.set(503);
    assert_eq!(
        run(AuthService::new(&store, &host, &cfg).authenticate_admin(&req))
            .err()
            .ok_or("expected503")?
            .status,
        503
    );
    assert!(run(store.read(&session_key(token)))?.is_some());
    host.status.set(200);
    host.admin.set(false);
    assert_eq!(
        run(AuthService::new(&store, &host, &cfg).authenticate_admin(&req))
            .err()
            .ok_or("demoted")?
            .status,
        403
    );
    host.admin.set(true);
    assert!(run(AuthService::new(&store, &host, &cfg).authenticate_admin(&req))?.is_admin);
    host.status.set(401);
    assert_eq!(
        run(AuthService::new(&store, &host, &cfg).authenticate_admin(&req))
            .err()
            .ok_or("revoked")?
            .status,
        401
    );
    assert!(run(store.read(&session_key(token)))?.is_none());
    Ok(())
}

#[test]
fn loopback_callback_rejects_state_replay_duplicates_and_wrong_binding() -> Result {
    let store = Store::default();
    let host = Host::new(LOCAL);
    let cfg = config(LOCAL);
    let cookie = session(&store, &host, LOCAL)?;
    let rows = store.0.borrow().len();
    let initial_login = login(&store, &host, LOCAL)?;
    let cookies = transient(&initial_login, LOCAL)?;
    let local_session_name = names(LOCAL)?[0].clone();
    for query in [
        "",
        "state=wrong&code=x",
        "state=wrong&state=wrong&code=x",
        "state=wrong&code=x&code=y",
    ] {
        let response = route(
            &store,
            &host,
            &cfg,
            &request(
                LOCAL,
                &format!("/auth/callback?{query}"),
                "GET",
                &format!("{cookie}; {cookies}"),
            ),
        )?;
        assert_eq!(response.status, 401);
        assert_eq!(store.0.borrow().len(), rows);
        assert!(
            response
                .set_cookies
                .iter()
                .all(|c| !c.starts_with(&format!("{}=", local_session_name)))
        );
    }
    let before = host.outbound("exchange")?.len();
    let wrong = route(
        &store,
        &host,
        &cfg,
        &request(
            "http://127.0.0.1:7999",
            "/auth/callback?state=x&code=x",
            "GET",
            &cookies,
        ),
    )?;
    assert_eq!(wrong.status, 401);
    assert_eq!(host.outbound("exchange")?.len(), before);
    let first = callback(&store, &host, LOCAL, &initial_login, "replayed-code")?;
    assert_eq!(first.status, 302);
    let replay = callback(&store, &host, LOCAL, &initial_login, "replayed-code")?;
    assert!(replay.status >= 400);
    for (agent, malformed) in [(true, false), (false, true)] {
        host.agent.set(agent);
        host.malformed.set(malformed);
        let fresh = login(&store, &host, LOCAL)?;
        let response = callback(
            &store,
            &host,
            LOCAL,
            &fresh,
            &format!("bad-{agent}-{malformed}"),
        )?;
        assert!(response.status >= 400);
    }
    Ok(())
}

#[test]
fn unconfigured_host_port_and_forwarded_headers_do_not_select_binding() -> Result {
    let store = Store::default();
    let host = Host::new(LOCAL);
    let cfg = config(LOCAL);
    for origin in [
        "http://127.0.0.1:7999",
        "http://localhost:7998",
        "https://attacker.example",
    ] {
        let mut req = request(origin, "/auth/login", "GET", "");
        req.headers.extend(BTreeMap::from([
            ("origin".into(), LOCAL.into()),
            ("x-forwarded-host".into(), "127.0.0.1:7998".into()),
            ("x-forwarded-proto".into(), "http".into()),
        ]));
        assert_eq!(route(&store, &host, &cfg, &req)?.status, 401);
    }
    let missing = AuthConfig {
        passport: None,
        ..config(LOCAL)
    };
    assert_eq!(
        route(
            &store,
            &host,
            &missing,
            &request(LOCAL, "/auth/login", "GET", "")
        )?
        .status,
        401
    );
    let mut forwarded = request("http://ai.ubq.fi", "/auth/login", "GET", "");
    forwarded
        .headers
        .insert("x-forwarded-proto".into(), "https".into());
    assert_eq!(
        route(&store, &host, &config("https://ai.ubq.fi"), &forwarded)?.status,
        302
    );
    assert!(host.requests.borrow().is_empty());
    Ok(())
}

#[test]
fn loopback_logout_requires_same_origin_and_revokes_only_local_grant() -> Result {
    let store = Store::default();
    let host = Host::new(LOCAL);
    let cfg = config(LOCAL);
    let pair = session(&store, &host, LOCAL)?;
    let key = session_key(pair.split_once('=').ok_or("cookie")?.1);
    *host.binding.borrow_mut() = PassportConfig::production();
    let production = session(&store, &host, "https://ai.ubq.fi")?;
    let production_key = session_key(production.split_once('=').ok_or("production cookie")?.1);
    *host.binding.borrow_mut() = binding(LOCAL);
    let mut req = request(LOCAL, "/auth/logout", "POST", &pair);
    req.headers
        .insert("origin".into(), "http://127.0.0.1:7999".into());
    assert_eq!(route(&store, &host, &cfg, &req)?.status, 403);
    assert!(run(store.read(&key))?.is_some());
    req.headers.insert("origin".into(), LOCAL.into());
    req.headers
        .insert("sec-fetch-site".into(), "cross-site".into());
    assert_eq!(route(&store, &host, &cfg, &req)?.status, 403);
    req.headers
        .insert("sec-fetch-site".into(), "same-origin".into());
    let response = route(&store, &host, &cfg, &req)?;
    assert_eq!(response.status, 204);
    assert!(run(store.read(&key))?.is_none());
    assert_eq!(response.set_cookies.len(), 1);
    assert!(
        response.set_cookies[0].starts_with(&format!("{}=;", names(LOCAL)?[0]))
            && response.set_cookies[0].contains("Max-Age=0")
    );
    for body in host.outbound("revoke")? {
        assert_binding(&body, LOCAL);
    }
    assert!(run(store.read(&production_key))?.is_some());
    Ok(())
}

#[test]
fn unavailable_existing_ai_client_is_fail_closed_without_registration_change() -> Result {
    for status in [400, 401, 403, 503] {
        let store = Store::default();
        let host = Host::new(LOCAL);
        host.exchange_status.set(status);
        let login = login(&store, &host, LOCAL)?;
        let response = callback(&store, &host, LOCAL, &login, "unavailable-client")?;
        assert!(response.status >= 400);
        assert!(store.0.borrow().is_empty());
        assert!(host.requests.borrow().iter().all(|request| matches!(
            request,
            AuthUpstreamRequest::PassportPkceExchange { .. } | AuthUpstreamRequest::PassportJwks
        )));
        assert_eq!(host.outbound("exchange")?.len(), 1);
    }
    Ok(())
}

const PRIVATE_PEM: &str = "-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQC2KZoerxCpwvKR
/jhLxhtSgAwXFc024RC923ofAAkWD2O1OFDdktMX1i7/gHqE1kyquoSCTPM8AJvG
Y8HKsFWnWrH+sMrgq2gMjrt1cM0s9vL2MnQvLELUz7MDoxzrFCXQnsdhzlUWL9Pv
h3ZH2wlv2IVjHmd8iS4Y9rIu52jP/uyGQa8GmruSRRHxE4Qo5pvmM/QHT75zkb0K
Z1JBH/8r5vDbQIU+PP5Gt4EgN2UV1hBvHaXmJ8r7QTEHunwnDTBZqA/ny5zH03q5
gcBDwA0RroWhyutjbWm2z+8s22XpY4GgTr4AKeMO1ynfy35BqaLRNqG+lHfVzV1k
FU5UYwJ3AgMBAAECggEABDxrbRM9G/WAloyG2pw8vmpj3ZWQNBp/vSqBaH8jtvOc
qfYpSz3C0ucUKLTgJH6nQcEcm7sWXOrWtIJVW1VGaKHfScDqttbFkKUxDsYUv1ru
w3Lg6MfqtfAuhYMhhQXu6y8GC+w0ZlOuZf+MbJdXXNZ4I5ntZiP6gnIaKqUO9uc4
isQI0viUHym/o9331WfV3Pg6F6V2/KEBqA9OWoBLcwY2K+UeKiIeu1iUAcBGIG83
FcXA34iqdVLe0+CymttjVmjVGOD9szJzO09U/Q1R2mwo+YNIW2fXC9YULLpQxPAU
Feyk2fV2rS3bSPsJRhWgC7eMCHIRnvErGeRZauybgQKBgQD5jb3QVFpnx+wMfL5+
6CsyAzepklIcS9r18CzdQYjvQb+IHqQlMCnn3lrmrJcVaP4g/o7VoBAlC3aVUrjk
LWoUVJmyltS5Hm/QYuaH4ieDk6tTnAu2WDJY7L0tGXB+GT3HSJ6HclKQc1msGqdR
2RA41cOMNw5X6S4SQ2m4ijfkEQKBgQC63jarIDW6ow/7xrUnoDMjM0Or5/Z40Qm2
IB0oxLBEcEIVvLrv+IaDAwSw59INtVpeI9GPjbqW0gECDG9e7+VCsF27xNroVV/l
19sAjDXzBr7C4BON9QY+iLVcJSQaVQtZ7A8orbRGvasahZddRSw0rOdeGPoE4Lsr
tJWHIS1mBwKBgBQtfMKCf4jSpmhgEbsLJKr3jaGdXhxBmeO21rnRGbRWuZjU+Wq0
WT8RQIJURCcMBTg/vObS+Vs1cRIjzSVN8rzQFllVTr/lER9lT8R+6bj+0KSDAoNs
4C1GfwlNfEYF3nPlkEPdX4WkfgSnZw38Ig6mYhhNP1CmXv5FCyBLtLcxAoGASfMM
MVZSzOQgvCNbpy3HbuOnoQzvxcn7jN5Si8KiesLt9Ao29CYmnQwBvDxRLOmatGx6
TtPohizfBz+Ch1BqKqT+wwLZGl5Gz/9HBIV4CA+gSCBCvMe9S9vv4WI1UlJbZp97
ND7he6ktYBaD2v43yzStbth1gJ6qk1YoYkxn2dsCgYEAiDbzXmxw0yFb9sEsW+EG
ueB+H/hZWR6V/shBzlZXbRbGFypgbgK/7hyiw5agg6tGItw+g6E41GoAHt2WxRVe
zX/WE6ttFH3l9iyyV4B0Ye+CONXFoBqXR1FjTqKl2uehhKJJ1EL6S5UqB8dnwZEF
p1H85+tp157Y4v9cbfX8dHM=
-----END PRIVATE KEY-----";
