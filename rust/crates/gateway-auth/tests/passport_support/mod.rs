//! Shared Passport fixtures: a fixed RSA key, a deterministic host and an
//! in-memory authority store with exact revision fences.
use gateway_auth::{
    crypto::base64url,
    passport::{PASSPORT_STATE_COOKIE, PassportConfig, PassportService},
    types::{AuthConfig, AuthHost, AuthRequest},
    upstream::{AuthUpstreamRequest, AuthUpstreamResponse},
    webauthn::{CosePublicKey, SignatureInput, WebAuthnCrypto},
};
use gateway_core::{
    error::GatewayError,
    state::{AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, StoredRecord},
};
use rsa::{
    RsaPrivateKey,
    pkcs1v15::SigningKey,
    pkcs8::DecodePrivateKey,
    signature::{SignatureEncoding, Signer},
    traits::PublicKeyParts,
};
use serde_json::{Value, json};
use sha2::Sha256;
use std::{
    cell::{Cell, RefCell},
    collections::BTreeMap,
    future::Future,
    task::{Context, Poll, Waker},
};

pub type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;
/// Converts a missing value or a failed result into the test's error instead of a panic.
pub trait Must<T> {
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

pub const PRIVATE_PEM: &str = "-----BEGIN PRIVATE KEY-----
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

pub fn run<F: Future>(future: F) -> F::Output {
    let mut context = Context::from_waker(Waker::noop());
    let mut future = std::pin::pin!(future);
    loop {
        if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
            return value;
        }
        std::thread::yield_now();
    }
}

pub fn private() -> TestResult<RsaPrivateKey> {
    RsaPrivateKey::from_pkcs8_pem(PRIVATE_PEM).must()
}

pub fn public_jwk(kid: &str) -> TestResult<Value> {
    let public = private()?.to_public_key();
    Ok(json!({
        "kty":"RSA",
        "kid":kid,
        "alg":"RS256",
        "use":"sig",
        "n":base64url(&public.n().to_bytes_be()),
        "e":base64url(&public.e().to_bytes_be()),
    }))
}

pub fn jwt(header: Value, claims: &Value) -> TestResult<String> {
    let header = base64url(serde_json::to_string(&header).must()?.as_bytes());
    let payload = base64url(serde_json::to_string(claims).must()?.as_bytes());
    let signing = SigningKey::<Sha256>::new(private()?);
    let signature = signing.sign(format!("{header}.{payload}").as_bytes());
    Ok(format!(
        "{header}.{payload}.{}",
        base64url(&signature.to_vec())
    ))
}

pub fn claims() -> Value {
    json!({
        "iss":"https://passport.ubq.fi",
        "sub":"user-123",
        "handle":"alice",
        "isAdmin":true,
        "scopes":["ai"],
        "aud":"ai",
        "clientId":"ai",
        "auth":"sso",
        "iat":1_000,
        "exp":2_000,
        "jti":"jti-1"
    })
}

pub fn token(claims: &Value) -> TestResult<String> {
    jwt(json!({"alg":"RS256","typ":"JWT","kid":"key-a"}), claims)
}

pub const GRANT: &str = "uos_app_session_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
pub const BINDING: [(&str, &str); 4] = [
    ("clientId", "ai"),
    ("audience", "ai"),
    ("origin", "https://ai.ubq.fi"),
    ("redirectUri", "https://ai.ubq.fi/auth/callback"),
];

pub fn bound(mut value: Value) -> Value {
    for (name, field) in BINDING {
        value[name] = json!(field);
    }
    value
}

pub struct Host {
    pub now: Cell<i64>,
    pub jwks: RefCell<Vec<u8>>,
    pub exchange: RefCell<Vec<u8>>,
    pub jwks_status: Cell<u16>,
    pub exchange_status: Cell<u16>,
    pub jwks_fetches: Cell<usize>,
    pub exchange_requests: RefCell<Vec<AuthUpstreamRequest>>,
    pub random: Cell<u8>,
    pub session_status: Cell<u16>,
    pub session_admin: Cell<bool>,
    pub session_calls: RefCell<Vec<String>>,
}

impl Host {
    pub fn new(jwks: Vec<Value>, access_token: String) -> TestResult<Self> {
        let grant = bound(
            json!({"token":GRANT,"subject":"user-123","createdAt":"2026-01-01T00:00:00.000Z"}),
        );
        Ok(Self {
            now: Cell::new(1_500_000),
            jwks: RefCell::new(serde_json::to_vec(&json!({"keys":jwks})).must()?),
            exchange: RefCell::new(
                serde_json::to_vec(
                    &json!({"accessToken":access_token,"tokenType":"Bearer","appSession":grant}),
                )
                .must()?,
            ),
            jwks_status: Cell::new(200),
            exchange_status: Cell::new(200),
            jwks_fetches: Cell::new(0),
            exchange_requests: RefCell::new(Vec::new()),
            random: Cell::new(0),
            session_status: Cell::new(200),
            session_admin: Cell::new(true),
            session_calls: RefCell::new(Vec::new()),
        })
    }
    fn session_reply(&self, request: &AuthUpstreamRequest) -> AuthUpstreamResponse {
        let headers = request.headers().into_iter().collect::<BTreeMap<_, _>>();
        assert_eq!(headers["authorization"], format!("Bearer {GRANT}"));
        let body = request.body().ok();
        let body = body.and_then(|bytes| serde_json::from_slice::<Value>(&bytes).ok());
        assert_eq!(body, Some(bound(json!({}))));
        self.session_calls.borrow_mut().push(request.url());
        let status = self.session_status.get();
        if request.url().ends_with("/revoke") {
            let status = if status == 200 { 204 } else { status };
            return AuthUpstreamResponse {
                status,
                body: Vec::new(),
            };
        }
        let session = bound(json!({"subject":"user-123","createdAt":"2026-01-01T00:00:00.000Z"}));
        let user = json!({"id":"user-123","handle":"alice","isAdmin":self.session_admin.get(),"scopes":["ai"]});
        let body = json!({"authenticated":true,"session":session,"user":user});
        match serde_json::to_vec(&body) {
            Ok(body) => AuthUpstreamResponse { status, body },
            Err(_) => AuthUpstreamResponse {
                status: 500,
                body: Vec::new(),
            },
        }
    }
}

impl AuthHost for Host {
    fn now_ms(&self) -> i64 {
        self.now.get()
    }
    fn random_bytes(&self, len: usize) -> Result<Vec<u8>, GatewayError> {
        self.random.set(self.random.get().wrapping_add(1));
        Ok(vec![self.random.get(); len])
    }
    async fn fetch_auth(
        &self,
        request: AuthUpstreamRequest,
    ) -> Result<AuthUpstreamResponse, GatewayError> {
        match &request {
            AuthUpstreamRequest::PassportJwks => {
                assert_eq!(request.url(), "https://passport.ubq.fi/api/auth/jwks");
                assert_eq!(request.method(), "GET");
                self.jwks_fetches.set(self.jwks_fetches.get() + 1);
                Ok(AuthUpstreamResponse {
                    status: self.jwks_status.get(),
                    body: self.jwks.borrow().clone(),
                })
            }
            AuthUpstreamRequest::PassportExchange { .. } => {
                assert_eq!(
                    request.url(),
                    "https://passport.ubq.fi/api/auth/sso/exchange"
                );
                assert_eq!(request.method(), "POST");
                self.exchange_requests.borrow_mut().push(request);
                Ok(AuthUpstreamResponse {
                    status: self.exchange_status.get(),
                    body: self.exchange.borrow().clone(),
                })
            }
            AuthUpstreamRequest::PassportSession(_) => {
                assert_eq!(request.method(), "POST");
                Ok(self.session_reply(&request))
            }
            _ => Err(GatewayError::unavailable(
                "Synthetic fixture denies unrelated network",
                "fixture_network_denied",
            )),
        }
    }
    async fn kernel_principal(
        &self,
        _: &str,
        _: &str,
        _: &str,
        _: &str,
    ) -> Result<String, GatewayError> {
        Err(GatewayError::unavailable(
            "Synthetic fixture denies policy creation",
            "fixture_policy_denied",
        ))
    }
}

impl WebAuthnCrypto for Host {
    async fn fetch_certificate_revocation_list(&self, _: &str) -> Result<Vec<u8>, GatewayError> {
        Err(GatewayError::unavailable(
            "Synthetic fixture does not fetch CRLs",
            "fixture_denied",
        ))
    }
    async fn validate_key(&self, _: &CosePublicKey) -> Result<(), GatewayError> {
        Err(GatewayError::unavailable(
            "Synthetic fixture does not execute ceremonies",
            "fixture_denied",
        ))
    }
    async fn verify_signature(&self, _: SignatureInput) -> Result<bool, GatewayError> {
        Err(GatewayError::unavailable(
            "Synthetic fixture does not execute ceremonies",
            "fixture_denied",
        ))
    }
}

#[derive(Default)]
pub struct Store {
    pub rows: RefCell<BTreeMap<String, StoredRecord>>,
}
impl AuthorityStore for Store {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        Ok(self.rows.borrow().get(&key.0).cloned())
    }
    async fn commit(&self, transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        let mut rows = self.rows.borrow_mut();
        for check in &transition.checks {
            if rows.get(&check.key.0).map(|record| &record.revision)
                != check.expected_revision.as_ref()
            {
                return Ok(CommitOutcome::Conflict);
            }
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

pub fn config() -> AuthConfig {
    AuthConfig {
        passport: Some(PassportConfig::production()),
        ..Default::default()
    }
}

pub fn request(method: &str, url: &str) -> AuthRequest {
    AuthRequest {
        method: method.into(),
        url: url.into(),
        ..Default::default()
    }
}

pub fn with_cookie(mut request: AuthRequest, name: &str, value: &str) -> AuthRequest {
    request
        .headers
        .insert("cookie".into(), format!("{name}={value}"));
    request
}

pub fn cookie_value(header: &str, name: &str) -> TestResult<String> {
    let pair = header.split(';').next().must()?;
    Ok(pair.strip_prefix(&format!("{name}=")).must()?.to_owned())
}

/// Begins a real login and returns the callback request carrying its cookies.
pub fn begin(
    passport: &PassportService<'_, Store, Host>,
    return_to: &str,
) -> TestResult<AuthRequest> {
    let mut login = url::Url::parse("https://ai.ubq.fi/auth/login").must()?;
    login.query_pairs_mut().append_pair("return_to", return_to);
    let response = run(passport.login(&request("GET", login.as_str()))).must()?;
    let cookies = response
        .set_cookies
        .iter()
        .map(|value| value.split(';').next().map(str::to_owned).must())
        .collect::<TestResult<Vec<_>>>()?
        .join("; ");
    let state = cookie_value(&response.set_cookies[0], PASSPORT_STATE_COOKIE)?;
    let mut callback = request(
        "GET",
        &format!("https://ai.ubq.fi/auth/callback?state={state}&code=code-1"),
    );
    callback.headers.insert("cookie".into(), cookies);
    Ok(callback)
}
