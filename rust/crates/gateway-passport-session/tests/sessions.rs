//! Ports `tests/passport-sessions.test.ts` against the actual store contract.
use gateway_core::{
    error::GatewayError,
    state::{AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, StoredRecord},
};
use gateway_passport_session::{
    binding::{Binding, is_persistent_session, pkce_challenge, sha256_hex},
    call::{SessionCall, SessionHost, SessionReply},
    record::session_key,
    service::{Identity, PassportSessions, RevokeOutcome},
};
use serde_json::{Value, json};
use std::{
    cell::{Cell, RefCell},
    collections::BTreeMap,
    future::Future,
    task::{Context, Poll, Waker},
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

const GRANT: &str = "uos_app_session_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

fn run<F: Future>(future: F) -> F::Output {
    let mut context = Context::from_waker(Waker::noop());
    let mut future = std::pin::pin!(future);
    loop {
        if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
            return value;
        }
    }
}

fn binding() -> Binding {
    Binding {
        client_id: "ai".into(),
        audience: "ai".into(),
        origin: "https://ai.ubq.fi".into(),
        redirect_uri: "https://ai.ubq.fi/auth/callback".into(),
    }
}

fn grant() -> Value {
    json!({
        "token": GRANT,
        "subject": "fixture-admin",
        "clientId": "ai",
        "audience": "ai",
        "origin": "https://ai.ubq.fi",
        "redirectUri": "https://ai.ubq.fi/auth/callback",
        "createdAt": "2000-01-01T00:00:00.000Z",
    })
}

#[derive(Default)]
struct Store {
    rows: RefCell<BTreeMap<String, StoredRecord>>,
    unavailable: Cell<bool>,
}

impl AuthorityStore for Store {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        if self.unavailable.get() {
            return Err(GatewayError::unavailable("fixture store", "fixture"));
        }
        Ok(self.rows.borrow().get(&key.0).cloned())
    }
    async fn commit(&self, transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        if self.unavailable.get() {
            return Err(GatewayError::unavailable("fixture store", "fixture"));
        }
        let mut rows = self.rows.borrow_mut();
        let fenced = transition.checks.iter().all(|check| {
            rows.get(&check.key.0).map(|row| &row.revision) == check.expected_revision.as_ref()
        });
        if !fenced {
            return Ok(CommitOutcome::Conflict);
        }
        for mutation in transition.mutations {
            match mutation {
                gateway_core::state::RecordMutation::Put { key, record } => {
                    rows.insert(key.0, record);
                }
                gateway_core::state::RecordMutation::Delete { key } => {
                    rows.remove(&key.0);
                }
            }
        }
        Ok(CommitOutcome::Committed)
    }
}

struct Host<'a> {
    store: &'a Store,
    counter: Cell<u8>,
    status: Cell<u16>,
    body: RefCell<Option<Value>>,
    is_admin: Cell<bool>,
    transport_down: Cell<bool>,
    /// Simulates a logout committed while this introspection is in flight.
    revoke_during_read: Cell<bool>,
    reads: Cell<usize>,
    revokes: Cell<usize>,
}

impl<'a> Host<'a> {
    fn new(store: &'a Store) -> Self {
        Self {
            store,
            counter: Cell::new(0),
            status: Cell::new(200),
            body: RefCell::new(None),
            is_admin: Cell::new(true),
            transport_down: Cell::new(false),
            revoke_during_read: Cell::new(false),
            reads: Cell::new(0),
            revokes: Cell::new(0),
        }
    }
    fn session_body(&self) -> Value {
        let scopes = if self.is_admin.get() {
            json!(["admin"])
        } else {
            json!([])
        };
        json!({
            "authenticated": true,
            "session": {
                "subject": "fixture-admin",
                "clientId": "ai",
                "audience": "ai",
                "origin": "https://ai.ubq.fi",
                "redirectUri": "https://ai.ubq.fi/auth/callback",
                "createdAt": "2000-01-01T00:00:00.000Z",
            },
            "user": {"id": "fixture-admin", "handle": "administrator", "isAdmin": self.is_admin.get(), "scopes": scopes},
        })
    }
}

impl SessionHost for Host<'_> {
    fn random_bytes(&self, length: usize) -> Result<Vec<u8>, GatewayError> {
        self.counter.set(self.counter.get().wrapping_add(1));
        Ok(vec![self.counter.get(); length])
    }
    async fn fetch_session(&self, call: SessionCall) -> Result<SessionReply, GatewayError> {
        assert_eq!(call.method(), "POST");
        assert_eq!(
            serde_json::from_slice::<Value>(&call.body()?).ok(),
            Some(binding().to_json())
        );
        let headers = call.headers().into_iter().collect::<BTreeMap<_, _>>();
        assert_eq!(headers["authorization"], format!("Bearer {GRANT}"));
        assert!(!format!("{call:?}").contains(GRANT));
        if self.transport_down.get() {
            return Err(GatewayError::unavailable("synthetic outage", "fixture"));
        }
        if call.is_revoke() {
            assert_eq!(
                call.url(),
                "https://passport.ubq.fi/api/auth/sso/session/revoke"
            );
            self.revokes.set(self.revokes.get() + 1);
            let status = if self.status.get() == 200 {
                204
            } else {
                self.status.get()
            };
            return Ok(SessionReply {
                status,
                body: Vec::new(),
            });
        }
        assert_eq!(call.url(), "https://passport.ubq.fi/api/auth/sso/session");
        self.reads.set(self.reads.get() + 1);
        if self.revoke_during_read.get() {
            self.store.rows.borrow_mut().clear();
        }
        let body = self
            .body
            .borrow()
            .clone()
            .unwrap_or_else(|| self.session_body());
        Ok(SessionReply {
            status: self.status.get(),
            body: serde_json::to_vec(&body).map_err(|_| GatewayError::unavailable("x", "x"))?,
        })
    }
}

fn sessions<'a>(
    store: &'a Store,
    host: &'a Host<'a>,
    binding: &'a Binding,
) -> PassportSessions<'a, Store, Host<'a>> {
    PassportSessions {
        store,
        host,
        binding,
    }
}

fn create(
    service: &PassportSessions<'_, Store, Host<'_>>,
    previous: Option<&str>,
) -> TestResult<String> {
    run(service.create(Some(&grant()), "fixture-admin", previous)).must()
}

fn status(result: Result<impl Sized, GatewayError>) -> u16 {
    result.err().map_or(200, |error| error.status)
}

#[test]
fn permanent_sessions_store_a_hashed_local_secret_without_expiry_or_role_snapshot() -> TestResult {
    let (store, binding) = (Store::default(), binding());
    let host = Host::new(&store);
    let service = sessions(&store, &host, &binding);
    let token = create(&service, None)?;
    assert!(is_persistent_session(&token));
    let key = session_key(&token).must()?;
    let expected = gateway_core::tuple_key::TupleKey::from_json_parts(&[
        json!("uos_ai"),
        json!("auth"),
        json!("passport"),
        json!("sessions"),
        json!(sha256_hex(token.as_bytes())),
    ])
    .must()?
    .to_record_key();
    assert_eq!(key, expected);
    let rows = store.rows.borrow();
    let row = rows.get(&key.0).must()?;
    assert_eq!(row.expires_at_ms, None);
    assert_eq!(
        serde_json::to_string(row.json_value().must()?).ok(),
        Some(format!(
            r#"{{"version":1,"subject":"fixture-admin","clientId":"ai","audience":"ai","origin":"https://ai.ubq.fi","redirectUri":"https://ai.ubq.fi/auth/callback","createdAt":"2000-01-01T00:00:00.000Z","appSessionToken":"{GRANT}"}}"#
        ))
    );
    assert!(!key.0.contains(&token));
    drop(rows);
    let identity = run(service.get(&token)).must()?;
    assert!(identity.is_admin);
    assert_eq!(host.reads.get(), 1);
    Ok(())
}

#[test]
fn every_read_uses_current_role_and_a_demotion_is_seen_immediately() -> TestResult {
    let (store, binding) = (Store::default(), binding());
    let host = Host::new(&store);
    let service = sessions(&store, &host, &binding);
    let token = create(&service, None)?;
    assert!(run(service.get(&token)).must()?.is_admin);
    host.is_admin.set(false);
    assert_eq!(
        run(service.get(&token)).ok(),
        Some(Identity {
            subject: "fixture-admin".into(),
            handle: "administrator".into(),
            is_admin: false,
            scopes: Vec::new(),
        })
    );
    assert_eq!(host.reads.get(), 2);
    assert_eq!(store.rows.borrow().len(), 1);
    Ok(())
}

#[test]
fn unavailable_introspection_keeps_the_local_session_and_recovers() -> TestResult {
    let (store, binding) = (Store::default(), binding());
    let host = Host::new(&store);
    let service = sessions(&store, &host, &binding);
    let token = create(&service, None)?;
    host.status.set(503);
    assert_eq!(status(run(service.get(&token))), 503);
    assert_eq!(store.rows.borrow().len(), 1);
    host.status.set(200);
    host.transport_down.set(true);
    assert_eq!(status(run(service.get(&token))), 503);
    assert_eq!(store.rows.borrow().len(), 1);
    host.transport_down.set(false);
    *host.body.borrow_mut() = Some(json!("not an object"));
    assert_eq!(status(run(service.get(&token))), 503);
    *host.body.borrow_mut() = None;
    assert_eq!(run(service.get(&token)).must()?.subject, "fixture-admin");
    Ok(())
}

#[test]
fn authoritative_revocation_deletes_local_authority() -> TestResult {
    let (store, binding) = (Store::default(), binding());
    let host = Host::new(&store);
    let service = sessions(&store, &host, &binding);
    let token = create(&service, None)?;
    host.status.set(401);
    assert_eq!(status(run(service.get(&token))), 401);
    assert_eq!(store.rows.borrow().len(), 0);
    host.status.set(200);
    assert_eq!(status(run(service.get(&token))), 401);
    assert_eq!(host.reads.get(), 1);
    Ok(())
}

#[test]
fn creation_rejects_every_wrong_binding_or_subject_without_replacing_a_session() -> TestResult {
    let (store, binding) = (Store::default(), binding());
    let host = Host::new(&store);
    let service = sessions(&store, &host, &binding);
    let token = create(&service, None)?;
    for (field, value) in [
        ("clientId", json!("other")),
        ("audience", json!("other")),
        ("origin", json!("https://evil.example")),
        ("redirectUri", json!("https://ai.ubq.fi/other")),
        ("subject", json!("other")),
        ("token", json!("legacy.jwt.token")),
        ("createdAt", json!("not a time")),
    ] {
        let mut patched = grant();
        patched[field] = value;
        let result = run(service.create(Some(&patched), "fixture-admin", Some(&token)));
        assert_eq!(status(result), 503, "{field}");
        assert_eq!(store.rows.borrow().len(), 1);
    }
    assert_eq!(
        status(run(service.create(None, "fixture-admin", None))),
        503
    );
    assert_eq!(status(run(service.create(Some(&grant()), "", None))), 503);
    assert_eq!(run(service.get(&token)).must()?.subject, "fixture-admin");
    Ok(())
}

#[test]
fn introspection_rejects_substituted_identities_and_bindings() -> TestResult {
    let (store, binding) = (Store::default(), binding());
    let host = Host::new(&store);
    let service = sessions(&store, &host, &binding);
    let token = create(&service, None)?;
    for (section, field, value) in [
        ("session", "audience", json!("other")),
        ("session", "origin", json!("https://evil.example")),
        ("session", "createdAt", json!("2001-01-01T00:00:00.000Z")),
        ("user", "id", json!("other")),
        ("user", "isAdmin", json!("true")),
        ("user", "scopes", json!([1])),
    ] {
        let mut body = host.session_body();
        body[section][field] = value;
        *host.body.borrow_mut() = Some(body);
        assert_eq!(status(run(service.get(&token))), 503, "{section}.{field}");
        assert_eq!(store.rows.borrow().len(), 1);
    }
    Ok(())
}

#[test]
fn logout_revokes_locally_first_and_reports_unavailable_upstream_truthfully() -> TestResult {
    let (store, binding) = (Store::default(), binding());
    let host = Host::new(&store);
    let service = sessions(&store, &host, &binding);
    let token = create(&service, None)?;
    host.status.set(503);
    assert_eq!(
        run(service.revoke(&token)).ok(),
        Some(RevokeOutcome::Unavailable)
    );
    assert_eq!(store.rows.borrow().len(), 0);
    assert_eq!(status(run(service.get(&token))), 401);
    let token = create(&service, None)?;
    host.status.set(200);
    assert_eq!(
        run(service.revoke(&token)).ok(),
        Some(RevokeOutcome::Revoked)
    );
    assert_eq!(
        run(service.revoke("legacy.jwt")).ok(),
        Some(RevokeOutcome::NotApplicable)
    );
    assert_eq!(RevokeOutcome::Unavailable.as_str(), "unavailable");
    Ok(())
}

#[test]
fn local_store_failure_never_claims_a_logout() -> TestResult {
    let (store, binding) = (Store::default(), binding());
    let host = Host::new(&store);
    let service = sessions(&store, &host, &binding);
    let token = create(&service, None)?;
    store.unavailable.set(true);
    assert_eq!(status(run(service.revoke(&token))), 503);
    assert_eq!(status(run(service.get(&token))), 503);
    store.unavailable.set(false);
    assert_eq!(store.rows.borrow().len(), 1);
    assert_eq!(host.revokes.get(), 0);
    Ok(())
}

#[test]
fn logout_wins_over_an_in_flight_successful_introspection() -> TestResult {
    let (store, binding) = (Store::default(), binding());
    let host = Host::new(&store);
    let service = sessions(&store, &host, &binding);
    let token = create(&service, None)?;
    host.revoke_during_read.set(true);
    assert_eq!(status(run(service.get(&token))), 401);
    assert_eq!(store.rows.borrow().len(), 0);
    Ok(())
}

#[test]
fn a_stored_replacement_atomically_removes_the_previous_session() -> TestResult {
    let (store, binding) = (Store::default(), binding());
    let host = Host::new(&store);
    let service = sessions(&store, &host, &binding);
    let previous = create(&service, None)?;
    let replacement = create(&service, Some(&previous))?;
    assert_ne!(previous, replacement);
    assert_eq!(store.rows.borrow().len(), 1);
    assert_eq!(host.revokes.get(), 1);
    assert_eq!(status(run(service.get(&previous))), 401);
    assert_eq!(
        run(service.get(&replacement)).must()?.subject,
        "fixture-admin"
    );
    Ok(())
}

#[test]
fn malformed_tokens_and_corrupt_records_fail_closed() -> TestResult {
    let (store, binding) = (Store::default(), binding());
    let host = Host::new(&store);
    let service = sessions(&store, &host, &binding);
    assert_eq!(
        status(run(service.get("uos_ai_passport_session_short"))),
        401
    );
    let token = create(&service, None)?;
    let key = session_key(&token).must()?;
    store
        .rows
        .borrow_mut()
        .insert(key.0, StoredRecord::json("r", json!({"version": 2}), None));
    assert_eq!(status(run(service.get(&token))), 503);
    assert_eq!(host.reads.get(), 0);
    Ok(())
}

#[test]
fn pkce_challenge_is_base64url_sha256_of_the_verifier() {
    // RFC 7636 appendix B.
    assert_eq!(
        pkce_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"),
        "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"
    );
}
