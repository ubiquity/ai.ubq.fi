//! Complete fresh external four-context test-only served fixture; no production capability API.
mod cache;
mod inference;
mod routing;
use gateway_auth_contracts::types::{AuthConfig, AuthRequest};
use gateway_auth_credentials::provider_credentials::{
    CodexNativeOwner, codex_source_pool_key, parse_codex_source_pool,
};
use gateway_auth_native_contracts::transport::{NativeAuthHeaders, NativeCapability};
use gateway_auth_native_proof::native_auth::{NativeAuthenticator, NativeTransportIdentity};
use gateway_auth_native_protocol::native_replay::StoreNonceAuthority;
use gateway_auth_native_wire::{
    control::{NativeControlEnvelope, NativeControlRequest},
    native_read::NATIVE_CONTROL_PATH,
};
use gateway_auth_passport_sessions::sessions::PassportSessions;
use gateway_auth_primitives::crypto::base64url;
use gateway_authority::{native_control::owners::RegisteredRenewalOwners, store::Authority};
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

use gateway_authority_contracts::TokenSource;
use gateway_worker_fixture_passport::{Cancel, Clock, Host};
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

#[derive(Clone, Copy)]
struct TokenClock;
impl AuthorityClock for TokenClock {
    fn now_ms(&self) -> Result<i64, GatewayError> {
        gateway_worker_storage::store::now_ms()
    }
}
impl gateway_authority_contracts::TokenSource for TokenClock {
    fn next_token(&self) -> Result<String, GatewayError> {
        gateway_worker_platform::random::opaque_token()
    }
}
struct Case {
    authority: Authority<RemoteAuthorityStore, TokenClock>,
    host: Host,
    config: AuthConfig,
    identity: NativeTransportIdentity,
    owner: RegisteredRenewalOwner,
    original: AuthRequest,
    context: RequestContext,
}
fn meta_key(kind: &str) -> RecordKey {
    gateway_kv_codec::tuple_key::TupleKey::from_scalar_parts(&[
        gateway_kv_codec::tuple_key::ScalarKeyPart::String("uos_ai"),
        gateway_kv_codec::tuple_key::ScalarKeyPart::String("fixture_source_external"),
        gateway_kv_codec::tuple_key::ScalarKeyPart::String(kind),
    ])
    .to_record_key()
}
fn input() -> Result<serde_json::Value, GatewayError> {
    serde_json::from_str(include_str!(
        "../../gateway-authority/tests/fixtures/cache-experiment-inventory-source-v2.json"
    ))
    .map_err(|_| invalid())
}
fn native(
    account: &str,
    now: i64,
    offset: i64,
) -> Result<gateway_auth_credentials::native_credentials::NativeCredentials, GatewayError> {
    let payload=serde_json::to_vec(&serde_json::json!({"exp":now/1000+offset,"https://api.openai.com/auth":{"chatgpt_user_id":"fixture-external-user","chatgpt_account_id":account}})).map_err(|_|invalid())?;
    let access = format!("e30.{}.fixture", base64url(&payload));
    gateway_auth_credentials::native_credentials::parse_native_document(
        &serde_json::json!({"auth_mode":"chatgptAuthTokens","OPENAI_API_KEY":null,"tokens":{"account_id":account,"access_token":access,"id_token":access,"refresh_token":""},"last_refresh":null}),
    )
}
async fn bootstrap(
    env: &Env,
    request: &Request,
    kind: &str,
    id: &NativeTransportIdentity,
) -> Result<Case, GatewayError> {
    let store = RemoteAuthorityStore::from_env(env)?;
    let inputs = gateway_worker_fixture_setup::native_config::inputs()?;
    let receiver = PublicationReceiver::configured(
        id.clone(),
        inputs.clone(),
        Vec::new(),
        gateway_worker_fixture_setup::native_config::now,
    )?;
    gateway_worker_fixture_setup::initialize(&store, id, &inputs, &receiver, "direct").await?;
    let now = TokenClock.now_ms()?;
    let inventory = input()?;
    let original = gateway_auth_credentials::provider_credentials::parse_codex_source_pool(
        &inventory["input"]["codexAuthPool"],
    )?
    .ok_or_else(invalid)?;
    let mut accounts = Vec::new();
    for previous in original.accounts {
        let next = native(&previous.account_id, now, 300)?;
        let identity = gateway_auth_credentials::provider_credentials::codex_credential_identity(
            &next.account_id,
            &next.access_token,
            &next.refresh_token,
        );
        accounts.push(
            gateway_auth_credentials::provider_credentials::CodexSourceAccount {
                account_id: next.account_id,
                access_token: next.access_token,
                refresh_token: next.refresh_token,
                updated_at_ms: now as f64,
                native_owner: Some(CodexNativeOwner {
                    codex_home: "/tmp/worker-external-synthetic-owner".into(),
                    generation_hash: identity.native_generation_hash,
                    native_refreshed_at: None,
                    external_user_id: next.external_user_id,
                    managed_source: None,
                }),
            },
        );
    }
    let source = gateway_auth_credentials::provider_credentials::CodexSourcePool {
        accounts,
        updated_at_ms: now as f64,
    };
    let revision = TokenClock.next_token()?;
    let normalized = SubscriptionPool {
        v: 1,
        pool_versionstamp: gateway_contracts::identity::PoolVersionstamp(revision.clone()),
        accounts: source
            .accounts
            .iter()
            .map(|a| {
                let i = a.identity();
                gateway_authority_routing_records::types::PoolAccount {
                    account_id_hash: i.routing_account_hash,
                    admin_account_hash: i.administrative_account_hash,
                    credential_version: i.credential_version,
                    enabled: true,
                }
            })
            .collect(),
    };
    let source_key = codex_source_pool_key();
    let normalized_key = keys::pool();
    if store
        .commit(AtomicTransition {
            checks: vec![
                gateway_core::state::RevisionCheck {
                    key: source_key.clone(),
                    expected_revision: None,
                },
                gateway_core::state::RevisionCheck {
                    key: normalized_key.clone(),
                    expected_revision: None,
                },
            ],
            mutations: vec![
                RecordMutation::Put {
                    key: source_key,
                    record: StoredRecord::json(&revision, source.private_source_value(), None),
                },
                RecordMutation::Put {
                    key: normalized_key,
                    record: StoredRecord::json(
                        TokenClock.next_token()?,
                        serde_json::to_value(normalized).map_err(|_| invalid())?,
                        None,
                    ),
                },
            ],
        })
        .await?
        != gateway_core::state::CommitOutcome::Committed
    {
        return Err(invalid());
    }
    // Snapshot/runtime are ordinary fresh fixture DATA; real configuration publication rebinds its current revision.
    for (key, value) in [
        (
            gateway_schema::catalog_store::codex_snapshot_key(),
            inventory["input"]["snapshot"].clone(),
        ),
        (
            gateway_schema::catalog_store::runtime_config_key(),
            inventory["runtime"].clone(),
        ),
    ] {
        let before = store.read(&key).await?;
        let revision = TokenClock.next_token()?;
        if store
            .commit(AtomicTransition {
                checks: vec![gateway_core::state::RevisionCheck {
                    key: key.clone(),
                    expected_revision: before.as_ref().map(|r| r.revision.clone()),
                }],
                mutations: vec![RecordMutation::Put {
                    key,
                    record: StoredRecord::json(revision, value, None),
                }],
            })
            .await?
            != gateway_core::state::CommitOutcome::Committed
        {
            return Err(invalid());
        }
    }
    gateway_worker_fixture_setup::runtime_configuration::publish(&store, id, &inputs, &receiver)
        .await?;
    let host = Host::new(env.clone());
    let cfg = config(env);
    let subject = JsString16::from_units(vec![0xd800, 0]);
    let grant = SourceJson::parse(&format!(
        r#"{{"subject":"\ud800\u0000","clientId":"ai","audience":"ai","origin":"https://ai.ubq.fi","redirectUri":"https://ai.ubq.fi/auth/callback","createdAt":"10/27/2026","token":"uos_app_session_{}"}}"#,
        "A".repeat(43)
    ))?;
    let token = PassportSessions {
        store: &store,
        host: &host,
    }
    .create(&grant, &subject, None)
    .await?;
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
        request_id: RequestId(TokenClock.next_token()?),
        principal: PrincipalId::from_source(JsString16::from_units(
            "passport-user:".encode_utf16().chain([0xd800, 0]).collect(),
        )),
        key_id: None,
        route: if kind == "inference" {
            "responses"
        } else {
            "live.sideband"
        }
        .into(),
        started_at_ms: now,
    };
    let owner = RegisteredRenewalOwner::configured(
        "fixture-external-owner".into(),
        "/tmp/worker-external-synthetic-owner".into(),
    )?;
    let _ = request;
    Ok(Case {
        authority: Authority::with_clock(store, TokenClock, TokenClock),
        host,
        config: cfg,
        identity: id.clone(),
        owner,
        original,
        context,
    })
}
async fn prepare(
    case: &Case,
    kind: &str,
    context: &Context,
    request: &Request,
) -> Result<Vec<u8>, GatewayError> {
    let actor_service = ActorService::new(case.authority.store(), &case.host, &case.config);
    let actor = if kind == "cache" {
        actor_service
            .authenticate_source_admin_actor(&case.original, &case.context)
            .await?
    } else {
        actor_service
            .authenticate_source_client_actor(&case.original, &case.context)
            .await?
    };
    let prep = case.authority.source_native_renewals(
        &case.host,
        &case.config,
        &case.identity,
        &case.owner,
        &Cancel,
    );
    let action = match kind {
        "inference" => inference::prepare(case, context, request).await?,
        "cache" => cache::prepare(case, &actor).await?,
        "live_create" => {
            use gateway_authority_routing_selection::{
                SelectionOperations, types::LiveRouteSelection,
            };
            let LiveRouteSelection::Eligible(selected) = case
                .authority
                .select_live_account(None, TokenClock.now_ms()?)
                .await?
            else {
                return Err(invalid());
            };
            prep.prepare_source_live_create_renewal(&actor, &selected)
                .await?
        }
        "live_mapped" => {
            let raw = case
                .authority
                .store()
                .read(&codex_source_pool_key())
                .await?
                .ok_or_else(invalid)?;
            let pool = parse_codex_source_pool(raw.json_value()?)?.ok_or_else(invalid)?;
            #[derive(serde::Serialize)]
            struct Mapping<'a> {
                account_id: &'a str,
                principal_id: &'a JsString16,
            }
            let mapping = SourceJson::from_serializable(&Mapping {
                account_id: &pool.accounts[0].account_id,
                principal_id: case.context.principal.as_source(),
            })?;
            case.authority
                .store()
                .commit(AtomicTransition {
                    checks: Vec::new(),
                    mutations: vec![RecordMutation::Put {
                        key: mapping_key("source-external", false),
                        record: encode_source_control_record(
                            TokenClock.next_token()?,
                            &mapping,
                            None,
                        )?,
                    }],
                })
                .await?;
            prep.prepare_source_live_mapped_renewal(&actor, "source-external")
                .await?
        }
        _ => return Err(invalid()),
    };
    let operation = "fixture-external-operation";
    let control = serde_json::to_vec(&NativeControlEnvelope {
        v: 1,
        operation_id: operation.into(),
        request: NativeControlRequest::CodexRenewAccess {
            action_ref: action.action_ref().into(),
        },
    })
    .map_err(|_| invalid())?;
    let proof = native_proof(
        &case.identity,
        case.authority.store(),
        NATIVE_CONTROL_PATH,
        &control,
        211,
    )
    .await?;
    let lifecycle = case.authority.source_native_lifecycle(
        &case.host,
        &case.config,
        &case.identity,
        &case.owner,
        &Cancel,
    );
    let mut claim = lifecycle
        .claim(&proof, action.action_ref(), operation, &control)
        .await?;
    lifecycle.start(&mut claim).await?;
    let raw = case
        .authority
        .store()
        .read(&codex_source_pool_key())
        .await?
        .ok_or_else(invalid)?;
    let source = parse_codex_source_pool(raw.json_value()?)?.ok_or_else(invalid)?;
    let selected = source
        .accounts
        .iter()
        .find(|a| a.identity().routing_account_hash.0 == action.fence().account_id_hash)
        .ok_or_else(invalid)?;
    let next = native(&selected.account_id, TokenClock.now_ms()?, 600)?;
    let command=gateway_native_external_publication_wire::command::PreparedSourceExternalAccessCommand::from_registered(gateway_native_external_publication_wire::command::ExpectedSourceExternalAccessPublication{fence:action.fence(),action_ref:action.action_ref(),operation_id:operation,claim_ref:claim.claim_ref(),publication_revision:"fixture-external-publication"},&next)?;
    let old_owner = selected.native_owner.as_ref().ok_or_else(invalid)?;
    let unchanged = gateway_auth_credentials::native_credentials::NativeCredentials {
        account_id: selected.account_id.clone(),
        access_token: selected.access_token.clone(),
        refresh_token: selected.refresh_token.clone(),
        email: None,
        native_refreshed_at: old_owner.native_refreshed_at.clone(),
        external_user_id: old_owner.external_user_id.clone(),
    };
    let unchanged_command=gateway_native_external_publication_wire::command::PreparedSourceExternalAccessCommand::from_registered(gateway_native_external_publication_wire::command::ExpectedSourceExternalAccessPublication{fence:action.fence(),action_ref:action.action_ref(),operation_id:operation,claim_ref:claim.claim_ref(),publication_revision:"fixture-external-unchanged"},&unchanged)?;
    serde_json::to_vec(&serde_json::json!({"v":2,"context":kind,"action_ref":action.action_ref(),"operation_id":operation,"claim_ref":claim.claim_ref(),"publication_revision":"fixture-external-publication","unchanged_publication_revision":"fixture-external-unchanged","request_base64":base64url(&command.bytes()?),"unchanged_request_base64":base64url(&unchanged_command.bytes()?)})).map_err(|_|invalid())
}
pub async fn fetch(
    request: Request,
    env: Env,
    context: Context,
) -> worker::Result<gateway_worker_web::response::GatewayResponse> {
    let result = async {
        if env
            .var("UOS_MATRIX_SCENARIO")
            .map_err(|_| invalid())?
            .to_string()
            != "native-source-external"
        {
            return Err(invalid());
        }
        let kind = request
            .url()
            .map_err(|_| invalid())?
            .query_pairs()
            .find(|(name, _)| name == "source_context")
            .map(|(_, v)| v.into_owned())
            .unwrap_or_else(|| "live_mapped".into());
        if !["live_mapped", "live_create", "cache", "inference"].contains(&kind.as_str()) {
            return Err(invalid());
        }
        let id = identity()?;
        let store = RemoteAuthorityStore::from_env(&env)?;
        let key = meta_key(&kind);
        let metadata = if let Some(row) = store.read(&key).await? {
            row.json_value()?
                .get("metadata_base64")
                .and_then(serde_json::Value::as_str)
                .ok_or_else(invalid)?
                .into()
        } else {
            if request.method() != worker::Method::Get || request.path() != "/health" {
                return Err(invalid());
            }
            let case = bootstrap(&env, &request, &kind, &id).await?;
            let bytes = prepare(&case, &kind, &context, &request).await?;
            let metadata = base64url(&bytes);
            let record = StoredRecord::json(
                TokenClock.next_token()?,
                serde_json::json!({"metadata_base64":metadata}),
                None,
            );
            if store
                .commit(AtomicTransition {
                    checks: vec![gateway_core::state::RevisionCheck {
                        key: key.clone(),
                        expected_revision: None,
                    }],
                    mutations: vec![RecordMutation::Put { key, record }],
                })
                .await?
                != gateway_core::state::CommitOutcome::Committed
            {
                return Err(invalid());
            }
            metadata
        };
        Ok((id, metadata))
    }
    .await;
    let (id, metadata) =
        result.map_err(|e: GatewayError| worker::Error::RustError(e.to_string()))?;
    let inputs = gateway_worker_fixture_setup::native_config::inputs()
        .map_err(|e| worker::Error::RustError(e.to_string()))?;
    let receiver = PublicationReceiver::configured(
        id.clone(),
        inputs,
        Vec::new(),
        gateway_worker_fixture_setup::native_config::now,
    )
    .map_err(|e| worker::Error::RustError(e.to_string()))?;
    let bindings =
        gateway_worker::native_host::NativeBindings::configured(env.service("ASSETS")?, id)
            .with_renewal_owners(
                RegisteredRenewalOwners::configured(vec![
                    RegisteredRenewalOwner::configured(
                        "fixture-external-owner".into(),
                        "/tmp/worker-external-synthetic-owner".into(),
                    )
                    .map_err(|e| worker::Error::RustError(e.to_string()))?,
                ])
                .map_err(|e| worker::Error::RustError(e.to_string()))?,
            );
    let published = if request.path()
        == gateway_auth_native_wire::native_read::NATIVE_RENEWAL_AUTHORITY_PATH
    {
        let mut cloned = request.clone()?;
        matches!(
            cloned
                .json::<gateway_native_authority_request::NativeRenewalAuthorityRequest>()
                .await,
            Ok(
                gateway_native_authority_request::NativeRenewalAuthorityRequest::Published { .. }
                    | gateway_native_authority_request::NativeRenewalAuthorityRequest::Unknown { .. }
            )
        )
    } else {
        false
    };
    let response =
        gateway_worker::fetch_with_bindings(request, env, context, Some(&receiver), bindings).await;
    let finished = published
        && response
            .as_ref()
            .is_ok_and(|reply| reply.status_code() == 200)
        || response.is_err();
    let settled = if finished {
        inference::release_and_settle().await
    } else {
        Ok(())
    };
    let mut response = response?;
    response
        .headers_mut()
        .set("x-uos-fixture-source-external-metadata", &metadata)?;
    if finished {
        let observation = match settled {
            Ok(()) => serde_json::json!({"state":"settled"}),
            Err(error) => {
                serde_json::json!({"state":"error","status":error.status,"error":error.envelope})
            }
        };
        let bytes = serde_json::to_vec(&observation)
            .map_err(|e| worker::Error::RustError(e.to_string()))?;
        response
            .headers_mut()
            .set("x-uos-fixture-inference-settlement", &base64url(&bytes))?;
    }
    Ok(response)
}
