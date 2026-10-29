use std::sync::atomic::Ordering;
mod inference_fixture;
mod support;
use gateway_auth::resolve::AuthService;
use gateway_auth_contracts::types::{AuthConfig, AuthRequest};
use gateway_auth_credentials::{
    native_credentials::parse_native_document,
    provider_credentials::{CodexNativeOwner, codex_source_pool_key, parse_codex_source_pool},
};
use gateway_auth_passport_contracts::passport::{PASSPORT_COOKIE, PassportConfig};
use gateway_auth_passport_sessions::sessions::PassportSessions;
use gateway_auth_primitives::crypto::base64url;
use gateway_authority_actor_currentness::app::ActorService;
use gateway_authority_keys as keys;
use gateway_authority_native_preparation::{
    SourceNativePreparationContext, SourceNativeRenewalService,
};
use gateway_authority_native_records::{owner::RegisteredRenewalOwner, rows};
use gateway_authority_paid_control_records::{
    PAID_CONTROL_CONFIG_VERSION, PaidControlConfig, PaidControlOwner, PaidSourceCheck,
};
use gateway_authority_routing_records::types::SubscriptionPool;
use gateway_core::{source_control::decode_source_control_record, state::AuthorityStore};
use gateway_kv_codec::value::ValueEncoding;
use gateway_local_journal::context::decode_journal_context;
use gateway_provider_policy::policy::HopFailure;
use gateway_routing_runtime::{
    session::RoutingSession,
    types::{AttemptFailure, FailureOrigin, TransportState},
};
use gateway_routing_runtime_test_support::{
    input::{NOW as RUNTIME_NOW, input},
    seed::seed_pool,
    store::Store as RuntimeStore,
};
use gateway_source::{
    js_string::JsString16, source_json::SourceJson, source_serialization as source,
};
use inference_fixture::NativeRefreshHost;
use std::{
    collections::BTreeMap,
    future::Future,
    task::{Context, Waker},
};
use support::{
    Cancel, Host, Store, TestResult, Tokens, actor, control_body, fixture, identity, run,
    signed_owner,
};
async fn inference_input(
    store: &RuntimeStore,
    host: &Host,
    id: &str,
) -> TestResult<(
    gateway_routing_runtime::types::RoutingInput,
    AuthRequest,
    gateway_auth_native_proof::native_auth::NativeTransportIdentity,
    AuthConfig,
)> {
    seed_pool(store);
    host.now.0.store(RUNTIME_NOW, Ordering::SeqCst);
    let native = identity()?;
    let raw = store
        .read(&codex_source_pool_key())
        .await?
        .ok_or("source")?;
    let mut pool = parse_codex_source_pool(raw.json_value()?)?.ok_or("pool")?;
    let account = &mut pool.accounts[0];
    let payload = serde_json::json!({"https://api.openai.com/auth":{
        "chatgpt_user_id":"native-user","chatgpt_account_id":account.account_id
    }});
    let access = format!("e30.{}.fixture", base64url(&serde_json::to_vec(&payload)?));
    let credentials = parse_native_document(&serde_json::json!({
        "auth_mode":"chatgptAuthTokens", "OPENAI_API_KEY":null,
        "tokens":{"account_id":account.account_id,"access_token":access,
            "id_token":access,"refresh_token":""}
    }))?;
    let generation_hash = credentials.generation_hash();
    account.access_token = credentials.access_token;
    account.refresh_token = credentials.refresh_token;
    account.native_owner = Some(CodexNativeOwner {
        codex_home: "/tmp/native-source".into(),
        generation_hash,
        native_refreshed_at: credentials.native_refreshed_at,
        external_user_id: credentials.external_user_id,
        managed_source: None,
    });
    let account_identity = account.identity();
    let source = pool.private_source_value();
    parse_codex_source_pool(&source)?.ok_or("external pool")?;
    store.seed(codex_source_pool_key(), source);
    let source_revision = store
        .read(&codex_source_pool_key())
        .await?
        .ok_or("new source")?
        .revision;
    let raw = store.read(&keys::pool()).await?.ok_or("normalized")?;
    let mut normalized: SubscriptionPool = serde_json::from_value(raw.json_value()?.clone())?;
    normalized.accounts[0].account_id_hash = account_identity.routing_account_hash;
    normalized.accounts[0].admin_account_hash = account_identity.administrative_account_hash;
    normalized.accounts[0].credential_version = account_identity.credential_version;
    normalized.pool_versionstamp.0 = source_revision;
    store.seed(keys::pool(), normalized);
    let runtime = gateway_schema::catalog_store::runtime_config_key();
    store.seed(runtime.clone(), serde_json::json!({}));
    let runtime_revision = store.read(&runtime).await?.ok_or("runtime")?.revision;
    store.seed(
        keys::paid_control_owner(),
        PaidControlOwner {
            v: 1,
            owner_id: native.owner_id().into(),
            owner_epoch: native.owner_epoch().into(),
            key_fingerprint: native.key_fingerprint().into(),
        },
    );
    store.seed(
        keys::paid_control_config(),
        PaidControlConfig {
            v: PAID_CONTROL_CONFIG_VERSION,
            owner_id: native.owner_id().into(),
            owner_epoch: native.owner_epoch().into(),
            generation: "configuration".into(),
            configuration_fingerprint: "fingerprint".into(),
            surplus_binding: None,
            openlux_discovery_binding: None,
            openlux_binding: None,
            source_checks: vec![PaidSourceCheck {
                key: runtime.0,
                expected_revision: Some(runtime_revision),
            }],
        },
    );
    let grant = SourceJson::parse(&format!(
        r#"{{"subject":"\ud800\u0000","clientId":"ai","audience":"ai","origin":"https://ai.ubq.fi","redirectUri":"https://ai.ubq.fi/auth/callback","createdAt":"10/27/2026","token":"uos_app_session_{}"}}"#,
        "A".repeat(43)
    ))?;
    let cookie = PassportSessions { store, host }
        .create(&grant, &JsString16::from_units(vec![0xd800, 0]), None)
        .await?;
    let request = AuthRequest {
        method: "POST".into(),
        url: "https://ai.ubq.fi/v1/responses".into(),
        headers: BTreeMap::from([("cookie".into(), format!("{PASSPORT_COOKIE}={cookie}"))]),
        ..Default::default()
    };
    let config = AuthConfig {
        passport: Some(PassportConfig::production()),
        ..Default::default()
    };
    let principal = AuthService::new(store, host, &config)
        .authenticate_client(&request)
        .await?;
    let mut input = input(id, "gpt-test", &[]);
    input.context.principal = principal.method.principal_id(
        principal
            .token
            .as_ref()
            .map(gateway_auth_primitives::crypto::Secret::expose),
    );
    input.context.key_id = None;
    input.principal = principal;
    Ok((input, request, native, config))
}
fn refusal() -> AttemptFailure {
    AttemptFailure {
        origin: FailureOrigin::Provider,
        status: Some(401),
        headers: BTreeMap::new(),
        body: vec![],
        body_complete: true,
        failure: HopFailure::Status(401),
        transport: TransportState::Started,
        semantic_committed: false,
        cancelled: false,
    }
}
#[test]
fn signed_source_claim_consumes_one_start_and_shares_original_generation_namespace() -> TestResult {
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
        let prep = SourceNativeRenewalService::new(
            &store,
            &tokens,
            Some(&host.now),
            &host,
            &config,
            SourceNativePreparationContext::configured(&id, &owner, &cancel),
        );
        let action = prep
            .prepare_source_live_mapped_renewal(&actor, "call")
            .await?;
        let service = gateway_authority_native_lifecycle::SourceNativeLifecycleService::new(
            &store,
            &tokens,
            Some(&host.now),
            &host,
            &config,
            gateway_authority_native_lifecycle::SourceNativeLifecycleContext::configured(
                &id, &owner, &cancel,
            ),
        );
        let body = control_body(action.action_ref(), "operation1")?;
        let proof = signed_owner(&id, &body, 1).await?;
        let mut claim = service
            .claim(&proof, action.action_ref(), "operation1", &body)
            .await?;
        use gateway_native_authority_wire::renewal::NativeRenewalMode;
        assert_eq!(claim.mode(), NativeRenewalMode::RenewOnce);
        assert_eq!(claim.fence(), action.fence());
        assert_eq!(
            service.start(&mut claim).await?,
            NativeRenewalMode::RenewOnce
        );
        assert_eq!(
            service.start(&mut claim).await?,
            NativeRenewalMode::ReconcileOnly
        );
        let body = control_body(action.action_ref(), "operation1")?;
        let proof = signed_owner(&id, &body, 2).await?;
        let repeated = service
            .claim(&proof, action.action_ref(), "operation1", &body)
            .await?;
        assert_eq!(repeated.mode(), NativeRenewalMode::ReconcileOnly);
        assert!(repeated.start_permit().is_none());
        let key = rows::generation_key_parts(
            &action.fence().native_owner_id,
            &action.fence().account_id_hash,
            &action.fence().source_native_generation,
        )?;
        let row = store.read(&key).await?.ok_or("generation")?;
        let generation: rows::GenerationEffect = serde_json::from_value(row.json_value()?.clone())?;
        assert_eq!(generation.v, 1);
        assert_eq!(generation.operation_id, "operation1");
        assert_eq!(generation.claim_ref, claim.claim_ref());
        Ok(())
    })
}

#[test]
fn real_v3_writer_and_owned_refresh_await_prepare_lossless_source_inference_then_refuse_retired_attempt()
-> TestResult {
    run(async {
        let store = RuntimeStore::new();
        let auth = Host::default();
        let (input, request, native, config) =
            inference_input(&store, &auth, "native-source-inference").await?;
        let original = input.context.clone();
        assert!(original.principal.try_scalar().is_err());
        let source_actor = ActorService::new(&store, &auth, &config)
            .authenticate_source_client_actor(&request, &original)
            .await?;
        let (host, pause) = NativeRefreshHost::new(&store);
        let session =
            RoutingSession::begin(input, store.clone(), host, store.clone(), store.clone())
                .await
                .map_err(|error| *error.error)?;
        let paid_request = gateway_authority_paid_records::PaidRequest {
            key_id: gateway_contracts::identity::ApiKeyId("paid-target".into()),
            request_id: original.request_id.clone(),
            attempt_id: gateway_contracts::identity::PaidAttemptId("paid-attempt".into()),
            provider: gateway_contracts::provider::PaidProvider::Surplus,
            model: gateway_contracts::model::ModelId::from("gpt-test"),
            route: original.route.clone(),
            path: "/v1/responses".into(),
            stream: false,
            reasoning: None,
            created_at_ms: original.started_at_ms,
        };
        let admitted_record = store
            .read(&rows::session_key(&original.request_id.0))
            .await?
            .ok_or("admitted journal")?;
        let paid_checks = gateway_authority_paid_evidence::capture_original(
            &store,
            &paid_request,
            &original,
            None,
            RUNTIME_NOW,
        )
        .await?;
        assert!(paid_checks.iter().any(|check| {
            check.key == rows::session_key(&original.request_id.0)
                && check.expected_revision.as_deref() == Some(admitted_record.revision.as_str())
        }));
        let attempt = session
            .next_attempt()
            .await
            .map_err(|error| *error.error)?
            .ok_or("attempt")?;
        session
            .before_dispatch(&attempt)
            .await
            .map_err(|error| *error.error)?;
        session
            .transport_started(&attempt)
            .await
            .map_err(|error| *error.error)?;
        let mut refresh = Box::pin(session.refusal(&attempt, refusal()));
        let mut cx = Context::from_waker(Waker::noop());
        assert!(refresh.as_mut().poll(&mut cx).is_pending());
        let raw = store
            .read(&rows::session_key(&original.request_id.0))
            .await?
            .ok_or("journal")?;
        assert_eq!(raw.value_encoding, ValueEncoding::ProfileV2);
        gateway_routing_runtime::custody::validate_journal_row(
            &rows::session_key(&original.request_id.0),
            &raw,
        )?;
        let paid_error = gateway_authority_paid_evidence::capture_original(
            &store,
            &paid_request,
            &original,
            None,
            RUNTIME_NOW,
        )
        .await
        .err()
        .ok_or("active attempt accepted by prospective paid capture")?;
        assert_eq!(paid_error.status, 503);
        assert_eq!(
            paid_error.envelope.error.code.as_deref(),
            Some("paid_control_unavailable")
        );
        assert_eq!(
            paid_error.envelope.error.message,
            "Original request ownership is missing, expired, finalizing or changed."
        );
        let journal = decode_source_control_record(&raw)?;
        assert_eq!(decode_journal_context(&journal)?, original);
        assert_eq!(source::member(&journal, "v").ok_or("v")?.as_str(), "3");
        let refreshed = source::member(&journal, "refreshed_versions").ok_or("refreshed")?;
        assert!(
            !source::array(&refreshed)
                .ok_or("refreshed array")?
                .is_empty()
        );
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(false);
        let service = SourceNativeRenewalService::new(
            &store,
            &store,
            Some(&store),
            &auth,
            &config,
            SourceNativePreparationContext::configured(&native, &owner, &cancel),
        );
        let action = service
            .prepare_source_inference_renewal(&source_actor)
            .await?;
        assert_eq!(action.fence().v, 3);
        assert_eq!(action.fence().principal, *original.principal.as_source());
        let gateway_native_authority_wire::context::NativeRenewalContext::Inference {
            attempt_id,
            model,
            ..
        } = &action.fence().context
        else {
            return Err("wrong inference context".into());
        };
        assert_eq!(attempt_id, &attempt.id);
        assert_eq!(model, &gateway_contracts::model::ModelId::from("gpt-test"));
        let row = store
            .read(&rows::key("action", action.action_ref()))
            .await?
            .ok_or("action")?;
        let row=gateway_authority_native_records::source_rows::SourceRenewalActionRowV3::from_source_json(&decode_source_control_record(&row)?)?;
        assert_eq!(row.context, original);
        pause.release();
        refresh.await.map_err(|error| *error.error)?;
        assert!(
            service
                .prepare_source_inference_renewal(&source_actor)
                .await
                .is_err()
        );
        Ok(())
    })
}
