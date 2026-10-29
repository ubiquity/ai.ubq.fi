//! Finite test-only callback composition; every actor/action/claim/terminal is minted by real services.
use gateway_auth_contracts::types::{AuthConfig, AuthRequest};
use gateway_auth_credentials::provider_credentials::{
    CodexNativeOwner, MANAGED_NATIVE_SOURCE_VERSION, ManagedNativeSource,
    codex_credential_identity, codex_source_pool_key, parse_codex_source_pool,
};
use gateway_auth_native_contracts::transport::{NativeAuthHeaders, NativeCapability};
use gateway_auth_native_proof::native_auth::{NativeAuthenticator, NativeTransportIdentity};
use gateway_auth_native_protocol::native_replay::StoreNonceAuthority;
use gateway_auth_native_wire::{
    control::{NativeControlEnvelope, NativeControlRequest},
    native_read::{NATIVE_CONTROL_PATH, NATIVE_PUBLICATION_PATH},
};
use gateway_auth_passport_sessions::sessions::{PassportSessions, session_key};
use gateway_auth_primitives::crypto::{Secret, base64url, decode_base64url};
use gateway_authority::{native_control::actor::authenticate_client_actor, store::Authority};
use gateway_authority_actor_currentness::app::ActorService;
use gateway_authority_contracts::AuthorityClock;
use gateway_authority_keys as keys;
use gateway_authority_native_records::{live::mapping_key, owner::RegisteredRenewalOwner};
use gateway_authority_routing_records::types::SubscriptionPool;
use gateway_contracts::identity::{PrincipalId, RequestContext, RequestId};
use gateway_core::{
    source_control::encode_source_control_record,
    state::{AtomicTransition, AuthorityStore, RecordMutation, StoredRecord},
};
use gateway_kv_codec::key::RecordKey;
use gateway_publication_transport::transport::PublicationReceiver;
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use gateway_worker_authority_binding::authority::RemoteAuthorityStore;
use std::collections::{BTreeMap, BTreeSet};
use worker::{Context, Env, Request};
fn invalid() -> GatewayError {
    gateway_worker_fixture_setup::invalid()
}
use gateway_worker_fixture_passport::{Cancel, Clock, Tokens};

fn config(env: &Env) -> AuthConfig {
    let names = [
        "UOS_AI_TOKEN",
        "DENO_DEPLOY_TOKEN",
        "DENO_TIMELINE",
        "DENO_DEPLOY",
        "DENO_DEPLOYMENT_ID",
        "DENO_REGION",
        "UOS_AI_KERNEL_PUBLIC_KEY",
        "DENO_DEPLOY_APP_SLUG",
        "DENO_DEPLOY_ORG_SLUG",
        "UOS_AI_PASSPORT_CLIENT_ID",
        "UOS_AI_PASSPORT_AUDIENCE",
        "UOS_AI_PASSPORT_ORIGIN",
        "UOS_AI_PASSPORT_REDIRECT_URI",
    ];
    let values = names
        .into_iter()
        .filter_map(|name| {
            env.secret(name)
                .map(|v| v.to_string())
                .or_else(|_| env.var(name).map(|v| v.to_string()))
                .ok()
                .map(|v| (name.to_owned(), v))
        })
        .collect();
    let mut cfg = AuthConfig::from_source_values(&values, true);
    cfg.candidate_passkeys = true;
    cfg.extra_relay_origins
        .insert("https://rust-candidate.ai.ubq.fi".into());
    cfg
}
fn identity() -> Result<NativeTransportIdentity, GatewayError> {
    NativeTransportIdentity::new(
        "fixture-matrix-key".into(),
        "fixture-matrix-owner".into(),
        "fixture-matrix-epoch".into(),
        [31; 32],
        BTreeSet::from([
            NativeCapability::ConfigurationPublish,
            NativeCapability::CodexAuthImport,
            NativeCapability::OwnerRenewal,
            NativeCapability::AccessPublish,
        ]),
    )
}
async fn put(
    store: &RemoteAuthorityStore,
    key: RecordKey,
    record: StoredRecord,
) -> Result<(), GatewayError> {
    if store
        .commit(AtomicTransition {
            checks: Vec::new(),
            mutations: vec![RecordMutation::Put { key, record }],
        })
        .await?
        != gateway_core::state::CommitOutcome::Committed
    {
        return Err(invalid());
    }
    Ok(())
}
async fn native_proof(
    id: &NativeTransportIdentity,
    store: &RemoteAuthorityStore,
    path: &str,
    body: &[u8],
    n: u8,
) -> Result<gateway_auth_native_proof::native_auth::AuthenticatedNativeOwner, GatewayError> {
    let now = Clock.now_ms()?;
    let nonce = base64url(&[n; 32]);
    let mac = id.sign(path, now, &nonce, body)?;
    NativeAuthenticator::new(id, &StoreNonceAuthority::new(store))
        .authenticate(
            path,
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
async fn managed_input(store: &RemoteAuthorityStore) -> Result<(), GatewayError> {
    let raw = store
        .read(&codex_source_pool_key())
        .await?
        .ok_or_else(invalid)?;
    let mut source = parse_codex_source_pool(raw.json_value()?)?.ok_or_else(invalid)?;
    let account = &mut source.accounts[0];
    account.refresh_token = Secret::new(String::new());
    let id = account.identity();
    account.native_owner = Some(CodexNativeOwner {
        codex_home: "/tmp/worker-import-synthetic-owner".into(),
        generation_hash: id.native_generation_hash,
        native_refreshed_at: None,
        external_user_id: None,
        managed_source: Some(ManagedNativeSource {
            v: MANAGED_NATIVE_SOURCE_VERSION,
            generation_hash: "a".repeat(64),
        }),
    });
    let row = store.read(&keys::pool()).await?.ok_or_else(invalid)?;
    let mut normalized: SubscriptionPool =
        serde_json::from_value(row.json_value()?.clone()).map_err(|_| invalid())?;
    normalized.pool_versionstamp.0 = "callback-source-input".into();
    normalized.accounts[0].credential_version = id.credential_version;
    put(
        store,
        codex_source_pool_key(),
        StoredRecord::json("callback-source-input", source.private_source_value(), None),
    )
    .await?;
    put(
        store,
        keys::pool(),
        StoredRecord::json(
            "callback-normalized-input",
            serde_json::to_value(normalized).map_err(|_| invalid())?,
            None,
        ),
    )
    .await
}
async fn setup(
    env: &Env,
    receiver: &PublicationReceiver,
    id: &NativeTransportIdentity,
    scenario: &str,
) -> Result<String, GatewayError> {
    let inputs = super::inputs()?;
    let _ = super::native_import::initialize(env, receiver, id, &inputs).await?;
    let store = RemoteAuthorityStore::from_env(env)?;
    let meta_key = gateway_kv_codec::tuple_key::TupleKey::from_scalar_parts(&[
        gateway_kv_codec::tuple_key::ScalarKeyPart::String("uos_ai"),
        gateway_kv_codec::tuple_key::ScalarKeyPart::String("fixture_native_callback"),
        gateway_kv_codec::tuple_key::ScalarKeyPart::String(scenario),
    ])
    .to_record_key();
    if let Some(row) = store.read(&meta_key).await? {
        return Ok(base64url(
            &serde_json::to_vec(row.json_value()?).map_err(|_| invalid())?,
        ));
    }
    managed_input(&store).await?;
    let cfg = config(env);
    let host = gateway_worker_fixture_passport::Host::new(env.clone());
    let authority = Authority::with_clock(store.clone(), Tokens, Clock);
    let owner = RegisteredRenewalOwner::configured(
        "fixture-import-native-owner".into(),
        "/tmp/worker-import-synthetic-owner".into(),
    )?;
    let cancel = Cancel;
    let now = Clock.now_ms()?;
    let pool = store
        .read(&codex_source_pool_key())
        .await?
        .ok_or_else(invalid)?;
    let pool = parse_codex_source_pool(pool.json_value()?)?.ok_or_else(invalid)?;
    let account = &pool.accounts[0].account_id;
    let operation = "fixture-callback-operation";
    let is_legacy = scenario == "native-callback-legacy";
    let (reference, claim_ref) = if is_legacy {
        let original = AuthRequest {
            method: "POST".into(),
            url: "https://ai.ubq.fi/v1/responses".into(),
            headers: BTreeMap::from([(
                "authorization".into(),
                "Bearer uos_ai_session_SYNTHETIC_WORKER_IMPORT_TOKEN".into(),
            )]),
            ..Default::default()
        };
        let context = RequestContext {
            request_id: RequestId("fixture-callback-legacy-original".into()),
            principal: PrincipalId::from_scalar("passkey-user:fixture-import-user".into()),
            key_id: None,
            route: "live.sideband".into(),
            started_at_ms: now,
        };
        let actor = authenticate_client_actor(&authority, &host, &cfg, &original, &context).await?;
        let mapping = SourceJson::from_serializable(
            &serde_json::json!({"account_id":account,"principal_id":"passkey-user:fixture-import-user"}),
        )?;
        put(
            &store,
            mapping_key("callback-legacy", false),
            encode_source_control_record("callback-legacy-map", &mapping, None)?,
        )
        .await?;
        let service =
            gateway_authority::native_control::receiver::NativeRenewalAuthority::configured(
                &authority, id, &owner, &cfg, &cancel,
            );
        let action = service
            .prepare_live_mapped_renewal(&actor, "callback-legacy")
            .await?;
        (action.action_ref().to_owned(), None)
    } else {
        let subject = JsString16::from_units(vec![0xd800, 0]);
        let grant = SourceJson::parse(&format!(
            r#"{{"subject":"\ud800\u0000","clientId":"ai","audience":"ai","origin":"https://ai.ubq.fi","redirectUri":"https://ai.ubq.fi/auth/callback","createdAt":"10/27/2026","token":"uos_app_session_{}"}}"#,
            "A".repeat(43)
        ))?;
        let token = PassportSessions {
            store: authority.store(),
            host: &host,
        }
        .create(&grant, &subject, None)
        .await?;
        let principal = PrincipalId::from_source(JsString16::from_units(
            "passport-user:".encode_utf16().chain([0xd800, 0]).collect(),
        ));
        let original = AuthRequest {
            method: "POST".into(),
            url: "https://ai.ubq.fi/v1/responses".into(),
            headers: BTreeMap::from([(
                "cookie".into(),
                format!("__Host-uos_ai_passport_session={token}"),
            )]),
            ..Default::default()
        };
        let context = RequestContext {
            request_id: RequestId("fixture-callback-source-original".into()),
            principal: principal.clone(),
            key_id: None,
            route: "live.sideband".into(),
            started_at_ms: now,
        };
        let actor = ActorService::new(authority.store(), &host, &cfg)
            .authenticate_source_client_actor(&original, &context)
            .await?;
        let mapping = SourceJson::parse(&format!(
            r#"{{"account_id":{},"principal_id":"passport-user:\ud800\u0000"}}"#,
            serde_json::to_string(account).map_err(|_| invalid())?
        ))?;
        put(
            &store,
            mapping_key("callback-source", false),
            encode_source_control_record("callback-source-map", &mapping, None)?,
        )
        .await?;
        let prep = authority.source_native_renewals(&host, &cfg, id, &owner, &cancel);
        let action = prep
            .prepare_source_live_mapped_renewal(&actor, "callback-source")
            .await?;
        if scenario == "native-callback-source" {
            (action.action_ref().to_owned(), None)
        } else {
            let control = serde_json::to_vec(&NativeControlEnvelope {
                v: 1,
                operation_id: operation.into(),
                request: NativeControlRequest::CodexRenewAccess {
                    action_ref: action.action_ref().into(),
                },
            })
            .map_err(|_| invalid())?;
            let service = authority.source_native_lifecycle(&host, &cfg, id, &owner, &cancel);
            let mut claim = service
                .claim(
                    &native_proof(id, &store, NATIVE_CONTROL_PATH, &control, 220).await?,
                    action.action_ref(),
                    operation,
                    &control,
                )
                .await?;
            service.start(&mut claim).await?;
            if scenario == "native-callback-published" {
                let access = format!(
                    "{}.{}.synthetic",
                    base64url(b"{}"),
                    base64url(format!(r#"{{"exp":{}}}"#, now / 1000 + 600).as_bytes())
                );
                let version = codex_credential_identity(
                    account,
                    &Secret::new(access.clone()),
                    &Secret::new(String::new()),
                );
                let fence = action.fence();
                let body=serde_json::to_vec(&serde_json::json!({"operation":"managed_access_publish","v":2,"action_ref":action.action_ref(),"operation_id":operation,"native_owner_id":fence.native_owner_id,"owner_revision":fence.owner_revision,"source_revision":fence.source_revision,"normalized_revision":fence.normalized_revision,"publication_revision":"fixture-callback-publication","account_id_hash":fence.account_id_hash,"previous_native_source_generation":fence.source_native_generation,"native_source_generation":"b".repeat(64),"access_generation":version.native_generation_hash,"document":{"account_id":account,"access_token":access,"native_refreshed_at":null}})).map_err(|_|invalid())?;
                authority
                    .source_native_managed(&host, &cfg, id, &owner, &cancel)
                    .publish_managed(
                        &native_proof(id, &store, NATIVE_PUBLICATION_PATH, &body, 221).await?,
                        &body,
                    )
                    .await?;
            } else {
                let keys = [session_key(&token), codex_source_pool_key(), keys::pool()];
                let mutations = keys
                    .into_iter()
                    .map(|key| RecordMutation::Delete { key })
                    .collect();
                store
                    .commit(AtomicTransition {
                        checks: Vec::new(),
                        mutations,
                    })
                    .await?;
            }
            (
                action.action_ref().to_owned(),
                Some(claim.claim_ref().to_owned()),
            )
        }
    };
    let publication_access = format!(
        "{}.{}.synthetic",
        base64url(b"{}"),
        base64url(format!(r#"{{"exp":{}}}"#, now / 1000 + 600).as_bytes())
    );
    let publication_identity = codex_credential_identity(
        account,
        &Secret::new(publication_access.clone()),
        &Secret::new(String::new()),
    );
    let original = serde_json::to_vec(&NativeControlEnvelope {
        v: 1,
        operation_id: operation.into(),
        request: NativeControlRequest::CodexRenewAccess {
            action_ref: reference.clone(),
        },
    })
    .map_err(|_| invalid())?;
    let original_nonce = base64url(&[223; 32]);
    let original_time = Clock.now_ms()?;
    let original_mac = id.sign(
        NATIVE_CONTROL_PATH,
        original_time,
        &original_nonce,
        &original,
    )?;
    let meta = serde_json::json!({"original_control_body_base64url":base64url(&original),"original_auth_headers":{"version":"1","key_id":id.key_id(),"time_ms":original_time.to_string(),"nonce":original_nonce,"mac":original_mac},"action_ref":reference,"operation_id":operation,"claim_ref":claim_ref,"v":if is_legacy{1}else{2},"publication_revision":"fixture-callback-publication","account_id":account,"publication_access":publication_access,"publication_access_generation":publication_identity.native_generation_hash});
    let encoded = base64url(&serde_json::to_vec(&meta).map_err(|_| invalid())?);
    put(
        &store,
        meta_key,
        StoredRecord::json("fixture-callback-metadata", meta, None),
    )
    .await?;
    Ok(encoded)
}
pub(super) async fn fetch(
    request: Request,
    env: Env,
    context: Context,
) -> worker::Result<gateway_worker_web::response::GatewayResponse> {
    let scenario = env.var("UOS_MATRIX_SCENARIO")?.to_string();
    if ![
        "native-callback-legacy",
        "native-callback-source",
        "native-callback-published",
        "native-callback-unknown",
    ]
    .contains(&scenario.as_str())
    {
        return Err(worker::Error::RustError(
            "invalid callback fixture scenario".into(),
        ));
    }
    let id = identity().map_err(|e| worker::Error::RustError(e.to_string()))?;
    let inputs = super::inputs().map_err(|e| worker::Error::RustError(e.to_string()))?;
    let receiver = PublicationReceiver::configured(id.clone(), inputs, Vec::new(), || {
        gateway_worker_storage::store::now_ms().unwrap_or(i64::MAX)
    })
    .map_err(|e| worker::Error::RustError(e.to_string()))?;
    let metadata = setup(&env, &receiver, &id, &scenario)
        .await
        .map_err(|e| worker::Error::RustError(e.to_string()))?;
    let bindings =
        gateway_worker::native_host::NativeBindings::configured(env.service("ASSETS")?, id)
            .with_renewal_owners(
                super::native_import::owners()
                    .map_err(|e| worker::Error::RustError(e.to_string()))?,
            );
    let mut response =
        gateway_worker::fetch_with_bindings(request, env, context, Some(&receiver), bindings)
            .await?;
    response
        .headers_mut()
        .set("x-uos-fixture-callback-metadata", &metadata)?;
    Ok(response)
}
