//! Actual C source actors, complete action CAS and signed same-store native lifecycle; no external provider/network.
use gateway_auth_contracts::{
    types::{AuthConfig, AuthHost},
    upstream::{AuthUpstreamRequest, AuthUpstreamResponse},
    webauthn::{CosePublicKey, SignatureInput, WebAuthnCrypto},
};
use gateway_auth_credentials::provider_credentials::{
    CodexNativeOwner, CodexSourceAccount, CodexSourcePool, codex_source_pool_key,
};
use gateway_auth_native_contracts::transport::NativeCapability;
use gateway_auth_native_proof::native_auth::NativeTransportIdentity;
use gateway_auth_passport_contracts::session::PassportDate;
use gateway_auth_passport_sessions::sessions::PassportSessions;
use gateway_auth_primitives::crypto::Secret;
use gateway_authority_actor_currentness::{
    app::ActorService, source_actor::SourceNativeRenewalActor,
};
use gateway_authority_contracts::{AuthorityClock, NativeRenewalCancellation, TokenSource};
use gateway_authority_keys as keys;
use gateway_authority_native_records::live::mapping_key;
use gateway_authority_paid_control_records::{
    PAID_CONTROL_CONFIG_VERSION, PaidControlConfig, PaidControlOwner, PaidSourceCheck,
};
use gateway_authority_routing_records::types::{PoolAccount, SubscriptionPool};
use gateway_contracts::identity::{PoolVersionstamp, PrincipalId, RequestContext, RequestId};
use gateway_core::{
    source_control::encode_source_control_record,
    state::{AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, StoredRecord},
};
use gateway_kv_codec::key::RecordKey;
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use std::{
    cell::{Cell, RefCell},
    collections::BTreeMap,
    future::Future,
    task::{Context, Poll, Waker},
};
pub(super) const NOW: i64 = 1_793_059_200_000;
pub(super) type TestResult = Result<(), Box<dyn std::error::Error>>;
#[derive(Default)]
pub(super) struct Store(
    pub(super) RefCell<BTreeMap<String, StoredRecord>>,
    pub(super) RefCell<Option<RecordKey>>,
);
impl AuthorityStore for Store {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        Ok(self.0.borrow().get(&key.0).cloned())
    }
    async fn commit(&self, tx: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        if tx
            .checks
            .iter()
            .any(|check| check.key == mapping_key("call", false))
            && let Some(key) = self.1.borrow_mut().take()
        {
            self.0.borrow_mut().remove(&key.0);
        }
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
pub(super) struct Host {
    pub(super) status: Cell<u16>,
    pub(super) hold: Cell<bool>,
    pub(super) admin: Cell<bool>,
    pub(super) calls: Cell<usize>,
    pub(super) now: Cell<i64>,
}
impl Default for Host {
    fn default() -> Self {
        Self {
            status: Cell::new(200),
            hold: Cell::new(false),
            admin: Cell::new(true),
            calls: Cell::new(0),
            now: Cell::new(NOW),
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
        self.now.get()
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
pub(super) async fn create(store: &Store, host: &Host) -> Result<String, GatewayError> {
    let grant = SourceJson::parse(&format!(
        r#"{{"subject":"\ud800\u0000","clientId":"ai","audience":"ai","origin":"https://ai.ubq.fi","redirectUri":"https://ai.ubq.fi/auth/callback","createdAt":"10/27/2026","token":"uos_app_session_{}"}}"#,
        "A".repeat(43)
    ))?;
    PassportSessions { store, host }
        .create(&grant, &JsString16::from_units(vec![0xd800, 0]), None)
        .await
}

pub(super) fn seed(
    store: &Store,
    key: RecordKey,
    revision: &str,
    value: impl serde::Serialize,
) -> Result<(), GatewayError> {
    store.0.borrow_mut().insert(
        key.0,
        StoredRecord::json(
            revision,
            serde_json::to_value(value).map_err(|_| unavailable())?,
            None,
        ),
    );
    Ok(())
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
pub(super) fn fixture(
    store: &Store,
    id: &NativeTransportIdentity,
    expiry: Option<i64>,
) -> Result<(), GatewayError> {
    let mut account = CodexSourceAccount {
        account_id: "account".into(),
        access_token: Secret::new("SYNTHETIC_ACCESS".into()),
        refresh_token: Secret::new("SYNTHETIC_REFRESH".into()),
        updated_at_ms: (NOW - 420_001) as f64,
        native_owner: None,
    };
    let identity = account.identity();
    account.native_owner = Some(CodexNativeOwner {
        codex_home: "/tmp/native-source".into(),
        generation_hash: identity.native_generation_hash.clone(),
        native_refreshed_at: None,
        external_user_id: Some("native-user".into()),
        managed_source: None,
    });
    let pool = SubscriptionPool {
        v: 1,
        pool_versionstamp: PoolVersionstamp("source".into()),
        accounts: vec![PoolAccount {
            account_id_hash: identity.routing_account_hash,
            admin_account_hash: identity.administrative_account_hash,
            credential_version: identity.credential_version,
            enabled: true,
        }],
    };
    seed(
        store,
        codex_source_pool_key(),
        "source",
        CodexSourcePool {
            accounts: vec![account],
            updated_at_ms: NOW as f64,
        }
        .private_source_value(),
    )?;
    seed(store, keys::pool(), "normalized", pool)?;
    seed(
        store,
        keys::paid_control_owner(),
        "owner",
        PaidControlOwner {
            v: 1,
            owner_id: id.owner_id().into(),
            owner_epoch: id.owner_epoch().into(),
            key_fingerprint: id.key_fingerprint().into(),
        },
    )?;
    let runtime = gateway_schema::catalog_store::runtime_config_key();
    seed(store, runtime.clone(), "runtime", serde_json::json!({}))?;
    seed(
        store,
        keys::paid_control_config(),
        "configuration",
        PaidControlConfig {
            v: PAID_CONTROL_CONFIG_VERSION,
            owner_id: id.owner_id().into(),
            owner_epoch: id.owner_epoch().into(),
            generation: "configuration".into(),
            configuration_fingerprint: "fingerprint".into(),
            surplus_binding: None,
            openlux_discovery_binding: None,
            openlux_binding: None,
            source_checks: vec![PaidSourceCheck {
                key: runtime.0,
                expected_revision: Some("runtime".into()),
            }],
        },
    )?;
    let document = SourceJson::parse(
        r#"{"account_id":"account","principal_id":"passport-user:\ud800\u0000"}"#,
    )?;
    store.0.borrow_mut().insert(
        mapping_key("call", false).0,
        encode_source_control_record("mapping", &document, expiry)?,
    );
    Ok(())
}

impl AuthorityClock for Host {
    fn now_ms(&self) -> Result<i64, GatewayError> {
        Ok(self.now.get())
    }
}
pub(super) struct Cancel(pub(super) bool);
impl NativeRenewalCancellation for Cancel {
    fn cancelled(&self) -> bool {
        self.0
    }
}
#[derive(Default)]
pub(super) struct Tokens(Cell<u64>);
impl TokenSource for Tokens {
    fn next_token(&self) -> Result<String, GatewayError> {
        let n = self.0.get() + 1;
        self.0.set(n);
        Ok(format!("source-action-{n}"))
    }
}
pub(super) async fn actor<'a>(
    store: &'a Store,
    host: &'a Host,
    config: &'a AuthConfig,
) -> Result<(SourceNativeRenewalActor<'a, Store>, String), GatewayError> {
    let token = create(store, host).await?;
    let request = gateway_auth_contracts::types::AuthRequest {
        method: "POST".into(),
        url: "https://ai.ubq.fi/v1/responses".into(),
        headers: BTreeMap::from([(
            "cookie".into(),
            format!("__Host-uos_ai_passport_session={token}"),
        )]),
        ..Default::default()
    };
    let principal = PrincipalId::from_source(JsString16::from_units(
        "passport-user:".encode_utf16().chain([0xd800, 0]).collect(),
    ));
    let context = RequestContext {
        request_id: RequestId("request".into()),
        principal,
        key_id: None,
        route: "live.sideband".into(),
        started_at_ms: NOW,
    };
    Ok((
        ActorService::new(store, host, config)
            .authenticate_source_client_actor(&request, &context)
            .await?,
        token,
    ))
}
#[derive(Default)]
pub(super) struct Nonces(RefCell<std::collections::BTreeSet<String>>);
impl gateway_auth_native_contracts::nonce::NativeNonceAuthority for Nonces {
    async fn consume_nonce(
        &self,
        _: &str,
        nonce: &str,
        _: i64,
        _: i64,
    ) -> Result<(), GatewayError> {
        if self.0.borrow_mut().insert(nonce.into()) {
            Ok(())
        } else {
            Err(unavailable())
        }
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
pub(super) async fn signed_owner(
    identity: &NativeTransportIdentity,
    body: &[u8],
    n: u8,
) -> Result<gateway_auth_native_proof::native_auth::AuthenticatedNativeOwner, GatewayError> {
    use gateway_auth_native_contracts::transport::NativeAuthHeaders;
    use gateway_auth_native_proof::native_auth::NativeAuthenticator;
    let path = gateway_auth_native_wire::native_read::NATIVE_CONTROL_PATH;
    let nonce = gateway_auth_primitives::crypto::base64url(&[n; 32]);
    let mac = identity.sign(path, NOW, &nonce, body)?;
    NativeAuthenticator::new(identity, &Nonces::default())
        .authenticate(
            path,
            NativeAuthHeaders {
                version: "1",
                key_id: identity.key_id(),
                time_ms: &NOW.to_string(),
                nonce: &nonce,
                mac: &mac,
            },
            body,
            NOW,
        )
        .await
}
