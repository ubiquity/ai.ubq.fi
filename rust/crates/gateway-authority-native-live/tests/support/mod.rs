//! Ordinary synthetic store/host inputs; every actor and native proof comes from real authentication.
use gateway_auth_contracts::{
    types::{AuthConfig, AuthHost, AuthRequest},
    upstream::{AuthUpstreamRequest, AuthUpstreamResponse},
    webauthn::{CosePublicKey, SignatureInput, WebAuthnCrypto},
};
use gateway_auth_credentials::provider_credentials::{
    CodexSourceAccount, CodexSourcePool, codex_source_pool_key,
};
use gateway_auth_native_contracts::{
    nonce::NativeNonceAuthority,
    transport::{NativeAuthHeaders, NativeCapability},
};
use gateway_auth_native_proof::{
    native_auth::NativeTransportIdentity,
    native_live::{AuthenticatedNativeLive, NativeLiveAuthenticator},
};
use gateway_auth_passport_contracts::session::PassportDate;
use gateway_auth_passport_sessions::sessions::PassportSessions;
use gateway_auth_primitives::crypto::{Secret, base64url};
use gateway_authority_actor_currentness::{
    app::ActorService, source_actor::SourceNativeRenewalActor,
};
use gateway_authority_contracts::{AuthorityClock, TokenSource};
use gateway_authority_keys as keys;
use gateway_authority_native_live::{SourceNativeLiveAccess, SourceNativeLiveService};
use gateway_authority_native_records::live::mapping_key;
use gateway_authority_paid_control_records::PaidControlOwner;
use gateway_authority_routing_records::types::{PoolAccount, SubscriptionPool};
use gateway_contracts::identity::{PoolVersionstamp, PrincipalId, RequestContext, RequestId};
use gateway_core::{
    source_control::encode_source_control_record,
    state::{AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, StoredRecord},
};
use gateway_kv_codec::key::RecordKey;
use gateway_native_authority_request::live::NativeLiveAuthorityRequest;
use gateway_native_authority_wire::source_response::SourceNativeLiveAuthorityResponseV2;
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use std::{
    cell::{Cell, RefCell},
    collections::{BTreeMap, BTreeSet},
    future::Future,
    task::{Context, Poll, Waker},
};
pub(super) const NOW: i64 = 1_793_059_200_000;
pub(super) type TestResult = Result<(), Box<dyn std::error::Error>>;
pub(super) fn unavailable() -> GatewayError {
    GatewayError::unavailable("Fixture unavailable", "fixture_unavailable")
}
#[derive(Default)]
pub(super) struct Store {
    pub(super) rows: RefCell<BTreeMap<String, StoredRecord>>,
    pub(super) race: RefCell<Option<(RecordKey, Option<StoredRecord>)>>,
    pub(super) failure: Cell<u8>,
}
impl AuthorityStore for Store {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        Ok(self.rows.borrow().get(&key.0).cloned())
    }
    async fn commit(&self, tx: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        let marker = mapping_key("call", true);
        let mapping = mapping_key("call", false);
        let marker_put = tx
            .mutations
            .iter()
            .any(|m| matches!(m,RecordMutation::Put{key,..}if key==&marker));
        let mapping_delete = tx
            .mutations
            .iter()
            .any(|m| matches!(m,RecordMutation::Delete{key}if key==&mapping));
        let marker_delete = tx
            .mutations
            .iter()
            .any(|m| matches!(m,RecordMutation::Delete{key}if key==&marker));
        let fault = self.failure.get();
        if fault == 1 && marker_put {
            self.failure.set(0);
            return Err(unavailable());
        }
        if (fault == 2 && mapping_delete) || (fault == 3 && marker_delete) {
            self.failure.set(0);
            return Ok(CommitOutcome::Conflict);
        }
        if tx.checks.iter().any(|c| c.key == mapping)
            && let Some((key, value)) = self.race.borrow_mut().take()
        {
            match value {
                Some(row) => {
                    self.rows.borrow_mut().insert(key.0, row);
                }
                None => {
                    self.rows.borrow_mut().remove(&key.0);
                }
            }
        }
        let mut rows = self.rows.borrow_mut();
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
    pub(super) subject: JsString16,
    pub(super) status: Cell<u16>,
    pub(super) hold: Cell<bool>,
    pub(super) admin: Cell<bool>,
    pub(super) calls: Cell<usize>,
    pub(super) now: Cell<i64>,
}
impl Default for Host {
    fn default() -> Self {
        Self {
            subject: JsString16::from_units(vec![0xd800, 0]),
            status: Cell::new(200),
            hold: Cell::new(false),
            admin: Cell::new(true),
            calls: Cell::new(0),
            now: Cell::new(NOW),
        }
    }
}
impl PassportDate for Host {
    fn parse_date_ms(&self, value: &JsString16) -> f64 {
        if value == &JsString16::from_utf8("10/27/2026") {
            NOW as f64
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
        let subject = self.subject.to_source_json()?;
        let body = format!(
            r#"{{"authenticated":true,"session":{{"subject":{},"clientId":"ai","audience":"ai","origin":"https://ai.ubq.fi","redirectUri":"https://ai.ubq.fi/auth/callback","createdAt":"10/27/2026"}},"user":{{"id":{},"handle":"","isAdmin":{},"scopes":[]}}}}"#,
            subject.as_str(),
            subject.as_str(),
            self.admin.get()
        );
        Ok(AuthUpstreamResponse {
            status: self.status.get(),
            body: body.into_bytes(),
        })
    }
}
impl AuthorityClock for Host {
    fn now_ms(&self) -> Result<i64, GatewayError> {
        Ok(self.now.get())
    }
}
#[derive(Default)]
pub(super) struct Tokens(Cell<u64>);
impl TokenSource for Tokens {
    fn next_token(&self) -> Result<String, GatewayError> {
        let n = self.0.get() + 1;
        self.0.set(n);
        Ok(format!("source-live-{n}"))
    }
}
#[derive(Default)]
pub(super) struct Nonces(RefCell<BTreeSet<String>>);
impl NativeNonceAuthority for Nonces {
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
pub(super) struct Fixture {
    pub(super) store: Store,
    pub(super) host: Host,
    pub(super) tokens: Tokens,
    pub(super) id: NativeTransportIdentity,
    pub(super) config: AuthConfig,
    pub(super) nonces: Nonces,
    nonce: Cell<u8>,
}
impl Fixture {
    pub(super) fn new(subject: JsString16) -> Result<Self, GatewayError> {
        let f = Self {
            store: Store::default(),
            host: Host {
                subject,
                ..Default::default()
            },
            tokens: Tokens::default(),
            id: NativeTransportIdentity::new(
                "key".into(),
                "owner".into(),
                "epoch".into(),
                [73; 32],
                BTreeSet::from([NativeCapability::LiveSideband]),
            )?,
            config: AuthConfig::default(),
            nonces: Nonces::default(),
            nonce: Cell::new(0),
        };
        source(&f.store, "SYNTHETIC_ACCESS", "source")?;
        seed(
            &f.store,
            keys::paid_control_owner(),
            "owner",
            PaidControlOwner {
                v: 1,
                owner_id: f.id.owner_id().into(),
                owner_epoch: f.id.owner_epoch().into(),
                key_fingerprint: f.id.key_fingerprint().into(),
            },
        )?;
        let mapping = SourceJson::parse(&format!(
            r#"{{"account_id":"account","principal_id":{}}}"#,
            f.principal().as_source().to_source_json()?.as_str()
        ))?;
        f.store.rows.borrow_mut().insert(
            mapping_key("call", false).0,
            encode_source_control_record("mapping", &mapping, None)?,
        );
        Ok(f)
    }
    pub(super) fn principal(&self) -> PrincipalId {
        PrincipalId::from_source(JsString16::from_units(
            "passport-user:"
                .encode_utf16()
                .chain(self.host.subject.as_units().iter().copied())
                .collect(),
        ))
    }
    pub(super) fn service(&self) -> SourceNativeLiveService<'_, Store, Tokens, Host> {
        SourceNativeLiveService::new(
            &self.store,
            &self.tokens,
            Some(&self.host),
            &self.id,
            &self.host,
            &self.config,
        )
    }
    pub(super) async fn actor(
        &self,
        admin: bool,
    ) -> Result<(SourceNativeRenewalActor<'_, Store>, String), GatewayError> {
        let grant = SourceJson::parse(&format!(
            r#"{{"subject":{},"clientId":"ai","audience":"ai","origin":"https://ai.ubq.fi","redirectUri":"https://ai.ubq.fi/auth/callback","createdAt":"10/27/2026","token":"uos_app_session_{}"}}"#,
            self.host.subject.to_source_json()?.as_str(),
            "A".repeat(43)
        ))?;
        let token = PassportSessions {
            store: &self.store,
            host: &self.host,
        }
        .create(&grant, &self.host.subject, None)
        .await?;
        let request = AuthRequest {
            method: "POST".into(),
            url: "https://ai.ubq.fi/v1/responses".into(),
            headers: BTreeMap::from([(
                "cookie".into(),
                format!("__Host-uos_ai_passport_session={token}"),
            )]),
            ..Default::default()
        };
        let context = RequestContext {
            request_id: RequestId("original-request".into()),
            principal: self.principal(),
            key_id: None,
            route: "live.sideband".into(),
            started_at_ms: NOW,
        };
        let service = ActorService::new(&self.store, &self.host, &self.config);
        let actor = if admin {
            service
                .authenticate_source_admin_actor(&request, &context)
                .await?
        } else {
            service
                .authenticate_source_client_actor(&request, &context)
                .await?
        };
        Ok((actor, token))
    }
    pub(super) async fn proof(&self, body: &[u8]) -> Result<AuthenticatedNativeLive, GatewayError> {
        let n = self.nonce.get() + 1;
        self.nonce.set(n);
        signed(&self.id, &self.nonces, body, n, self.host.now.get()).await
    }
    pub(super) async fn call(
        &self,
        request: NativeLiveAuthorityRequest,
    ) -> Result<SourceNativeLiveAuthorityResponseV2<SourceNativeLiveAccess>, GatewayError> {
        let body = serde_json::to_vec(&request).map_err(|_| unavailable())?;
        self.service()
            .execute(&self.proof(&body).await?, &body)
            .await
    }
}
pub(super) async fn signed(
    id: &NativeTransportIdentity,
    replay: &Nonces,
    body: &[u8],
    n: u8,
    now: i64,
) -> Result<AuthenticatedNativeLive, GatewayError> {
    let nonce = base64url(&[n; 32]);
    let mac = id.sign_live_authority_request(now, &nonce, body)?;
    NativeLiveAuthenticator::new(id, replay)
        .authenticate_authority(
            NativeAuthHeaders {
                version: "1",
                key_id: id.key_id(),
                time_ms: &now.to_string(),
                nonce: &nonce,
                mac: &mac,
            },
            body,
            now,
        )
        .await
}
pub(super) fn source(store: &Store, access: &str, revision: &str) -> Result<(), GatewayError> {
    let account = CodexSourceAccount {
        account_id: "account".into(),
        access_token: Secret::new(access.into()),
        refresh_token: Secret::new("SYNTHETIC_REFRESH".into()),
        updated_at_ms: NOW as f64,
        native_owner: None,
    };
    let id = account.identity();
    seed(
        store,
        codex_source_pool_key(),
        revision,
        CodexSourcePool {
            accounts: vec![account],
            updated_at_ms: NOW as f64,
        }
        .private_source_value(),
    )?;
    seed(
        store,
        keys::pool(),
        revision,
        SubscriptionPool {
            v: 1,
            pool_versionstamp: PoolVersionstamp(revision.into()),
            accounts: vec![PoolAccount {
                account_id_hash: id.routing_account_hash,
                admin_account_hash: id.administrative_account_hash,
                credential_version: id.credential_version,
                enabled: true,
            }],
        },
    )
}
pub(super) fn seed(
    store: &Store,
    key: RecordKey,
    revision: &str,
    value: impl serde::Serialize,
) -> Result<(), GatewayError> {
    store.rows.borrow_mut().insert(
        key.0,
        StoredRecord::json(
            revision,
            serde_json::to_value(value).map_err(|_| unavailable())?,
            None,
        ),
    );
    Ok(())
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
