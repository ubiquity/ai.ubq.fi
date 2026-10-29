use gateway_auth::resolve::AuthService;
use gateway_auth_contracts::{
    types::{AuthConfig, AuthHost, AuthMethod, AuthRequest},
    upstream::{AuthUpstreamRequest, AuthUpstreamResponse},
    webauthn::{CosePublicKey, SignatureInput, WebAuthnCrypto},
};
use gateway_auth_passport_contracts::{
    passport::{PASSPORT_COOKIE, PassportConfig},
    session::PassportDate,
};
use gateway_auth_passport_sessions::sessions::PassportSessions;
use gateway_authority_contracts::{AuthorityClock, TokenSource};
use gateway_authority_paid_records::PaidRequest;
use gateway_contracts::{
    identity::{ApiKeyId, PaidAttemptId, RequestContext},
    model::ModelId,
    provider::PaidProvider,
};
use gateway_core::{
    source_control::decode_source_control_record,
    state::{AuthorityStore, StoredRecord},
};
use gateway_kv_codec::{
    key::RecordKey,
    tuple_key::{ScalarKeyPart, TupleKey},
};
use gateway_routing_plan::input::RoutingInput;

use gateway_routing_runtime_test_support::{
    input::{NOW, input},
    store::Store,
};
use gateway_source::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use std::{
    cell::Cell,
    collections::BTreeMap,
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
};
pub type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;

struct PassportHost {
    subject: JsString16,
    calls: Cell<usize>,
}
fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Offline journal fixture denied an unexpected operation.",
        "journal_fixture_unavailable",
    )
}
impl PassportDate for PassportHost {
    fn parse_date_ms(&self, value: &JsString16) -> f64 {
        if value == &JsString16::from_utf8("10/27/2026") {
            1_793_059_200_000.0
        } else {
            f64::NAN
        }
    }
}
impl WebAuthnCrypto for PassportHost {
    async fn validate_key(&self, _: &CosePublicKey) -> Result<(), GatewayError> {
        Err(unavailable())
    }
    async fn verify_signature(&self, _: SignatureInput) -> Result<bool, GatewayError> {
        Err(unavailable())
    }
}
impl AuthHost for PassportHost {
    fn now_ms(&self) -> i64 {
        NOW
    }
    fn random_bytes(&self, count: usize) -> Result<Vec<u8>, GatewayError> {
        Ok(vec![7; count])
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
        let subject = self.subject.to_source_json()?;
        let body = format!(
            r#"{{"authenticated":true,"session":{{"subject":{subject},"clientId":"ai","audience":"ai","origin":"https://ai.ubq.fi","redirectUri":"https://ai.ubq.fi/auth/callback","createdAt":"10/27/2026"}},"user":{{"id":{subject},"handle":"fixture","isAdmin":false,"scopes":[]}}}}"#,
            subject = subject.as_str()
        );
        Ok(AuthUpstreamResponse {
            status: 200,
            body: body.into_bytes(),
        })
    }
}

pub async fn authenticated_input(
    store: &Store,
    id: &str,
    subject: Vec<u16>,
) -> TestResult<RoutingInput> {
    let host = PassportHost {
        subject: JsString16::from_units(subject),
        calls: Cell::new(0),
    };
    let subject = host.subject.to_source_json()?;
    let grant = SourceJson::parse(&format!(
        r#"{{"subject":{},"clientId":"ai","audience":"ai","origin":"https://ai.ubq.fi","redirectUri":"https://ai.ubq.fi/auth/callback","createdAt":"10/27/2026","token":"uos_app_session_{}"}}"#,
        subject.as_str(),
        "A".repeat(43)
    ))?;
    let token = PassportSessions { store, host: &host }
        .create(&grant, &host.subject, None)
        .await?;
    let config = AuthConfig {
        passport: Some(PassportConfig::production()),
        ..Default::default()
    };
    let request = AuthRequest {
        method: "POST".into(),
        url: "https://ai.ubq.fi/v1/responses".into(),
        headers: BTreeMap::from([("cookie".into(), format!("{PASSPORT_COOKIE}={token}"))]),
        ..Default::default()
    };
    let principal = AuthService::new(store, &host, &config)
        .authenticate_client(&request)
        .await?;
    assert!(matches!(
        &principal.method,
        AuthMethod::PassportSession { .. }
    ));
    assert_eq!(host.calls.get(), 1);
    let mut input = input(id, "gpt-test", &[]);
    input.context.principal = principal.method.principal_id(
        principal
            .token
            .as_ref()
            .map(gateway_auth_primitives::crypto::Secret::expose),
    );
    input.context.key_id = None;
    input.principal = principal;
    Ok(input)
}

pub fn key(kind: &str, id: &str) -> RecordKey {
    TupleKey::from_scalar_parts(&[
        ScalarKeyPart::String("uos_ai"),
        ScalarKeyPart::String("routing_runtime"),
        ScalarKeyPart::String("v1"),
        ScalarKeyPart::String(kind),
        ScalarKeyPart::String(id),
    ])
    .to_record_key()
}
pub async fn record(store: &Store, id: &str) -> TestResult<StoredRecord> {
    store
        .read(&key("session", id))
        .await?
        .ok_or_else(|| "missing actual routing session".into())
}
pub fn document(record: &StoredRecord) -> TestResult<SourceJson> {
    Ok(decode_source_control_record(record)?)
}
pub fn paid_request(context: &RequestContext) -> PaidRequest {
    PaidRequest {
        key_id: ApiKeyId("paid-target".into()),
        request_id: context.request_id.clone(),
        attempt_id: PaidAttemptId("paid-attempt".into()),
        provider: PaidProvider::Surplus,
        model: ModelId::from("gpt-test"),
        route: context.route.clone(),
        path: "/v1/responses".into(),
        stream: false,
        reasoning: None,
        created_at_ms: context.started_at_ms,
    }
}
pub fn replace(
    value: &SourceJson,
    name: &str,
    replacement: Option<SourceJson>,
) -> TestResult<SourceJson> {
    let name = JsString16::from_utf8(name);
    let mut fields = value.object_entries()?;
    fields.retain(|(key, _)| key != &name);
    if let Some(value) = replacement {
        fields.push((name, value));
    }
    Ok(source::object_keys(fields)?)
}
pub fn member(value: &SourceJson, name: &str) -> TestResult<SourceJson> {
    source::member(value, name).ok_or_else(|| format!("missing {name}").into())
}

#[derive(Clone)]
pub struct CountingTokens {
    pub store: Store,
    pub calls: Arc<AtomicU64>,
}
impl TokenSource for CountingTokens {
    fn next_token(&self) -> Result<String, GatewayError> {
        self.calls.fetch_add(1, Ordering::SeqCst);
        self.store.next_token()
    }
}
pub fn now(store: &Store) -> Result<i64, GatewayError> {
    store.now_ms()
}
