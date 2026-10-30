use gateway_auth_contracts::{
    types::{AuthHost, AuthRequest},
    upstream::{AuthUpstreamRequest, AuthUpstreamResponse},
    webauthn::{CosePublicKey, SignatureInput, WebAuthnCrypto},
};
use gateway_auth_passport::browser::PassportBrowser;
use gateway_auth_passport_contracts::session::PassportDate;
use gateway_auth_passport_sessions::sessions::PassportSessions;
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, StoredRecord,
};
use gateway_kv_codec::key::RecordKey;
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use std::{
    cell::RefCell,
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
#[derive(Clone, Copy)]
enum Reply {
    Network,
    HttpFailure,
    Oversize,
    MissingBody,
    Malformed,
}
struct Host(Reply);
fn denied() -> GatewayError {
    GatewayError::unavailable("Synthetic operation failed", "fixture_denied")
}
impl PassportDate for Host {
    fn parse_date_ms(&self, value: &JsString16) -> f64 {
        if value == &JsString16::from_utf8("2026-10-27T00:00:00.000Z") {
            1793059200000.0
        } else {
            f64::NAN
        }
    }
}
impl WebAuthnCrypto for Host {
    async fn validate_key(&self, _: &CosePublicKey) -> Result<(), GatewayError> {
        Err(denied())
    }
    async fn verify_signature(&self, _: SignatureInput) -> Result<bool, GatewayError> {
        Err(denied())
    }
}
impl AuthHost for Host {
    fn now_ms(&self) -> i64 {
        1793059200000
    }
    fn random_bytes(&self, n: usize) -> Result<Vec<u8>, GatewayError> {
        Ok(vec![1; n])
    }
    async fn kernel_principal(
        &self,
        _: &str,
        _: &str,
        _: &str,
        _: &str,
    ) -> Result<String, GatewayError> {
        Err(denied())
    }
    async fn fetch_auth(
        &self,
        request: AuthUpstreamRequest,
    ) -> Result<AuthUpstreamResponse, GatewayError> {
        let AuthUpstreamRequest::PassportPkceExchange {
            code,
            code_verifier,
            ..
        } = request
        else {
            return Err(denied());
        };
        assert_eq!(code.expose(), "valid");
        assert_eq!(code_verifier.expose(), "B".repeat(43));
        match self.0 {
            Reply::Network => Err(denied()),
            Reply::HttpFailure => Ok(AuthUpstreamResponse {
                status: 503,
                body: vec![],
            }),
            Reply::Oversize => Ok(AuthUpstreamResponse {
                status: 200,
                body: vec![b'x'; 65537],
            }),
            Reply::MissingBody => Err(GatewayError::new(
                502,
                "Passport verification is unavailable.",
                Some("passport_exchange_body_missing"),
            )),
            Reply::Malformed => Ok(AuthUpstreamResponse {
                status: 200,
                body: b"{".to_vec(),
            }),
        }
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
#[test]
fn callback_error_classes_match_source_and_never_replace_previous_session()
-> Result<(), Box<dyn std::error::Error>> {
    run(async {
        for (reply, status) in [
            (Reply::Network, 401),
            (Reply::HttpFailure, 502),
            (Reply::Oversize, 502),
            (Reply::MissingBody, 502),
            (Reply::Malformed, 401),
        ] {
            let store = Store::default();
            let host = Host(reply);
            let grant = SourceJson::parse(&format!(
                r#"{{"subject":"previous","clientId":"ai","audience":"ai","origin":"https://ai.ubq.fi","redirectUri":"https://ai.ubq.fi/auth/callback","createdAt":"2026-10-27T00:00:00.000Z","token":"uos_app_session_{}"}}"#,
                "C".repeat(43)
            ))?;
            let previous = PassportSessions {
                store: &store,
                host: &host,
            }
            .create(&grant, &JsString16::from_utf8("previous"), None)
            .await?;
            let before = store.0.borrow().clone();
            let state = format!("{}.chat", "A".repeat(43));
            let request = AuthRequest {
                method: "GET".into(),
                url: format!("https://ai.ubq.fi/auth/callback?state={state}&code=valid"),
                headers: BTreeMap::from([(
                    "cookie".into(),
                    format!(
                        "__Host-uos_ai_passport_session={previous}; __Host-uos_ai_passport_state={state}; __Host-uos_ai_passport_verifier={}",
                        "B".repeat(43)
                    ),
                )]),
                ..Default::default()
            };
            let response = PassportBrowser {
                store: &store,
                host: &host,
            }
            .route(&request)
            .await
            .ok_or("missing route")?;
            assert_eq!(response.status, status);
            assert_eq!(*store.0.borrow(), before);
            assert_eq!(response.set_cookies.len(), 2);
            assert!(
                response.set_cookies.iter().all(|c| c.contains("Max-Age=0")
                    && !c.starts_with("__Host-uos_ai_passport_session="))
            );
            assert_eq!(
                response.headers.get("cache-control").map(String::as_str),
                Some("no-store")
            );
            assert_eq!(
                response.headers.get("referrer-policy").map(String::as_str),
                Some("no-referrer")
            );
            let body: serde_json::Value = serde_json::from_slice(&response.body.to_bytes()?)?;
            assert_eq!(body["error"]["type"], "invalid_request_error");
            assert_eq!(
                body["error"]["message"],
                if status == 401 {
                    "Unauthorized"
                } else {
                    "Passport verification is unavailable."
                }
            );
            assert_eq!(
                body["error"]["code"],
                if status == 401 {
                    "invalid_api_key"
                } else {
                    "passport_unavailable"
                }
            );
        }
        Ok(())
    })
}
