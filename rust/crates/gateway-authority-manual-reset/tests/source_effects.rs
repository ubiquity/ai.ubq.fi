//! Actual source-auth/manual transaction fixtures; no provider, network or grant callback.
use gateway_auth_credentials::provider_credentials::{
    CodexSourceAccount, CodexSourcePool, codex_source_pool_key,
};
use gateway_auth_primitives::crypto::Secret;
use gateway_authority_actor_currentness::{
    app::ActorService, source_actor::SourceNativeRenewalActor,
};
use gateway_authority_contracts::{AuthorityClock, TokenSource};
use gateway_authority_keys as keys;
use gateway_authority_manual_reset::{SourceManualResetPreparation, SourceManualResetService};
use gateway_authority_reset_records::{
    AccountDayBudget, ResetClaim, ResetConfig, ResetMode, ResetState,
};
use gateway_authority_reset_spend::{ResetSpendFactory, ResetSpendOperations, ResetSpendService};
use gateway_authority_routing_evidence::capacity::{
    CapacityObservation, CapacityState, CapacityWindow, CapacityWindows,
};
use gateway_authority_routing_records::types::{PoolAccount, SubscriptionPool};
use gateway_contracts::identity::{
    ActiveSelection, ActiveTransitionReason, PoolVersionstamp, PrincipalId, RequestContext,
    RequestId,
};
const NOW: i64 = 1_793_059_200_000;
type TestResult = Result<(), Box<dyn std::error::Error>>;
use gateway_auth_contracts::{
    types::{AuthConfig, AuthHost},
    upstream::{AuthUpstreamRequest, AuthUpstreamResponse},
    webauthn::{CosePublicKey, SignatureInput, WebAuthnCrypto},
};
use gateway_auth_passport_contracts::actor_data::PassportActorDataV2;
use gateway_auth_passport_contracts::session::PassportDate;
use gateway_auth_passport_sessions::sessions::{PassportSessions, session_key};
use gateway_authority_actor_records::{
    legacy::config_digest, scope::NativeActorScope, source::StoredNativeActorV2,
};
use gateway_core::{
    state::AtomicTransition, state::AuthorityStore, state::CommitOutcome, state::RecordMutation,
    state::StoredRecord,
};
use gateway_kv_codec::key::RecordKey;
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use std::{
    cell::{Cell, RefCell},
    collections::BTreeMap,
    future::Future,
    sync::atomic::{AtomicBool, AtomicI64, AtomicU16, AtomicUsize, Ordering},
    task::{Context, Poll, Waker},
};

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
        if let Some(key) = self.1.borrow_mut().take() {
            rows.remove(&key.0);
        }
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
    status: AtomicU16,
    hold: AtomicBool,
    admin: AtomicBool,
    calls: AtomicUsize,
    now: AtomicI64,
}
impl Default for Host {
    fn default() -> Self {
        Self {
            status: AtomicU16::new(200),
            hold: AtomicBool::new(false),
            admin: AtomicBool::new(true),
            calls: AtomicUsize::new(0),
            now: AtomicI64::new(NOW),
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
        self.now.load(Ordering::SeqCst)
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
        self.calls
            .store(self.calls.load(Ordering::SeqCst) + 1, Ordering::SeqCst);
        std::future::poll_fn(|_| {
            if self.hold.load(Ordering::SeqCst) {
                Poll::Pending
            } else {
                Poll::Ready(())
            }
        })
        .await;
        let body = format!(
            r#"{{"authenticated":true,"session":{{"subject":"\ud800\u0000","clientId":"ai","audience":"ai","origin":"https://ai.ubq.fi","redirectUri":"https://ai.ubq.fi/auth/callback","createdAt":"10/27/2026"}},"user":{{"id":"\ud800\u0000","handle":"","isAdmin":{},"scopes":[]}}}}"#,
            self.admin.load(Ordering::SeqCst)
        );
        Ok(AuthUpstreamResponse {
            status: self.status.load(Ordering::SeqCst),
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

impl AuthorityClock for Host {
    fn now_ms(&self) -> Result<i64, GatewayError> {
        Ok(self.now.load(Ordering::SeqCst))
    }
}
#[derive(Default)]
struct Tokens(Cell<u64>);
impl TokenSource for Tokens {
    fn next_token(&self) -> Result<String, GatewayError> {
        let next = self.0.get() + 1;
        self.0.set(next);
        Ok(format!("manual-token-{next}"))
    }
}
struct Terminal<'a> {
    store: &'a Store,
    tokens: &'a Tokens,
    host: &'a Host,
}
impl ResetSpendFactory for Terminal<'_> {
    type Store = Store;
    type Tokens = Tokens;
    fn reset_spend(&self) -> ResetSpendService<'_, Store, Tokens> {
        ResetSpendService::new(self.store, self.tokens, Some(self.host))
    }
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
fn pool(store: &Store) -> Result<SubscriptionPool, GatewayError> {
    let account = CodexSourceAccount {
        account_id: "source-account".into(),
        access_token: Secret::new("SYNTHETIC_ACCESS".into()),
        refresh_token: Secret::new("SYNTHETIC_REFRESH".into()),
        updated_at_ms: NOW as f64,
        native_owner: None,
    };
    let id = account.identity();
    let pool = SubscriptionPool {
        v: 1,
        pool_versionstamp: PoolVersionstamp("source".into()),
        accounts: vec![PoolAccount {
            account_id_hash: id.routing_account_hash,
            admin_account_hash: id.administrative_account_hash,
            credential_version: id.credential_version,
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
    seed(store, keys::pool(), "normalized", &pool)?;
    seed(
        store,
        keys::reset_config(),
        "reset-policy",
        ResetConfig {
            enabled: true,
            mode: ResetMode::Shadow,
            max_per_account_per_day: 1,
            max_per_account_per_window: 1,
            ..Default::default()
        },
    )?;
    Ok(pool)
}
fn usage(pool: &SubscriptionPool, at: i64, used: f64) -> CapacityObservation {
    CapacityObservation {
        slot: 0,
        account_id_hash: pool.accounts[0].account_id_hash.clone(),
        state: CapacityState::Available,
        source_observed_at_ms: Some(at),
        snapshot_at_ms: at,
        windows: CapacityWindows {
            primary: Some(CapacityWindow {
                limit_window_seconds: Some(18000),
                used_percent: Some(used),
                reset_at_ms: Some(NOW + 300_000),
            }),
            secondary: None,
        },
        additional_rate_limits: vec![],
    }
}
async fn original_actor<'a>(
    store: &'a Store,
    host: &'a Host,
    config: &'a AuthConfig,
) -> Result<(SourceNativeRenewalActor<'a, Store>, String), GatewayError> {
    let token = create(store, host).await?;
    let request = gateway_auth_contracts::types::AuthRequest {
        method: "POST".into(),
        url: "https://ai.ubq.fi/admin".into(),
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
        request_id: RequestId("original".into()),
        principal,
        key_id: None,
        route: "responses".into(),
        started_at_ms: NOW,
    };
    let actor = ActorService::new(store, host, config)
        .authenticate_source_admin_actor(&request, &context)
        .await?;
    Ok((actor, token))
}
#[test]
fn complete_source_manual_flow_charges_once_settles_after_revocation_and_promotes_once()
-> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let tokens = Tokens::default();
        let config = AuthConfig::default();
        let pool = pool(&store)?;
        let hash = &pool.accounts[0].admin_account_hash.0;
        let (actor, _) = original_actor(&store, &host, &config).await?;
        let service = SourceManualResetService::new(&store, &tokens, Some(&host), &host, &config);
        let SourceManualResetPreparation::NeedsUsage(target) =
            service.prepare_source_manual_reset(&actor, hash).await?
        else {
            return Err("expected fresh usage".into());
        };
        let action = service
            .begin_source_manual_reset(&target, &usage(&pool, NOW, 100.0))
            .await?;
        let ResetClaim::Submit { record, .. } = service.claim_source_manual_reset(&action).await?
        else {
            return Err("expected owned claim".into());
        };
        let submitted = service
            .prepare_source_manual_reset_submission(&action, &record)
            .await?;
        let renewed = service
            .renew_source_manual_reset_before_redeem(&action, &submitted)
            .await?;
        host.admin.store(false, Ordering::SeqCst);
        assert!(
            service
                .renew_source_manual_reset_before_redeem(&action, &renewed)
                .await
                .is_err()
        );
        let terminal = Terminal {
            store: &store,
            tokens: &tokens,
            host: &host,
        };
        let verified = terminal
            .record_reset_outcome(
                &renewed,
                ResetState::Verified,
                Some("source-terminal"),
                None,
                NOW,
            )
            .await?;
        assert!(
            service
                .promote_source_manual_reset(&action, &verified, &usage(&pool, NOW, 0.0))
                .await
                .is_err()
        );
        host.admin.store(true, Ordering::SeqCst);
        host.now.store(NOW + 10, Ordering::SeqCst);
        let result = service
            .promote_source_manual_reset(&action, &verified, &usage(&pool, NOW + 10, 0.0))
            .await?;
        assert_eq!(result.active_account_id_hash, *hash);
        assert!(
            service
                .promote_source_manual_reset(&action, &verified, &usage(&pool, NOW + 10, 0.0))
                .await
                .is_err()
        );
        let row = store.read(&keys::active()).await?.ok_or("active")?;
        let active: ActiveSelection = serde_json::from_value(row.json_value()?.clone())?;
        assert_eq!(
            active.transition_reason,
            Some(ActiveTransitionReason::ManualReset)
        );
        let day = gateway_authority_reset_records::utc_day(NOW)?;
        let row = store
            .read(&keys::reset_day(hash, &day))
            .await?
            .ok_or("budget")?;
        let budget: AccountDayBudget = serde_json::from_value(row.json_value()?.clone())?;
        assert_eq!(budget.submission_count, 1);
        Ok(())
    })
}
#[test]
fn app_row_deleted_at_actual_intent_cas_cannot_publish_intent_or_spend_budget() -> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let tokens = Tokens::default();
        let config = AuthConfig::default();
        let pool = pool(&store)?;
        let hash = &pool.accounts[0].admin_account_hash.0;
        let (actor, token) = original_actor(&store, &host, &config).await?;
        let service = SourceManualResetService::new(&store, &tokens, Some(&host), &host, &config);
        let SourceManualResetPreparation::NeedsUsage(target) =
            service.prepare_source_manual_reset(&actor, hash).await?
        else {
            return Err("target".into());
        };
        *store.1.borrow_mut() = Some(session_key(&token));
        assert!(
            service
                .begin_source_manual_reset(&target, &usage(&pool, NOW, 100.0))
                .await
                .is_err()
        );
        assert!(
            store
                .read(&gateway_authority_reset_records::manual::intent_key(hash))
                .await?
                .is_none()
        );
        assert!(
            store
                .read(&keys::reset_day(
                    hash,
                    &gateway_authority_reset_records::utc_day(NOW)?
                ))
                .await?
                .is_none()
        );
        Ok(())
    })
}
#[test]
fn known_unknown_remains_nonreplayable_after_restart_and_new_spend_age() -> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let tokens = Tokens::default();
        let config = AuthConfig::default();
        let pool = pool(&store)?;
        let hash = &pool.accounts[0].admin_account_hash.0;
        let (actor, _) = original_actor(&store, &host, &config).await?;
        let service = SourceManualResetService::new(&store, &tokens, Some(&host), &host, &config);
        let SourceManualResetPreparation::NeedsUsage(target) =
            service.prepare_source_manual_reset(&actor, hash).await?
        else {
            return Err("target".into());
        };
        let action = service
            .begin_source_manual_reset(&target, &usage(&pool, NOW, 100.0))
            .await?;
        let ResetClaim::Submit { record, .. } = service.claim_source_manual_reset(&action).await?
        else {
            return Err("claim".into());
        };
        let submitted = service
            .prepare_source_manual_reset_submission(&action, &record)
            .await?;
        let terminal = Terminal {
            store: &store,
            tokens: &tokens,
            host: &host,
        };
        terminal
            .record_reset_outcome(&submitted, ResetState::Unknown, None, Some("unknown"), NOW)
            .await?;
        host.now.store(NOW + 60_000, Ordering::SeqCst);
        let restarted = SourceManualResetService::new(&store, &tokens, Some(&host), &host, &config);
        assert!(matches!(
            restarted.claim_source_manual_reset(&action).await?,
            ResetClaim::InProgress(_)
        ));
        assert!(
            restarted
                .renew_source_manual_reset_before_redeem(&action, &submitted)
                .await
                .is_err()
        );
        Ok(())
    })
}
