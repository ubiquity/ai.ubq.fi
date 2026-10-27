use gateway_auth_contracts::{
    types::{AuthConfig, AuthHost},
    upstream::{AuthUpstreamRequest, AuthUpstreamResponse},
    webauthn::{CosePublicKey, SignatureInput, WebAuthnCrypto},
};
use gateway_auth_passport_contracts::actor_data::PassportActorDataV2;
use gateway_auth_passport_contracts::session::PassportDate;
use gateway_auth_passport_sessions::sessions::{PassportSessions, session_key};
use gateway_authority_actor_currentness::app::ActorService;
use gateway_authority_actor_records::{
    legacy::config_digest, scope::NativeActorScope, source::StoredNativeActorV2,
};
use gateway_core::identity::{PrincipalId, RequestContext, RequestId};
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

async fn actor(
    store: &Store,
    host: &Host,
    config: &AuthConfig,
) -> Result<(StoredNativeActorV2, RequestContext, String), GatewayError> {
    let token = create(store, host).await?;
    let current = PassportSessions { store, host }
        .authenticate(&token)
        .await?;
    let principal = PrincipalId::from_source(JsString16::from_units(
        "passport-user:"
            .encode_utf16()
            .chain(current.binding.subject.as_units().iter().copied())
            .collect(),
    ));
    let context = RequestContext {
        request_id: RequestId("request".into()),
        principal: principal.clone(),
        key_id: None,
        route: "/admin".into(),
        started_at_ms: host.now_ms(),
    };
    let actor = StoredNativeActorV2 {
        v: 2,
        principal,
        scope: NativeActorScope::Admin,
        config_digest: config_digest(config)?,
        owner_revision: None,
        config_revision: None,
        runtime_revision: None,
        source_valid_until_ms: None,
        source: PassportActorDataV2::PassportAppSession(current.binding),
    };
    Ok((actor, context, token))
}
type TestResult = Result<(), Box<dyn std::error::Error>>;
#[test]
fn actual_preflight_uses_fresh_role_and_preserves_503_but_deletes_authoritative_401() -> TestResult
{
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let (actor, context, token) = actor(&store, &host, &config).await?;
        let service = ActorService::new(&store, &host, &config);
        service
            .check_app_effect(&actor, &context, NativeActorScope::Admin)
            .await?;
        host.admin.set(false);
        let error = service
            .check_app_effect(&actor, &context, NativeActorScope::Admin)
            .await
            .err()
            .ok_or("demotion accepted")?;
        assert_eq!(error.status, 403);
        host.status.set(503);
        let error = service
            .check_app_effect(&actor, &context, NativeActorScope::Admin)
            .await
            .err()
            .ok_or("503 accepted")?;
        assert_eq!(error.status, 503);
        assert!(store.read(&session_key(&token)).await?.is_some());
        host.status.set(401);
        assert!(
            service
                .check_app_effect(&actor, &context, NativeActorScope::Admin)
                .await
                .is_err()
        );
        assert!(store.read(&session_key(&token)).await?.is_none());
        Ok(())
    })
}
#[test]
fn logout_during_real_introspection_cannot_issue_a_checked_phase() -> TestResult {
    let store = Store::default();
    let host = Host::default();
    let config = AuthConfig::default();
    let (actor, context, token) = run(actor(&store, &host, &config))?;
    let service = ActorService::new(&store, &host, &config);
    host.hold.set(true);
    let mut future =
        std::pin::pin!(service.check_app_effect(&actor, &context, NativeActorScope::Admin));
    let mut cx = Context::from_waker(Waker::noop());
    assert!(future.as_mut().poll(&mut cx).is_pending());
    store.0.borrow_mut().remove(&session_key(&token).0);
    host.hold.set(false);
    assert!(run(future).is_err());
    Ok(())
}
#[test]
fn local_replacement_after_preflight_refuses_snapshot_and_original_final_cas() -> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let (actor, context, token) = actor(&store, &host, &config).await?;
        let service = ActorService::new(&store, &host, &config);
        let checked = service
            .check_app_effect(&actor, &context, NativeActorScope::Admin)
            .await?;
        let data = service
            .consume_app_snapshot(checked, &store, &actor, &context, host.now_ms())
            .await?;
        assert_eq!(data.principal(), &context.principal);
        let key = session_key(&token);
        assert!(data.revision_checks().iter().any(|check| check.key == key));
        let second = service
            .check_app_effect(&actor, &context, NativeActorScope::Admin)
            .await?;
        let mut row = store.read(&key).await?.ok_or("missing row")?;
        row.revision = "replacement".into();
        store.0.borrow_mut().insert(key.0, row);
        assert!(
            service
                .consume_app_snapshot(second, &store, &actor, &context, host.now_ms())
                .await
                .is_err()
        );
        let outcome = store
            .commit(AtomicTransition {
                checks: data.revision_checks().to_vec(),
                mutations: vec![],
            })
            .await?;
        assert_eq!(outcome, CommitOutcome::Conflict);
        Ok(())
    })
}
#[test]
fn checked_phase_is_bound_to_actual_service_store_not_same_typed_foreign_store() -> TestResult {
    run(async {
        let store = Store::default();
        let foreign = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let (actor, context, _) = actor(&store, &host, &config).await?;
        let service = ActorService::new(&store, &host, &config);
        let checked = service
            .check_app_effect(&actor, &context, NativeActorScope::Admin)
            .await?;
        let other = ActorService::new(&foreign, &host, &config);
        assert!(
            other
                .consume_app_snapshot(checked, &foreign, &actor, &context, host.now_ms())
                .await
                .is_err()
        );
        Ok(())
    })
}
