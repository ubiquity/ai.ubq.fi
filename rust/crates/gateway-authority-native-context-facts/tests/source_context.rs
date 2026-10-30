//! Actual C source phase plus fixed native context reader; no native effects/network/FS.
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
use gateway_auth_passport_sessions::sessions::{PassportSessions, session_key};
use gateway_auth_primitives::crypto::Secret;
use gateway_authority_actor_currentness::app::ActorService;
use gateway_authority_actor_records::scope::NativeActorScope;
use gateway_authority_contracts::{AuthorityClock, NativeRenewalCancellation};
use gateway_authority_keys as keys;
use gateway_authority_native_context_facts::SourceNativeContextFactsReader;
use gateway_authority_native_records::{live::mapping_key, owner::RegisteredRenewalOwner};
use gateway_authority_paid_control_records::{
    PAID_CONTROL_CONFIG_VERSION, PaidControlConfig, PaidControlOwner, PaidSourceCheck,
};
use gateway_authority_routing_records::types::{PoolAccount, SubscriptionPool};
use gateway_contracts::identity::{PoolVersionstamp, PrincipalId, RequestContext, RequestId};
use gateway_core::source_control::encode_source_control_record;
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, StoredRecord,
};
use gateway_kv_codec::key::RecordKey;
use gateway_native_authority_wire::context::NativeRenewalContext;
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
        let AuthUpstreamRequest::PassportSession { app_session_token, .. } = request else {
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
struct JumpClock;
impl AuthorityClock for JumpClock {
    fn now_ms(&self) -> Result<i64, GatewayError> {
        Ok(NOW + 2)
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
async fn phase<'a>(
    store: &'a Store,
    host: &'a Host,
    config: &'a AuthConfig,
) -> Result<
    (
        gateway_authority_actor_currentness::source_actor::SourceActorPreparationData,
        String,
    ),
    GatewayError,
> {
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
    let service = ActorService::new(store, host, config);
    let actor = service
        .authenticate_source_client_actor(&request, &context)
        .await?;
    Ok((
        service
            .prepare_source_phase(&actor, NativeActorScope::Client, NOW)
            .await?,
        token,
    ))
}
fn context() -> NativeRenewalContext {
    NativeRenewalContext::LiveMapped {
        call_id: "call".into(),
        mapping_revision: "mapping".into(),
    }
}
#[test]
fn live_mapped_context_retains_lone_principal_profile_and_all_fixed_revision_observations()
-> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let id = identity()?;
        fixture(&store, &id, None)?;
        let (phase, _) = phase(&store, &host, &config).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let data = SourceNativeContextFactsReader::new(
            &store,
            &id,
            &owner,
            Some(&host),
            &Cancel(false),
            NOW,
        )?
        .read_context(
            phase.actor_data(),
            phase.context(),
            phase.current_data(),
            &context(),
        )
        .await?;
        assert_eq!(data.fence().v, 3);
        assert_eq!(
            &data.fence().principal,
            phase.context().principal.as_source()
        );
        assert!(
            data.revision_checks()
                .iter()
                .any(|check| check.key == mapping_key("call", false)
                    && check.expected_revision.as_deref() == Some("mapping"))
        );
        assert!(
            data.revision_checks()
                .iter()
                .any(|check| check.key == codex_source_pool_key())
        );
        Ok(())
    })
}
#[test]
fn local_app_row_changed_after_actual_C_phase_refuses_native_observation() -> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let id = identity()?;
        fixture(&store, &id, None)?;
        let (phase, token) = phase(&store, &host, &config).await?;
        store.0.borrow_mut().remove(&session_key(&token).0);
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        assert!(
            SourceNativeContextFactsReader::new(
                &store,
                &id,
                &owner,
                Some(&host),
                &Cancel(false),
                NOW
            )?
            .read_context(
                phase.actor_data(),
                phase.context(),
                phase.current_data(),
                &context()
            )
            .await
            .is_err()
        );
        Ok(())
    })
}
#[test]
fn mapping_expiry_crossing_real_clock_sample_refuses_even_with_same_revision() -> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let id = identity()?;
        fixture(&store, &id, Some(NOW + 1))?;
        let (phase, _) = phase(&store, &host, &config).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        assert!(
            SourceNativeContextFactsReader::new(
                &store,
                &id,
                &owner,
                Some(&JumpClock),
                &Cancel(false),
                NOW
            )?
            .read_context(
                phase.actor_data(),
                phase.context(),
                phase.current_data(),
                &context()
            )
            .await
            .is_err()
        );
        Ok(())
    })
}
#[test]
fn constructor_cancellation_keeps_original_499_and_never_mints_a_native_effect() -> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let id = identity()?;
        fixture(&store, &id, None)?;
        let (phase, _) = phase(&store, &host, &config).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let error = SourceNativeContextFactsReader::new(
            &store,
            &id,
            &owner,
            Some(&host),
            &Cancel(true),
            NOW,
        )?
        .read_context(
            phase.actor_data(),
            phase.context(),
            phase.current_data(),
            &context(),
        )
        .await
        .err()
        .ok_or("cancel accepted")?;
        assert_eq!(error.status, 499);
        assert_eq!(
            error.envelope.error.code.as_deref(),
            Some("request_cancelled")
        );
        Ok(())
    })
}

fn known_managed_publication(
    store: &Store,
) -> Result<gateway_authority_native_records::rows::TerminalPublication, GatewayError> {
    let header = gateway_auth_primitives::crypto::base64url(b"{}");
    let payload = gateway_auth_primitives::crypto::base64url(
        serde_json::to_vec(&serde_json::json!({"exp":NOW/1000+600}))
            .map_err(|_| unavailable())?
            .as_slice(),
    );
    let mut account = CodexSourceAccount {
        account_id: "account".into(),
        access_token: Secret::new(format!("{header}.{payload}.synthetic")),
        refresh_token: Secret::new(String::new()),
        updated_at_ms: NOW as f64,
        native_owner: None,
    };
    let identity = account.identity();
    let physical = "c".repeat(64);
    account.native_owner = Some(CodexNativeOwner {
        codex_home: "/tmp/native-source".into(),
        generation_hash: identity.native_generation_hash.clone(),
        native_refreshed_at: None,
        external_user_id: None,
        managed_source: Some(
            gateway_auth_credentials::provider_credentials::ManagedNativeSource {
                v: gateway_auth_credentials::provider_credentials::MANAGED_NATIVE_SOURCE_VERSION,
                generation_hash: physical.clone(),
            },
        ),
    });
    let pool = SubscriptionPool {
        v: 1,
        pool_versionstamp: PoolVersionstamp("publication".into()),
        accounts: vec![PoolAccount {
            account_id_hash: identity.routing_account_hash,
            admin_account_hash: identity.administrative_account_hash,
            credential_version: identity.credential_version.clone(),
            enabled: true,
        }],
    };
    seed(
        store,
        codex_source_pool_key(),
        "publication",
        CodexSourcePool {
            accounts: vec![account],
            updated_at_ms: NOW as f64,
        }
        .private_source_value(),
    )?;
    seed(store, keys::pool(), "publication", pool)?;
    Ok(
        gateway_authority_native_records::rows::TerminalPublication {
            publication_revision: "publication".into(),
            source_revision: "publication".into(),
            normalized_revision: "publication".into(),
            native_source_generation: physical,
            access_generation: identity.native_generation_hash,
            credential_version: identity.credential_version.0,
            publication_request_digest: Some("request-digest".into()),
        },
    )
}

#[test]
fn known_terminal_checks_original_control_and_actual_new_managed_generation() -> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let id = identity()?;
        fixture(&store, &id, None)?;
        let (phase, _) = phase(&store, &host, &config).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let before = SourceNativeContextFactsReader::new(
            &store,
            &id,
            &owner,
            Some(&host),
            &Cancel(false),
            NOW,
        )?
        .read_context(
            phase.actor_data(),
            phase.context(),
            phase.current_data(),
            &context(),
        )
        .await?;
        let action = gateway_authority_native_records::source_rows::SourceRenewalActionRowV3 {
            v: 3,
            action_ref: "action".into(),
            actor: phase.actor_data().clone(),
            context: phase.context().clone(),
            created_at_ms: NOW,
            deadline_ms: NOW + 45_000,
            fence: before.fence().clone(),
            checks_digest: before.checks_digest().into(),
            control_checks_digest: before.control_checks_digest().into(),
            account_digests: before.account_digests().to_vec(),
            operation_id: Some("operation".into()),
        };
        let terminal = known_managed_publication(&store)?;
        let current = ActorService::new(&store, &host, &config)
            .check_stored_source_phase(
                &action.actor,
                &action.context,
                NativeActorScope::Client,
                NOW,
            )
            .await?;
        let control = SourceNativeContextFactsReader::new(
            &store,
            &id,
            &owner,
            Some(&host),
            &Cancel(false),
            NOW,
        )?
        .read_control(&action.actor, &action.context, &current, &context())
        .await?;
        assert_eq!(control.control_digest(), action.control_checks_digest);
        let data = SourceNativeContextFactsReader::new(
            &store,
            &id,
            &owner,
            Some(&host),
            &Cancel(false),
            NOW,
        )?
        .read_terminal(&action, &current, &terminal)
        .await?;
        assert_eq!(data.fence().source_revision, "publication");
        assert_eq!(
            data.fence().source_native_generation,
            terminal.native_source_generation
        );
        assert_eq!(data.fence().access_generation, terminal.access_generation);
        assert_ne!(
            data.fence().credential_version,
            action.fence.credential_version
        );
        assert_eq!(data.fence().principal, action.fence.principal);
        Ok(())
    })
}

#[test]
fn terminal_data_with_wrong_physical_generation_never_verifies_as_current() -> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let id = identity()?;
        fixture(&store, &id, None)?;
        let (phase, _) = phase(&store, &host, &config).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let before = SourceNativeContextFactsReader::new(
            &store,
            &id,
            &owner,
            Some(&host),
            &Cancel(false),
            NOW,
        )?
        .read_context(
            phase.actor_data(),
            phase.context(),
            phase.current_data(),
            &context(),
        )
        .await?;
        let action = gateway_authority_native_records::source_rows::SourceRenewalActionRowV3 {
            v: 3,
            action_ref: "action".into(),
            actor: phase.actor_data().clone(),
            context: phase.context().clone(),
            created_at_ms: NOW,
            deadline_ms: NOW + 45_000,
            fence: before.fence().clone(),
            checks_digest: before.checks_digest().into(),
            control_checks_digest: before.control_checks_digest().into(),
            account_digests: before.account_digests().to_vec(),
            operation_id: Some("operation".into()),
        };
        let mut terminal = known_managed_publication(&store)?;
        terminal.native_source_generation = "d".repeat(64);
        let current = ActorService::new(&store, &host, &config)
            .check_stored_source_phase(
                &action.actor,
                &action.context,
                NativeActorScope::Client,
                NOW,
            )
            .await?;
        assert!(
            SourceNativeContextFactsReader::new(
                &store,
                &id,
                &owner,
                Some(&host),
                &Cancel(false),
                NOW
            )?
            .read_terminal(&action, &current, &terminal)
            .await
            .is_err()
        );
        Ok(())
    })
}
