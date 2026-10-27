use gateway_auth::resolve::AuthService;
use gateway_auth_contracts::{
    types::{AuthConfig, AuthHost},
    upstream::{AuthUpstreamRequest, AuthUpstreamResponse},
    webauthn::{CosePublicKey, SignatureInput, WebAuthnCrypto},
};
use gateway_auth_passport_contracts::session::PassportDate;
use gateway_auth_passport_sessions::sessions::{PassportSessions, session_key};
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, RecordMutation, StoredRecord,
};
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use std::{
    cell::{Cell, RefCell},
    collections::BTreeMap,
    future::Future,
    task::{Context, Poll, Waker},
};

#[derive(Default)]
struct Store(RefCell<BTreeMap<String, StoredRecord>>);
impl AuthorityStore for Store {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        Ok(self.0.borrow().get(&key.0).cloned())
    }
    async fn commit(&self, tx: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        let mut rows = self.0.borrow_mut();
        if tx
            .checks
            .iter()
            .any(|c| rows.get(&c.key.0).map(|r| &r.revision) != c.expected_revision.as_ref())
        {
            return Ok(CommitOutcome::Conflict);
        }
        for m in tx.mutations {
            match m {
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
    status: Cell<u16>,
    hold: Cell<bool>,
    admin: Cell<bool>,
    calls: Cell<usize>,
}
impl Default for Host {
    fn default() -> Self {
        Self {
            status: Cell::new(200),
            hold: Cell::new(false),
            admin: Cell::new(true),
            calls: Cell::new(0),
        }
    }
}
fn unavailable() -> GatewayError {
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
        1793059200000
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
fn run<F: Future>(future: F) -> F::Output {
    let mut future = std::pin::pin!(future);
    let mut cx = Context::from_waker(Waker::noop());
    loop {
        if let Poll::Ready(v) = future.as_mut().poll(&mut cx) {
            return v;
        }
        std::thread::yield_now();
    }
}
async fn create(store: &Store, host: &Host) -> Result<String, GatewayError> {
    let grant = SourceJson::parse(&format!(
        r#"{{"subject":"\ud800\u0000","clientId":"ai","audience":"ai","origin":"https://ai.ubq.fi","redirectUri":"https://ai.ubq.fi/auth/callback","createdAt":"10/27/2026","token":"uos_app_session_{}"}}"#,
        "A".repeat(43)
    ))?;
    PassportSessions { store, host }
        .create(&grant, &JsString16::from_units(vec![0xd800, 0]), None)
        .await
}
#[test]
fn concrete_auth_service_check_is_fresh_exact_and_temporary_failure_recovers()
-> Result<(), Box<dyn std::error::Error>> {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let token = create(&store, &host).await?;
        let authenticated = PassportSessions {
            store: &store,
            host: &host,
        }
        .authenticate(&token)
        .await?;
        let config = AuthConfig::default();
        let service = AuthService::new(&store, &host, &config);
        assert_eq!(
            authenticated.binding.row_revision,
            store
                .read(&session_key(&token))
                .await?
                .ok_or("row")?
                .revision
        );
        assert_eq!(authenticated.binding.subject.as_units(), [0xd800, 0]);
        assert!(
            service
                .check_passport_app_session(&authenticated.binding)
                .await?
                .identity
                .is_admin
        );
        host.admin.set(false);
        assert!(
            !service
                .check_passport_app_session(&authenticated.binding)
                .await?
                .identity
                .is_admin
        );
        host.status.set(503);
        assert_eq!(
            service
                .check_passport_app_session(&authenticated.binding)
                .await
                .err()
                .ok_or("refusal")?
                .status,
            503
        );
        assert!(store.read(&session_key(&token)).await?.is_some());
        host.status.set(200);
        assert!(
            service
                .check_passport_app_session(&authenticated.binding)
                .await
                .is_ok()
        );
        let calls = host.calls.get();
        let mut wrong = authenticated.binding.clone();
        wrong.app_token_digest = "c".repeat(64);
        assert_eq!(
            service
                .check_passport_app_session(&wrong)
                .await
                .err()
                .ok_or("refusal")?
                .status,
            401
        );
        assert_eq!(host.calls.get(), calls);
        host.status.set(401);
        assert_eq!(
            service
                .check_passport_app_session(&authenticated.binding)
                .await
                .err()
                .ok_or("refusal")?
                .status,
            401
        );
        assert!(store.read(&session_key(&token)).await?.is_none());
        Ok(())
    })
}
#[test]
fn logout_and_row_replacement_win_during_bound_check() -> Result<(), Box<dyn std::error::Error>> {
    for delete in [false, true] {
        let store = Store::default();
        let host = Host::default();
        let token = run(create(&store, &host))?;
        let authenticated = run(PassportSessions {
            store: &store,
            host: &host,
        }
        .authenticate(&token))?;
        let config = AuthConfig::default();
        let service = AuthService::new(&store, &host, &config);
        host.hold.set(true);
        let mut check = std::pin::pin!(service.check_passport_app_session(&authenticated.binding));
        let mut cx = Context::from_waker(Waker::noop());
        assert!(check.as_mut().poll(&mut cx).is_pending());
        let key = session_key(&token);
        if delete {
            store.0.borrow_mut().remove(&key.0);
        } else {
            store.0.borrow_mut().get_mut(&key.0).ok_or("row")?.revision = "replacement".into();
        }
        host.hold.set(false);
        assert_eq!(run(check).err().ok_or("refusal")?.status, 401);
        assert_eq!(run(store.read(&key))?.is_none(), delete);
    }
    Ok(())
}

#[test]
fn actual_auth_report_preserves_source_units_null_jwt_fields_and_current_admin_role()
-> Result<(), Box<dyn std::error::Error>> {
    use gateway_auth_contracts::types::AuthRequest;
    use gateway_source::source_serialization::member;
    run(async {
        let store = Store::default();
        let host = Host::default();
        let token = create(&store, &host).await?;
        let config = AuthConfig::default();
        let auth = AuthService::new(&store, &host, &config);
        let mut request = AuthRequest {
            method: "GET".into(),
            url: "https://ai.ubq.fi/uos/auth".into(),
            headers: BTreeMap::from([(
                "cookie".into(),
                format!("__Host-uos_ai_passport_session={token}"),
            )]),
            ..Default::default()
        };
        let principal = auth.authenticate_client(&request).await?;
        let mut expected = "passport-user:".encode_utf16().collect::<Vec<_>>();
        expected.extend([0xd800, 0]);
        assert_eq!(
            principal.method.principal_id(None).as_source().as_units(),
            expected
        );
        assert!(!principal.is_super_admin);
        assert!(auth.authenticate_admin(&request).await?.is_admin);
        let response = auth.handle_auth_route(&request).await?.ok_or("report")?;
        assert_eq!(response.status, 200);
        assert_eq!(response.set_cookies.len(), 1);
        assert!(response.set_cookies[0].contains("Max-Age=34560000"));
        let document = SourceJson::parse_bytes(&response.body.to_bytes()?)?;
        let report = member(&document, "auth").ok_or("auth")?;
        assert_eq!(
            member(&report, "principal")
                .ok_or("principal")?
                .deserialize::<JsString16>()?
                .as_units(),
            expected
        );
        let method = member(&report, "method").ok_or("method")?;
        assert_eq!(
            member(&method, "kind")
                .ok_or("kind")?
                .deserialize::<String>()?,
            "passport_session"
        );
        let user = member(&method, "user").ok_or("user")?;
        for field in ["exp", "jti", "agent_token_id"] {
            assert_eq!(member(&user, field).ok_or("field")?.as_str(), "null");
        }
        assert_eq!(
            member(&user, "auth")
                .ok_or("auth")?
                .deserialize::<String>()?,
            "sso"
        );
        host.admin.set(false);
        assert_eq!(
            auth.authenticate_admin(&request)
                .await
                .err()
                .ok_or("refusal")?
                .status,
            403
        );
        host.status.set(503);
        let error = auth
            .handle_auth_route(&request)
            .await
            .err()
            .ok_or("refusal")?;
        assert_eq!(error.status, 503);
        assert!(
            gateway_auth_passport::browser::authentication_error(&request, error)
                .set_cookies
                .is_empty()
        );
        assert!(store.read(&session_key(&token)).await?.is_some());
        host.status.set(401);
        let error = auth
            .handle_auth_route(&request)
            .await
            .err()
            .ok_or("refusal")?;
        assert_eq!(error.status, 401);
        assert_eq!(
            gateway_auth_passport::browser::authentication_error(&request, error)
                .set_cookies
                .len(),
            1
        );
        assert!(store.read(&session_key(&token)).await?.is_none());
        request.url = "https://ai.ubq.fi/auth/logout".into();
        assert_eq!(
            auth.handle_auth_route(&request)
                .await?
                .ok_or("route")?
                .status,
            405
        );
        Ok(())
    })
}
