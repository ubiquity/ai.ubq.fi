//! Actual C source actors and complete private action CAS; no native effect/provider/network.
use gateway_auth_contracts::{
    types::{AuthConfig, AuthHost},
    upstream::{AuthUpstreamRequest, AuthUpstreamResponse},
    webauthn::{CosePublicKey, SignatureInput, WebAuthnCrypto},
};
use gateway_auth_credentials::provider_credentials::{
    CodexNativeOwner, CodexSourceAccount, CodexSourcePool, codex_source_pool_key,
};
use gateway_auth_native_proof::native_auth::{NativeCapability, NativeTransportIdentity};
use gateway_auth_passport_contracts::session::PassportDate;
use gateway_auth_passport_sessions::sessions::{PassportSessions, session_key};
use gateway_auth_primitives::crypto::Secret;
use gateway_authority_actor_currentness::{
    app::ActorService, source_actor::SourceNativeRenewalActor,
};
use gateway_authority_contracts::{AuthorityClock, NativeRenewalCancellation, TokenSource};
use gateway_authority_keys as keys;
use gateway_authority_native_preparation::{
    SourceNativePreparationContext, SourceNativeRenewalService,
};
use gateway_authority_native_records::{
    live::mapping_key, owner::RegisteredRenewalOwner, rows, source_rows::SourceRenewalActionRowV3,
};
use gateway_authority_paid_control_records::{
    PAID_CONTROL_CONFIG_VERSION, PaidControlConfig, PaidControlOwner, PaidSourceCheck,
};
use gateway_authority_routing_records::types::{PoolAccount, SubscriptionPool};
use gateway_contracts::identity::{PoolVersionstamp, PrincipalId, RequestContext, RequestId};
use gateway_core::{
    source_control::{decode_source_control_record, encode_source_control_record},
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
const NOW: i64 = 1_793_059_200_000;
type TestResult = Result<(), Box<dyn std::error::Error>>;
#[derive(Default)]
struct Store(
    RefCell<BTreeMap<String, StoredRecord>>,
    RefCell<Option<RecordKey>>,
);
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
    now: Cell<i64>,
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

fn seed(
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
fn identity() -> Result<NativeTransportIdentity, GatewayError> {
    NativeTransportIdentity::new(
        "key".into(),
        "owner".into(),
        "epoch".into(),
        [73; 32],
        std::collections::BTreeSet::from([NativeCapability::OwnerRenewal]),
    )
}
fn fixture(
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
struct Cancel(bool);
impl NativeRenewalCancellation for Cancel {
    fn cancelled(&self) -> bool {
        self.0
    }
}
#[derive(Default)]
struct Tokens(Cell<u64>);
impl TokenSource for Tokens {
    fn next_token(&self) -> Result<String, GatewayError> {
        let n = self.0.get() + 1;
        self.0.set(n);
        Ok(format!("source-action-{n}"))
    }
}
async fn actor<'a>(
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
#[test]
fn actual_source_actor_and_complete_cas_mint_one_bound_v3_action_with_original_context()
-> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let tokens = Tokens::default();
        let id = identity()?;
        fixture(&store, &id, None)?;
        let (actor, _) = actor(&store, &host, &config).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(false);
        let service = SourceNativeRenewalService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            SourceNativePreparationContext::configured(&id, &owner, &cancel),
        );
        let action = service
            .prepare_source_live_mapped_renewal(&actor, "call")
            .await?;
        assert_eq!(action.fence().v, 3);
        assert_eq!(&action.fence().principal, actor.principal().as_source());
        let raw = store
            .read(&rows::key("action", action.action_ref()))
            .await?
            .ok_or("action")?;
        let row = SourceRenewalActionRowV3::from_source_json(&decode_source_control_record(&raw)?)?;
        assert_eq!(&row.context, actor.context());
        assert_eq!(row.fence, *action.fence());
        assert!(row.operation_id.is_none());
        let repeated = service
            .prepare_source_live_mapped_renewal(&actor, "call")
            .await?;
        assert_eq!(repeated.action_ref(), action.action_ref());
        assert_eq!(
            store
                .0
                .borrow()
                .keys()
                .filter(|key| key.starts_with(&rows::prefix().0))
                .count(),
            1
        );
        Ok(())
    })
}
#[test]
fn logout_at_mapping_revision_cas_never_mints_or_publishes_a_source_action() -> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let tokens = Tokens::default();
        let id = identity()?;
        fixture(&store, &id, None)?;
        let (actor, token) = actor(&store, &host, &config).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(false);
        let service = SourceNativeRenewalService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            SourceNativePreparationContext::configured(&id, &owner, &cancel),
        );
        *store.1.borrow_mut() = Some(session_key(&token));
        assert!(
            service
                .prepare_source_live_mapped_renewal(&actor, "call")
                .await
                .is_err()
        );
        assert!(
            !store
                .0
                .borrow()
                .keys()
                .any(|key| key.starts_with(&rows::prefix().0))
        );
        Ok(())
    })
}
#[test]
fn opaque_actor_from_same_typed_foreign_store_cannot_prepare_here() -> TestResult {
    run(async {
        let store = Store::default();
        let foreign = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let tokens = Tokens::default();
        let id = identity()?;
        fixture(&store, &id, None)?;
        let (actor, _) = actor(&foreign, &host, &config).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(false);
        let service = SourceNativeRenewalService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            SourceNativePreparationContext::configured(&id, &owner, &cancel),
        );
        assert!(
            service
                .prepare_source_live_mapped_renewal(&actor, "call")
                .await
                .is_err()
        );
        assert!(
            !store
                .0
                .borrow()
                .keys()
                .any(|key| key.starts_with(&rows::prefix().0))
        );
        Ok(())
    })
}
#[test]
fn cancellation_keeps_the_original_499_before_new_action_effects() -> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let tokens = Tokens::default();
        let id = identity()?;
        fixture(&store, &id, None)?;
        let (actor, _) = actor(&store, &host, &config).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(true);
        let service = SourceNativeRenewalService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            SourceNativePreparationContext::configured(&id, &owner, &cancel),
        );
        let error = service
            .prepare_source_live_mapped_renewal(&actor, "call")
            .await
            .err()
            .ok_or("cancel accepted")?;
        assert_eq!(error.status, 499);
        assert!(
            !store
                .0
                .borrow()
                .keys()
                .any(|key| key.starts_with(&rows::prefix().0))
        );
        Ok(())
    })
}
