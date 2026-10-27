use gateway_auth_contracts::{
    passkeys::*,
    types::{AuthHost, AuthRequest},
    upstream::*,
};
use gateway_auth_passkeys::{passkeys::*, session_index};
use gateway_core::state::*;
use gateway_source::error::GatewayError;
use std::{
    cell::{Cell, RefCell},
    collections::BTreeMap,
    error::Error,
    future::Future,
    task::{Context, Poll, Waker},
};

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

#[derive(Default)]
pub struct Store {
    pub rows: RefCell<BTreeMap<String, StoredRecord>>,
    pub history: RefCell<Vec<AtomicTransition>>,
    pub conflict: Cell<bool>,
    pub unknown_before: Cell<bool>,
    pub unknown_after: Cell<bool>,
}
impl Store {
    pub fn put<T: serde::Serialize>(
        &self,
        key: RecordKey,
        value: &T,
        revision: &str,
        expiry: Option<i64>,
    ) -> Result<(), serde_json::Error> {
        self.rows.borrow_mut().insert(
            key.0,
            StoredRecord::json(revision, serde_json::to_value(value)?, expiry),
        );
        Ok(())
    }
}
impl AuthorityStore for Store {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        Ok(self.rows.borrow().get(&key.0).cloned())
    }
    async fn commit(&self, tx: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        let paired = tx.checks.len() == 2 && tx.mutations.len() == 2;
        self.history.borrow_mut().push(tx.clone());
        if paired && self.conflict.replace(false) {
            return Ok(CommitOutcome::Conflict);
        }
        if paired && self.unknown_before.replace(false) {
            return Err(GatewayError::unavailable(
                "Synthetic unknown write outcome",
                "fixture_unknown_write",
            ));
        }
        let mut rows = self.rows.borrow_mut();
        for check in tx.checks {
            if rows.get(&check.key.0).map(|row| &row.revision) != check.expected_revision.as_ref() {
                return Ok(CommitOutcome::Conflict);
            }
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
        if paired && self.unknown_after.replace(false) {
            return Err(GatewayError::unavailable(
                "Synthetic unknown write outcome",
                "fixture_unknown_write",
            ));
        }
        Ok(CommitOutcome::Committed)
    }
}

pub struct Host {
    pub now: Cell<i64>,
    serial: Cell<u64>,
}
impl Default for Host {
    fn default() -> Self {
        Self {
            now: Cell::new(1_000_000),
            serial: Cell::new(1),
        }
    }
}
impl AuthHost for Host {
    fn now_ms(&self) -> i64 {
        self.now.get()
    }
    fn random_bytes(&self, length: usize) -> Result<Vec<u8>, GatewayError> {
        let serial = self.serial.get();
        self.serial.set(serial + 1);
        Ok((0..length)
            .map(|index| serial.wrapping_add(index as u64) as u8)
            .collect())
    }
    async fn fetch_auth(
        &self,
        _: AuthUpstreamRequest,
    ) -> Result<AuthUpstreamResponse, GatewayError> {
        Err(GatewayError::unavailable(
            "Synthetic fixture denies network",
            "fixture_denied",
        ))
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
            "fixture_denied",
        ))
    }
}
impl gateway_auth_contracts::webauthn::WebAuthnCrypto for Host {
    async fn validate_key(
        &self,
        key: &gateway_auth_contracts::webauthn::CosePublicKey,
    ) -> Result<(), GatewayError> {
        verifying(key).map(|_| ())
    }
    async fn verify_signature(
        &self,
        input: gateway_auth_contracts::webauthn::SignatureInput,
    ) -> Result<bool, GatewayError> {
        use p256::ecdsa::signature::Verifier;
        let signature =
            p256::ecdsa::Signature::from_slice(&input.signature).map_err(|_| crypto_error())?;
        Ok(verifying(&input.key)?
            .verify(&input.data, &signature)
            .is_ok())
    }
}
fn crypto_error() -> GatewayError {
    gateway_auth_primitives::error::invalid("Invalid fixture P256 key or signature")
}
fn verifying(
    key: &gateway_auth_contracts::webauthn::CosePublicKey,
) -> Result<p256::ecdsa::VerifyingKey, GatewayError> {
    let mut point = vec![4];
    for field in ["x", "y"] {
        point.extend(gateway_auth_primitives::crypto::decode_base64url(
            key.jwk[field].as_str().ok_or_else(crypto_error)?,
        )?);
    }
    p256::ecdsa::VerifyingKey::from_sec1_bytes(&point).map_err(|_| crypto_error())
}
/// Real finite public login, never a widened private session/proof factory.
pub async fn create_session(
    store: &Store,
    host: &Host,
    config: &gateway_auth_contracts::types::AuthConfig,
) -> Result<PasskeySession, Box<dyn Error>> {
    use gateway_auth_primitives::crypto::base64url;
    use p256::ecdsa::{Signature, SigningKey, signature::Signer};
    use sha2::{Digest, Sha256};
    if store.read(&key("users", "fixture-user")).await?.is_none() {
        user(store, true, "fixture-user-revision")?;
    }
    let signing = SigningKey::from_bytes((&[7; 32]).into())?;
    let point = signing.verifying_key().to_encoded_point(false);
    use ciborium::value::Value as Cbor;
    let integer = |v: i64| Cbor::Integer(v.into());
    let cose = Cbor::Map(vec![
        (integer(1), integer(2)),
        (integer(3), integer(-7)),
        (integer(-1), integer(1)),
        (
            integer(-2),
            Cbor::Bytes(point.x().ok_or("missing fixture curve x")?.to_vec()),
        ),
        (
            integer(-3),
            Cbor::Bytes(point.y().ok_or("missing fixture curve y")?.to_vec()),
        ),
    ]);
    let mut encoded = Vec::new();
    ciborium::into_writer(&cose, &mut encoded)?;
    store.put(
        key("credentials", "fixture-credential"),
        &PasskeyCredential {
            credential_id: "fixture-credential".into(),
            user_id: "fixture-user".into(),
            public_key: base64url(&encoded),
            sign_count: 0,
            transports: Vec::new(),
            created_at_ms: 1,
        },
        "fixture-credential-revision",
        None,
    )?;
    let service = gateway_auth_passkeys::passkeys::PasskeyService {
        store,
        host,
        config,
    };
    let start = service
        .login_start(&AuthRequest {
            method: "POST".into(),
            url: "https://ai.ubq.fi/api/auth/login/start".into(),
            body: b"{}".to_vec(),
            ..Default::default()
        })
        .await?;
    let challenge = start
        .body
        .as_json()
        .and_then(|v| v["publicKey"]["challenge"].as_str())
        .ok_or("missing fixture challenge")?;
    let client = serde_json::to_vec(
        &serde_json::json!({"type":"webauthn.get","challenge":challenge,"origin":"https://ai.ubq.fi"}),
    )?;
    let mut authenticator = Sha256::digest(b"ai.ubq.fi").to_vec();
    authenticator.push(5);
    authenticator.extend([0, 0, 0, 1]);
    let mut message = authenticator.clone();
    message.extend(Sha256::digest(&client));
    let signature: Signature = signing.sign(&message);
    let response=service.login_finish(&AuthRequest{method:"POST".into(),url:"https://ai.ubq.fi/api/auth/login/finish".into(),body:serde_json::to_vec(&serde_json::json!({"response":{"id":"fixture-credential","rawId":"fixture-credential","type":"public-key","response":{"clientDataJSON":base64url(&client),"authenticatorData":base64url(&authenticator),"signature":base64url(signature.to_der().as_bytes())}}}))?,..Default::default()}).await?;
    let token = response
        .body
        .as_json()
        .and_then(|v| v["token"].as_str())
        .ok_or("missing fixture token")?;
    let row = store
        .read(&key("sessions", token))
        .await?
        .ok_or("missing fixture source session")?;
    Ok(serde_json::from_value(row.json_value()?.clone())?)
}
pub fn request(token: &str) -> AuthRequest {
    AuthRequest {
        method: "POST".into(),
        url: "https://ai.ubq.fi/api/auth/logout".into(),
        headers: BTreeMap::from([("authorization".into(), format!("Bearer {token}"))]),
        ..Default::default()
    }
}
pub fn user(store: &Store, admin: bool, revision: &str) -> Result<(), serde_json::Error> {
    store.put(
        key("users", "fixture-user"),
        &PasskeyUser {
            id: "fixture-user".into(),
            handle: "operator".into(),
            is_admin: admin,
            credential_ids: vec!["fixture-credential".into()],
            created_at_ms: 1,
            updated_at_ms: 1,
        },
        revision,
        None,
    )
}
pub fn session(token: &str, expiry: i64) -> PasskeySession {
    PasskeySession {
        token: token.into(),
        user_id: "fixture-user".into(),
        created_at_ms: 1,
        expires_at_ms: expiry,
        audience_origin: None,
    }
}
pub fn install(
    store: &Store,
    session: &PasskeySession,
    revision: &str,
    expiry: Option<i64>,
    indexed: bool,
) -> Result<(), Box<dyn Error>> {
    store.put(key("sessions", &session.token), session, revision, expiry)?;
    if indexed {
        let index = session_index::record_for_session(session, revision, expiry)?;
        store.rows.borrow_mut().insert(
            session_index::index_key(&session_index::digest(&session.token))?.0,
            index,
        );
    }
    Ok(())
}

// This legacy-only fixture has no JS Date runtime. Permanent session records
// fail unavailable rather than gaining an ISO parser or fabricated date grant.
impl gateway_auth_passport_contracts::session::PassportDate for Host {
    fn parse_date_ms(&self, _: &gateway_source::js_string::JsString16) -> f64 {
        f64::NAN
    }
}
