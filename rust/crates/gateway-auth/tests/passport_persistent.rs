//! Controlled real-service tests. No network or policy-verification callback.
use gateway_auth_contracts::{
    types::{AuthHost, AuthRequest},
    upstream::{AuthUpstreamRequest, AuthUpstreamResponse},
    webauthn::{CosePublicKey, SignatureInput, WebAuthnCrypto},
};
use gateway_auth_passport::{
    browser::{PassportBrowser, authentication_error, production_request, renew_report_cookie},
    passport::reset_jwks_cache,
};
use gateway_auth_passport_contracts::{
    passport::{PASSPORT_COOKIE, PASSPORT_STATE_COOKIE},
    session::{PASSPORT_COOKIE_MAX_AGE_SECONDS, PassportDate},
};
use gateway_auth_passport_sessions::sessions::{PassportSessions, session_key, token_shape};
use gateway_auth_primitives::crypto::{base64url, sha256_base64url};
use gateway_core::{
    error::GatewayError,
    js_string::JsString16,
    source_json::SourceJson,
    state::{
        AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, RecordMutation, StoredRecord,
    },
};
use serde_json::{Value, json};
use std::{
    cell::{Cell, RefCell},
    collections::BTreeMap,
    future::Future,
    task::{Context, Poll, Waker},
};

fn run<F: Future>(future: F) -> F::Output {
    let mut cx = Context::from_waker(Waker::noop());
    let mut future = std::pin::pin!(future);
    loop {
        if let Poll::Ready(result) = future.as_mut().poll(&mut cx) {
            return result;
        }
        std::thread::yield_now();
    }
}
fn failure() -> GatewayError {
    GatewayError::unavailable("Synthetic store/network failure", "fixture_error")
}
#[derive(Default)]
struct Store {
    rows: RefCell<BTreeMap<String, StoredRecord>>,
    fail: Cell<bool>,
    conflict: Cell<bool>,
}
impl AuthorityStore for Store {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        if self.fail.get() {
            return Err(failure());
        }
        Ok(self.rows.borrow().get(&key.0).cloned())
    }
    async fn commit(&self, tx: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        if self.fail.get() {
            return Err(failure());
        }
        if self.conflict.replace(false) {
            return Ok(CommitOutcome::Conflict);
        }
        let mut rows = self.rows.borrow_mut();
        if tx
            .checks
            .iter()
            .any(|c| rows.get(&c.key.0).map(|r| &r.revision) != c.expected_revision.as_ref())
        {
            return Ok(CommitOutcome::Conflict);
        }
        for op in tx.mutations {
            match op {
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
    now: Cell<i64>,
    random: Cell<u8>,
    status: Cell<u16>,
    revoke: Cell<u16>,
    fail: Cell<bool>,
    hold: Cell<bool>,
    body: RefCell<Vec<u8>>,
    exchange: RefCell<Vec<u8>>,
    requests: RefCell<Vec<AuthUpstreamRequest>>,
}
impl Default for Host {
    fn default() -> Self {
        Self {
            now: Cell::new(1_500_000),
            random: Cell::new(0),
            status: Cell::new(200),
            revoke: Cell::new(204),
            fail: Cell::new(false),
            hold: Cell::new(false),
            body: RefCell::new(central().to_string().into_bytes()),
            exchange: RefCell::new(Vec::new()),
            requests: RefCell::new(Vec::new()),
        }
    }
}
impl PassportDate for Host {
    fn parse_date_ms(&self, value: &JsString16) -> f64 {
        // Explicit source Date.parse fixture results; no ISO-only production parser.
        match value.try_utf8().ok().as_deref() {
            Some("2026-10-27T00:00:00.000Z" | "Tue, 27 Oct 2026 00:00:00 GMT" | "10/27/2026") => {
                1_793_059_200_000.0
            }
            _ => f64::NAN,
        }
    }
}
impl WebAuthnCrypto for Host {
    async fn validate_key(&self, _: &CosePublicKey) -> Result<(), GatewayError> {
        Err(failure())
    }
    async fn verify_signature(&self, _: SignatureInput) -> Result<bool, GatewayError> {
        Err(failure())
    }
}
impl AuthHost for Host {
    fn now_ms(&self) -> i64 {
        self.now.get()
    }
    fn random_bytes(&self, n: usize) -> Result<Vec<u8>, GatewayError> {
        let next = self.random.get().wrapping_add(1);
        self.random.set(next);
        Ok(vec![next; n])
    }
    async fn kernel_principal(
        &self,
        _: &str,
        _: &str,
        _: &str,
        _: &str,
    ) -> Result<String, GatewayError> {
        Err(failure())
    }
    async fn fetch_auth(
        &self,
        request: AuthUpstreamRequest,
    ) -> Result<AuthUpstreamResponse, GatewayError> {
        self.requests.borrow_mut().push(request.clone());
        if self.fail.get() {
            return Err(failure());
        }
        match request {
            AuthUpstreamRequest::PassportSession { .. } => {
                std::future::poll_fn(|_| {
                    if self.hold.get() {
                        Poll::Pending
                    } else {
                        Poll::Ready(())
                    }
                })
                .await;
                Ok(AuthUpstreamResponse {
                    status: self.status.get(),
                    body: self.body.borrow().clone(),
                })
            }
            AuthUpstreamRequest::PassportRevoke { .. } => Ok(AuthUpstreamResponse {
                status: self.revoke.get(),
                body: Vec::new(),
            }),
            AuthUpstreamRequest::PassportPkceExchange { .. } => Ok(AuthUpstreamResponse {
                status: self.status.get(),
                body: self.exchange.borrow().clone(),
            }),
            AuthUpstreamRequest::PassportJwks => Ok(AuthUpstreamResponse {
                status: 200,
                body: serde_json::to_vec(&json!({"keys":[public_jwk().map_err(|_|failure())?]}))
                    .map_err(|_| failure())?,
            }),
            _ => Err(failure()),
        }
    }
}
fn binding() -> Value {
    json!({"clientId":"ai","audience":"ai","origin":"https://ai.ubq.fi","redirectUri":"https://ai.ubq.fi/auth/callback"})
}
fn grant() -> Value {
    let mut v = binding();
    v["subject"] = json!("user-123");
    v["createdAt"] = json!("2026-10-27T00:00:00.000Z");
    v["token"] = json!(format!("uos_app_session_{}", "A".repeat(43)));
    v
}
fn central() -> Value {
    let mut session = binding();
    session["subject"] = json!("user-123");
    session["createdAt"] = json!("2026-10-27T00:00:00.000Z");
    json!({"authenticated":true,"session":session,"user":{"id":"user-123","handle":"alice","isAdmin":true,"scopes":["ai"]}})
}
fn source(v: &Value) -> Result<SourceJson, GatewayError> {
    SourceJson::from_serializable(v)
}
async fn create(store: &Store, host: &Host) -> Result<String, GatewayError> {
    PassportSessions { store, host }
        .create(&source(&grant())?, &JsString16::from_utf8("user-123"), None)
        .await
}
fn request(path: &str, method: &str, token: Option<&str>) -> AuthRequest {
    AuthRequest {
        method: method.into(),
        url: format!("https://ai.ubq.fi{path}"),
        headers: token
            .map(|t| BTreeMap::from([("cookie".into(), format!("{PASSPORT_COOKIE}={t}"))]))
            .unwrap_or_default(),
        ..Default::default()
    }
}

#[test]
fn durable_record_is_hashed_no_expiry_no_roles_and_survives_service_restart()
-> Result<(), Box<dyn std::error::Error>> {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let token = create(&store, &host).await?;
        assert!(token_shape(&token));
        let key = session_key(&token);
        assert!(!key.0.contains(&token));
        let row = store.read(&key).await?.ok_or("missing fixture record")?;
        assert_eq!(row.expires_at_ms, None);
        let record = row.json_value()?;
        assert_eq!(record["version"], 1.0);
        assert_eq!(
            record.as_object().ok_or("missing fixture as_object")?.len(),
            8
        );
        for forbidden in ["handle", "isAdmin", "scopes", "exp", "expiresAt", "token"] {
            assert!(record.get(forbidden).is_none());
        }
        host.now.set(i64::MAX - 1);
        let reopened = PassportSessions {
            store: &store,
            host: &host,
        };
        assert!(reopened.get(&token).await?.is_admin);
        let outbound = host.requests.borrow();
        let request = outbound.last().ok_or("missing fixture last")?;
        assert_eq!(
            request.url(),
            "https://passport.ubq.fi/api/auth/sso/session"
        );
        assert_eq!(request.method(), "POST");
        assert_eq!(
            serde_json::from_slice::<Value>(&request.body()?)?,
            binding()
        );
        assert!(
            request
                .headers()
                .iter()
                .any(|(k, v)| k == "authorization" && v.starts_with("Bearer uos_app_session_"))
        );
        assert!(!request.headers().iter().any(|(_, v)| v.contains(&token)));
        Ok(())
    })
}
#[test]
fn central_demotion_and_scopes_are_current_and_503_recovers_without_relogin()
-> Result<(), Box<dyn std::error::Error>> {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let token = create(&store, &host).await?;
        let service = PassportSessions {
            store: &store,
            host: &host,
        };
        assert!(service.get(&token).await?.is_admin);
        let mut v = central();
        v["user"]["isAdmin"] = json!(false);
        v["user"]["scopes"] = json!(["new"]);
        *host.body.borrow_mut() = serde_json::to_vec(&v)?;
        let identity = service.get(&token).await?;
        assert!(!identity.is_admin);
        assert_eq!(identity.scopes, vec![JsString16::from_utf8("new")]);
        for status in [403, 500, 503, 302] {
            host.status.set(status);
            assert_eq!(
                service
                    .get(&token)
                    .await
                    .err()
                    .ok_or("expected fixture refusal")?
                    .status,
                503
            );
            assert!(store.read(&session_key(&token)).await?.is_some());
        }
        host.fail.set(true);
        assert_eq!(
            service
                .get(&token)
                .await
                .err()
                .ok_or("expected fixture refusal")?
                .status,
            503
        );
        host.fail.set(false);
        host.status.set(200);
        assert!(!service.get(&token).await?.is_admin);
        host.status.set(401);
        assert_eq!(
            service
                .get(&token)
                .await
                .err()
                .ok_or("expected fixture refusal")?
                .status,
            401
        );
        assert!(store.read(&session_key(&token)).await?.is_none());
        Ok(())
    })
}
#[test]
fn malformed_records_responses_and_substituted_binding_preserve_recoverable_state()
-> Result<(), Box<dyn std::error::Error>> {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let token = create(&store, &host).await?;
        let service = PassportSessions {
            store: &store,
            host: &host,
        };
        let mut cases = vec![json!(null), json!({}), central()];
        cases[2]["authenticated"] = json!(false);
        for field in [
            "clientId",
            "audience",
            "origin",
            "redirectUri",
            "subject",
            "createdAt",
        ] {
            let mut v = central();
            v["session"][field] = json!("wrong");
            cases.push(v);
        }
        for (field, bad) in [
            ("id", json!("other")),
            ("handle", json!("x".repeat(501))),
            ("isAdmin", json!(1)),
            ("scopes", json!([1])),
            ("scopes", json!(vec!["x"; 129])),
            ("scopes", json!(["x".repeat(501)])),
        ] {
            let mut v = central();
            v["user"][field] = bad;
            cases.push(v);
        }
        for v in cases {
            *host.body.borrow_mut() = serde_json::to_vec(&v)?;
            assert_eq!(
                service
                    .get(&token)
                    .await
                    .err()
                    .ok_or("expected fixture refusal")?
                    .status,
                503
            );
            assert!(store.read(&session_key(&token)).await?.is_some());
        }
        *host.body.borrow_mut() = vec![b'x'; 65_537];
        assert_eq!(
            service
                .get(&token)
                .await
                .err()
                .ok_or("expected fixture refusal")?
                .status,
            503
        );
        let key = session_key(&token);
        let mut row = store.read(&key).await?.ok_or("missing fixture record")?;
        row.expires_at_ms = Some(2_000_000);
        store.rows.borrow_mut().insert(key.0, row);
        assert_eq!(
            service
                .get(&token)
                .await
                .err()
                .ok_or("expected fixture refusal")?
                .status,
            503
        );
        assert_eq!(
            service
                .get(&format!("uos_ai_passport_session_{}", "B".repeat(43)))
                .await
                .err()
                .ok_or("expected fixture refusal")?
                .status,
            401
        );
        Ok(())
    })
}
#[test]
fn grant_validation_and_failed_atomic_creation_never_remove_previous_authority()
-> Result<(), Box<dyn std::error::Error>> {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let first = create(&store, &host).await?;
        let service = PassportSessions {
            store: &store,
            host: &host,
        };
        for field in [
            "subject",
            "createdAt",
            "token",
            "clientId",
            "audience",
            "origin",
            "redirectUri",
        ] {
            let mut bad = grant();
            bad[field] = json!("wrong");
            assert_eq!(
                service
                    .create(
                        &source(&bad)?,
                        &JsString16::from_utf8("user-123"),
                        Some(&first)
                    )
                    .await
                    .err()
                    .ok_or("expected fixture refusal")?
                    .status,
                503
            );
            assert!(store.read(&session_key(&first)).await?.is_some());
        }
        store.conflict.set(true);
        assert_eq!(
            service
                .create(
                    &source(&grant())?,
                    &JsString16::from_utf8("user-123"),
                    Some(&first)
                )
                .await
                .err()
                .ok_or("expected fixture refusal")?
                .status,
            503
        );
        assert!(store.read(&session_key(&first)).await?.is_some());
        let second = service
            .create(
                &source(&grant())?,
                &JsString16::from_utf8("user-123"),
                Some(&first),
            )
            .await?;
        assert_ne!(first, second);
        assert!(store.read(&session_key(&first)).await?.is_none());
        assert!(store.read(&session_key(&second)).await?.is_some());
        assert!(
            host.requests
                .borrow()
                .iter()
                .any(|r| matches!(r, AuthUpstreamRequest::PassportRevoke { .. }))
        );
        Ok(())
    })
}
#[test]
fn successful_introspection_cannot_race_logout_or_replacement()
-> Result<(), Box<dyn std::error::Error>> {
    run(async {
        for replacement in [false, true] {
            let store = Store::default();
            let host = Host::default();
            let token = create(&store, &host).await?;
            let service = PassportSessions {
                store: &store,
                host: &host,
            };
            host.hold.set(true);
            let mut pending = std::pin::pin!(service.get(&token));
            let mut cx = Context::from_waker(Waker::noop());
            assert!(pending.as_mut().poll(&mut cx).is_pending());
            if replacement {
                let _ = service
                    .create(
                        &source(&grant())?,
                        &JsString16::from_utf8("user-123"),
                        Some(&token),
                    )
                    .await?;
            } else {
                assert_eq!(service.revoke(&token).await?, "revoked");
            }
            host.hold.set(false);
            assert_eq!(
                pending
                    .await
                    .err()
                    .ok_or("expected fixture refusal")?
                    .status,
                401
            );
            assert!(store.read(&session_key(&token)).await?.is_none());
        }
        Ok(())
    })
}
#[test]
fn local_first_logout_has_truthful_status_and_foreign_requests_cannot_delete()
-> Result<(), Box<dyn std::error::Error>> {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let token = create(&store, &host).await?;
        let browser = PassportBrowser {
            store: &store,
            host: &host,
        };
        let mut req = request("/auth/logout", "POST", Some(&token));
        req.headers
            .insert("origin".into(), "https://evil.invalid".into());
        assert_eq!(
            browser
                .route(&req)
                .await
                .ok_or("missing fixture route")?
                .status,
            403
        );
        assert!(store.read(&session_key(&token)).await?.is_some());
        req.headers.remove("origin");
        store.fail.set(true);
        let response = browser.route(&req).await.ok_or("missing fixture route")?;
        assert_eq!(response.status, 503);
        assert!(response.set_cookies.is_empty());
        store.fail.set(false);
        host.revoke.set(503);
        let response = browser.route(&req).await.ok_or("missing fixture route")?;
        assert_eq!(response.status, 204);
        assert_eq!(
            response.headers["x-uos-passport-upstream-revoke"],
            "unavailable"
        );
        assert_eq!(response.set_cookies.len(), 1);
        assert!(store.read(&session_key(&token)).await?.is_none());
        assert_eq!(
            browser
                .route(&req)
                .await
                .ok_or("missing fixture route")?
                .headers["x-uos-passport-upstream-revoke"],
            "not_applicable"
        );
        assert_eq!(
            browser
                .route(&request("/auth/logout", "GET", None))
                .await
                .ok_or("missing fixture route")?
                .status,
            405
        );
        Ok(())
    })
}
#[test]
fn cookies_renew_only_successful_reports_and_only_authoritative_401_clears()
-> Result<(), Box<dyn std::error::Error>> {
    let token = format!("uos_ai_passport_session_{}", "A".repeat(43));
    let req = request("/uos/auth", "GET", Some(&token));
    for status in [200, 401, 503] {
        let mut response = gateway_auth_contracts::types::AuthResponse::json(status, json!({}));
        renew_report_cookie(&req, &mut response);
        assert_eq!(response.set_cookies.len(), usize::from(status == 200));
        if status == 200 {
            assert!(
                response.set_cookies[0]
                    .contains(&format!("Max-Age={PASSPORT_COOKIE_MAX_AGE_SECONDS}"))
            );
        }
    }
    assert_eq!(
        authentication_error(&req, gateway_auth_primitives::error::unauthorized())
            .set_cookies
            .len(),
        1
    );
    assert!(authentication_error(&req, failure()).set_cookies.is_empty());
    Ok(())
}
#[test]
fn source_units_and_non_iso_dates_survive_storage_and_introspection()
-> Result<(), Box<dyn std::error::Error>> {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let mut g = grant();
        g["createdAt"] = json!("Tue, 27 Oct 2026 00:00:00 GMT");
        let raw = serde_json::to_string(&g)?.replace("user-123", r"user-\ud800");
        let subject = JsString16::from_units(vec![117, 115, 101, 114, 45, 0xd800]);
        let token = PassportSessions {
            store: &store,
            host: &host,
        }
        .create(&SourceJson::parse(&raw)?, &subject, None)
        .await?;
        let body = serde_json::to_string(&central())?
            .replace("user-123", r"user-\ud800")
            .replace("2026-10-27T00:00:00.000Z", "Tue, 27 Oct 2026 00:00:00 GMT")
            .replace("alice", r"\udfff");
        *host.body.borrow_mut() = body.into_bytes();
        let identity = PassportSessions {
            store: &store,
            host: &host,
        }
        .get(&token)
        .await?;
        assert_eq!(identity.subject, subject);
        assert_eq!(identity.handle.as_units(), &[0xdfff]);
        let row = store
            .read(&session_key(&token))
            .await?
            .ok_or("missing fixture record")?;
        assert!(row.expires_at_ms.is_none());
        assert!(row.json_value().is_err());
        Ok(())
    })
}
#[test]
fn pkce_has_independent_random_cookies_fixed_binding_and_fixed_return_paths()
-> Result<(), Box<dyn std::error::Error>> {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let browser = PassportBrowser {
            store: &store,
            host: &host,
        };
        for (return_to, target) in [("/admin", "admin"), ("https://evil.invalid", "chat")] {
            let req = request(&format!("/auth/login?return_to={return_to}"), "GET", None);
            let response = browser.route(&req).await.ok_or("missing fixture route")?;
            assert_eq!(response.status, 302);
            assert_eq!(response.set_cookies.len(), 2);
            let authorize = url::Url::parse(&response.headers["location"])?;
            let pairs = authorize.query_pairs().collect::<BTreeMap<_, _>>();
            let state = &pairs["state"];
            assert!(state.ends_with(&format!(".{target}")));
            let verifier = response.set_cookies[1]
                .split(';')
                .next()
                .ok_or("missing fixture next")?
                .split_once('=')
                .ok_or("missing fixture separator")?
                .1;
            assert_eq!(verifier.len(), 43);
            assert_ne!(&state[..43], verifier);
            assert_eq!(
                pairs["code_challenge"],
                sha256_base64url(verifier.as_bytes())
            );
            assert_eq!(pairs["code_challenge_method"], "S256");
            assert_eq!(pairs["client_id"], "ai");
            assert_eq!(pairs["redirect_uri"], "https://ai.ubq.fi/auth/callback");
            for c in response.set_cookies {
                for attr in [
                    "Max-Age=600",
                    "Path=/",
                    "HttpOnly",
                    "Secure",
                    "SameSite=Lax",
                ] {
                    assert!(c.contains(attr));
                }
            }
        }
        let mut forwarded = request("/auth/login", "GET", None);
        forwarded.url = "http://ai.ubq.fi/auth/login".into();
        assert!(!production_request(&forwarded));
        forwarded
            .headers
            .insert("x-forwarded-proto".into(), "https".into());
        assert!(production_request(&forwarded));
        forwarded.url = "https://evil.invalid/auth/login".into();
        assert_eq!(
            browser
                .route(&forwarded)
                .await
                .ok_or("missing fixture route")?
                .status,
            401
        );
        Ok(())
    })
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

fn public_jwk() -> Result<Value, Box<dyn std::error::Error>> {
    use rsa::{pkcs8::DecodePrivateKey, traits::PublicKeyParts};
    let public = rsa::RsaPrivateKey::from_pkcs8_pem(PRIVATE_PEM)?.to_public_key();
    Ok(
        json!({"kty":"RSA","kid":"persistent-fixture","alg":"RS256","use":"sig","n":base64url(&public.n().to_bytes_be()),"e":base64url(&public.e().to_bytes_be())}),
    )
}
fn signed_token(agent: bool) -> Result<String, Box<dyn std::error::Error>> {
    use rsa::{
        pkcs8::DecodePrivateKey,
        signature::{SignatureEncoding, Signer},
    };
    let header = base64url(br#"{"alg":"RS256","typ":"JWT","kid":"persistent-fixture"}"#);
    let mut claims = json!({"iss":"https://passport.ubq.fi","sub":"user-123","handle":"alice","isAdmin":true,"scopes":["ai"],"aud":"ai","clientId":"ai","auth":"sso","iat":1000,"exp":2000,"jti":"fixture-jti"});
    if agent {
        claims["agentTokenId"] = json!("agent-fixture");
    }
    let payload = base64url(&serde_json::to_vec(&claims)?);
    let input = format!("{header}.{payload}");
    let signer = rsa::pkcs1v15::SigningKey::<sha2::Sha256>::new(
        rsa::RsaPrivateKey::from_pkcs8_pem(PRIVATE_PEM)?,
    );
    Ok(format!(
        "{input}.{}",
        base64url(&signer.sign(input.as_bytes()).to_vec())
    ))
}
#[test]
fn callback_uses_pkce_and_real_signature_before_atomic_creation_and_rejects_agents()
-> Result<(), Box<dyn std::error::Error>> {
    run(async {
        for agent in [false, true] {
            reset_jwks_cache();
            let store = Store::default();
            let host = Host::default();
            *host.exchange.borrow_mut() = serde_json::to_vec(
                &json!({"accessToken":signed_token(agent)?,"appSession":grant()}),
            )?;
            let browser = PassportBrowser {
                store: &store,
                host: &host,
            };
            let state = format!("{}.admin", "A".repeat(43));
            let mut req = request(
                &format!("/auth/callback?state={state}&code=fixture-code"),
                "GET",
                None,
            );
            req.headers.insert(
                "cookie".into(),
                format!(
                    "{PASSPORT_STATE_COOKIE}={state}; __Host-uos_ai_passport_verifier={}",
                    "B".repeat(43)
                ),
            );
            let response = browser.route(&req).await.ok_or("missing fixture route")?;
            assert_eq!(response.status, if agent { 401 } else { 302 });
            assert_eq!(response.set_cookies.len(), if agent { 2 } else { 3 });
            assert_eq!(store.rows.borrow().len(), usize::from(!agent));
            if !agent {
                assert_eq!(response.headers["location"], "/admin");
                assert!(response.set_cookies[0].contains("uos_ai_passport_session_"));
            }
            let requests = host.requests.borrow();
            let exchange = requests
                .iter()
                .find(|r| matches!(r, AuthUpstreamRequest::PassportPkceExchange { .. }))
                .ok_or("missing fixture exchange")?;
            let body: Value = serde_json::from_slice(&exchange.body()?)?;
            assert_eq!(body["appSession"], true);
            assert_eq!(body["codeVerifier"], "B".repeat(43));
            assert_eq!(body["origin"], "https://ai.ubq.fi");
        }
        Ok(())
    })
}
#[test]
fn callback_validation_clears_only_transient_cookies_without_exchange_or_replacement()
-> Result<(), Box<dyn std::error::Error>> {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let previous = create(&store, &host).await?;
        let browser = PassportBrowser {
            store: &store,
            host: &host,
        };
        let state = format!("{}.chat", "A".repeat(43));
        for query in [
            "".into(),
            format!("state={state}&state={state}&code=x"),
            format!("state={state}&code=x&code=y"),
            "state=bad&code=x".into(),
            format!("state={state}&code="),
        ] {
            let mut req = request(&format!("/auth/callback?{query}"), "GET", Some(&previous));
            req.headers
                .get_mut("cookie")
                .ok_or("missing fixture cookie")?
                .push_str(&format!(
                    "; {PASSPORT_STATE_COOKIE}={state}; __Host-uos_ai_passport_verifier={}",
                    "B".repeat(43)
                ));
            let response = browser.route(&req).await.ok_or("missing fixture route")?;
            assert_eq!(response.status, 401);
            assert_eq!(response.set_cookies.len(), 2);
            assert!(
                !response
                    .set_cookies
                    .iter()
                    .any(|c| c.starts_with(&format!("{PASSPORT_COOKIE}=")))
            );
            assert!(store.read(&session_key(&previous)).await?.is_some());
        }
        assert!(host.requests.borrow().is_empty());
        Ok(())
    })
}
