use gateway_admin_contracts::{
    operations::{AdminHost, AdminInput, PaidKeyFields, missing_capability},
    routes::AdminOperation,
};
use gateway_admin_keys::keys::{KeyService, deletion_key};
use gateway_admin_test_support::{
    contract_host::Host,
    key_fixtures::{ClaimedDeletion, Pricing, actor, source, wire_json},
    runtime_host::run,
    source_store::Store,
};
use gateway_auth::{
    crypto::Secret,
    types::{AuthHost, AuthResponse},
};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, RevisionCheck, StoredRecord,
};
use gateway_kv_codec::key::RecordKey;
use serde_json::json;
use std::{
    cell::{Cell, RefCell},
    collections::BTreeMap,
};

struct DeletionHooks<'a> {
    inner: ClaimedDeletion<'a>,
    fail_finish: Cell<bool>,
    conflict: bool,
    finishes: Cell<usize>,
    aborts: Cell<usize>,
}
impl AdminHost for DeletionHooks<'_> {
    async fn execute(
        &self,
        operation: AdminOperation,
        _: AdminInput,
    ) -> Result<AuthResponse, GatewayError> {
        Err(missing_capability(operation))
    }
    async fn paid_key_policy(&self, _: &SourceJson) -> Result<PaidKeyFields, GatewayError> {
        Err(missing_capability(AdminOperation::ApiKeysUpdate))
    }
    async fn prepare_key_deletion(&self, id: &str) -> Result<Vec<RevisionCheck>, GatewayError> {
        let checks = self.inner.prepare_key_deletion(id).await?;
        if self.conflict {
            let key = gateway_auth::api_keys::id_key(id);
            let mut row = self.inner.store.read(&key).await?.unwrap();
            row.revision = "concurrent-key-writer".into();
            self.inner.store.rows.borrow_mut().insert(key.0, row);
        }
        Ok(checks)
    }
    async fn finish_key_deletion(
        &self,
        id: &str,
        checks: &[RevisionCheck],
    ) -> Result<(), GatewayError> {
        self.finishes.set(self.finishes.get() + 1);
        if self.fail_finish.replace(false) {
            return Err(GatewayError::new(
                500,
                "Fixture post-cleanup failed",
                Some("fixture_cleanup_failed"),
            ));
        }
        self.inner.finish_key_deletion(id, checks).await
    }
    async fn abort_key_deletion(
        &self,
        id: &str,
        checks: &[RevisionCheck],
    ) -> Result<(), GatewayError> {
        self.aborts.set(self.aborts.get() + 1);
        self.inner.abort_key_deletion(id, checks).await
    }
}

#[test]
fn deletion_cleanup_is_guarded_idempotent_and_preserves_source_error_ordering() {
    run(async {
        for (fail_finish, conflict) in [(false, false), (true, false), (false, true)] {
            let store = Store::default();
            let host = Host::default();
            let caps = DeletionHooks {
                inner: ClaimedDeletion {
                    store: &store,
                    host: &host,
                },
                fail_finish: Cell::new(fail_finish),
                conflict,
                finishes: Cell::new(0),
                aborts: Cell::new(0),
            };
            let service = KeyService {
                store: &store,
                host: &host,
                capabilities: &caps,
                release: "fixture",
            };
            let created = service
                .create(&source(&json!({"name":"Cleanup"})), &actor(), "create")
                .await
                .unwrap();
            let created_body = wire_json(&created.body);
            let id = created_body["id"].as_str().unwrap();
            service
                .mutate(
                    AdminOperation::ApiKeysRevoke,
                    &source(&json!({"id":id})),
                    &actor(),
                    "revoke",
                )
                .await
                .unwrap();
            let legacy = gateway_auth::store::key(json!(["ubq_ai", "api_keys", "usage", id]));
            let daily = gateway_auth::store::key(json!(["ubq_ai", "api_keys", "usage_daily", id]));
            let history = gateway_auth::store::key(json!([
                "uos_ai",
                "paid_fallback",
                "ledger",
                id,
                1,
                "fixture-history"
            ]));
            for key in [&legacy, &daily, &history] {
                store.rows.borrow_mut().insert(
                    key.0.clone(),
                    StoredRecord::json("history", json!({"billing_status":"settled"}), None),
                );
            }
            let result = service
                .mutate(
                    AdminOperation::ApiKeysDelete,
                    &source(&json!({"id":id})),
                    &actor(),
                    "delete",
                )
                .await;
            let guard = store.read(&deletion_key(id)).await.unwrap().unwrap();
            assert_eq!(guard.expires_at_ms, None);
            if conflict {
                assert_eq!(result.unwrap_err().status, 409);
                assert_eq!(caps.aborts.get(), 1);
                assert_eq!(caps.finishes.get(), 0);
                assert!(
                    store
                        .read(&gateway_auth::api_keys::id_key(id))
                        .await
                        .unwrap()
                        .is_some()
                );
                assert!(store.read(&legacy).await.unwrap().is_some());
                assert!(store.read(&history).await.unwrap().is_some());
                continue;
            }
            assert!(
                store
                    .read(&gateway_auth::api_keys::id_key(id))
                    .await
                    .unwrap()
                    .is_none()
            );
            assert!(
                store.read(&legacy).await.unwrap().is_none()
                    && store.read(&daily).await.unwrap().is_none()
            );
            assert_eq!(caps.aborts.get(), 0);
            assert_eq!(caps.finishes.get(), 1);
            if fail_finish {
                assert_eq!(result.unwrap_err().status, 500);
                assert!(store.read(&history).await.unwrap().is_some());
                assert_eq!(
                    service
                        .mutate(
                            AdminOperation::ApiKeysDelete,
                            &source(&json!({"id":id})),
                            &actor(),
                            "retry"
                        )
                        .await
                        .unwrap_err()
                        .status,
                    404
                );
            } else {
                assert_eq!(wire_json(&result.unwrap().body), json!({"id":id}));
            }
            let checks = vec![RevisionCheck {
                key: deletion_key(id),
                expected_revision: Some(guard.revision.clone()),
            }];
            caps.finish_key_deletion(id, &checks).await.unwrap();
            caps.finish_key_deletion(id, &checks).await.unwrap();
            assert!(store.read(&history).await.unwrap().is_none());
            assert_eq!(store.read(&deletion_key(id)).await.unwrap().unwrap(), guard);
        }
    });
}

struct PaidRaceStore<'a> {
    store: &'a Store,
    host: &'a Host,
    key_id: String,
    armed: Cell<bool>,
}

struct PaidFixtureClock;
impl gateway_authority::store::AuthorityClock for PaidFixtureClock {
    fn now_ms(&self) -> Result<i64, GatewayError> {
        Ok(1_000_000)
    }
}

async fn paid_fixture_owner_proof(
    store: &Store,
    host: &Host,
    identity: &gateway_auth::native_auth::NativeTransportIdentity,
    body: &[u8],
) -> gateway_auth::native_auth::AuthenticatedNativeOwner {
    use gateway_auth::{
        crypto::base64url, native_auth::*,
        native_replay::StoreNonceAuthority,
    };
    use gateway_auth_native_wire::native_read::NATIVE_PUBLICATION_PATH;
    let nonce = base64url(&host.random_bytes(32).unwrap());
    let time = host.now_ms().to_string();
    let mac = identity
        .sign(NATIVE_PUBLICATION_PATH, host.now_ms(), &nonce, body)
        .unwrap();
    let nonces = StoreNonceAuthority::new(store);
    NativeAuthenticator::new(identity, &nonces)
        .authenticate(
            NATIVE_PUBLICATION_PATH,
            NativeAuthHeaders {
                version: "1",
                key_id: identity.key_id(),
                time_ms: &time,
                nonce: &nonce,
                mac: &mac,
            },
            body,
            host.now_ms(),
        )
        .await
        .unwrap()
}

async fn prepare_paid_fixture_custody(
    store: &Store,
    host: &Host,
) -> Result<(), Box<dyn std::error::Error>> {
    use gateway_auth::native_auth::{NativeCapability, NativeTransportIdentity};
    use gateway_authority::{keys, paid_control, paid_types::PaidRoutingPolicy};
    use gateway_contracts::provider::PaidProvider;
    use gateway_publication_policy::control_publication::{
        ConfiguredPaidInputs, PreparedConfigurationPublication, bootstrap_fixture_owner,
        fixture_owner_statement,
    };
    // An explicit isolated owner signs the actual configuration publication;
    // authorization below obtains the non-deserializable receipt from Authority.
    let identity = NativeTransportIdentity::new(
        "fixture-admin-race".into(),
        "fixture-admin-race".into(),
        "fixture-epoch".into(),
        [17; 32],
        std::collections::BTreeSet::from([NativeCapability::ConfigurationPublish]),
    )?;
    let owner_body = fixture_owner_statement(&identity)?;
    let owner_proof = paid_fixture_owner_proof(store, host, &identity, &owner_body).await;
    bootstrap_fixture_owner(store, &identity, &owner_proof, host.now_ms(), "race-owner").await?;
    let inputs = ConfiguredPaidInputs::new(
        Some(Secret::new("fixture-surplus-key".into())),
        None,
        None,
        json!({}),
    )?;
    let prepared = PreparedConfigurationPublication::capture(
        store,
        &identity,
        &inputs,
        "race-generation",
        host.now_ms(),
    )
    .await?;
    let config_proof = paid_fixture_owner_proof(store, host, &identity, prepared.statement()).await;
    let config = prepared
        .publish(store, &config_proof, host.now_ms(), "race-config")
        .await?;
    let observation = paid_control::PaidProviderObservation {
        v: 1,
        provider: PaidProvider::Surplus,
        owner_id: config.owner_id,
        owner_epoch: config.owner_epoch,
        configuration_generation: config.generation,
        configuration_fingerprint: config.configuration_fingerprint,
        credential_binding: inputs
            .binding(PaidProvider::Surplus)
            .ok_or("fixture surplus binding missing")?,
        observed_at_ms: host.now_ms(),
        source_valid_until_ms: None,
        stale: false,
        models: vec![source(
            &json!({"id":"paid-model","supported_endpoint_types":["openai-response"],"supports_tools":true,"input_price_per_token":0.001,"output_price_per_token":0.002}),
        )],
        source_checks: Vec::new(),
    };
    store.rows.borrow_mut().insert(
        keys::paid_control_observation(PaidProvider::Surplus).0,
        StoredRecord::json("race-observation", serde_json::to_value(observation)?, None),
    );
    let mut source_checks = Vec::new();
    for key in paid_control::paid_projection_source_keys() {
        source_checks.push(paid_control::PaidSourceCheck {
            expected_revision: store.read(&key).await?.map(|row| row.revision),
            key: key.0,
        });
    }
    let projection = paid_control::PaidRoutingProjection {
        policy: PaidRoutingPolicy {
            v: 2,
            codex_enabled: true,
            codex_models: vec!["codex-only".into()],
            models: BTreeMap::from([("paid-model".into(), vec![PaidProvider::Surplus])]),
        },
        provenance: paid_control::PaidProjectionProvenance {
            v: 1,
            owner_revision: "race-owner".into(),
            config_revision: "race-config".into(),
            surplus_observation_revision: Some("race-observation".into()),
            openlux_observation_revision: None,
            observed_at_ms: host.now_ms(),
            source_valid_until_ms: None,
            source_checks,
        },
    };
    store.rows.borrow_mut().insert(
        keys::paid_routing().0,
        StoredRecord::json("race-routing", serde_json::to_value(projection)?, None),
    );
    Ok(())
}
impl AuthorityStore for PaidRaceStore<'_> {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        self.store.read(key).await
    }
    async fn commit(&self, transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        let attempt = gateway_authority::keys::paid_attempt(&self.key_id, "racing-attempt");
        if transition
            .mutations
            .iter()
            .any(|mutation| matches!(mutation,RecordMutation::Put{key,..}if *key==attempt))
            && self.armed.replace(false)
        {
            let caps = ClaimedDeletion {
                store: self.store,
                host: self.host,
            };
            let service = KeyService {
                store: self.store,
                host: self.host,
                capabilities: &caps,
                release: "fixture",
            };
            service
                .mutate(
                    AdminOperation::ApiKeysRevoke,
                    &source(&json!({"id":self.key_id})),
                    &actor(),
                    "race-revoke",
                )
                .await?;
            assert_eq!(
                service
                    .mutate(
                        AdminOperation::ApiKeysDelete,
                        &source(&json!({"id":self.key_id})),
                        &actor(),
                        "race-delete"
                    )
                    .await?
                    .status,
                200
            );
        }
        self.store.commit(transition).await
    }
}

#[test]
fn claimed_deletion_uses_new_guard_revision_and_fences_actual_inflight_paid_admission()
-> Result<(), Box<dyn std::error::Error>> {
    run(async {
        use gateway_authority::{
            paid_control::PaidRequestContext,
            paid_types::{PaidAuthorization, PaidRequest},
        };
        use gateway_contracts::{
            identity::{ApiKeyId, PaidAttemptId, RequestId},
            provider::PaidProvider,
        };
        let store = Store::default();
        let host = Host::default();
        let pricing = Pricing {
            inputs: RefCell::new(Vec::new()),
        };
        let key = KeyService {
            store: &store,
            host: &host,
            capabilities: &pricing,
            release: "fixture",
        }
        .create(
            &source(&json!({"name":"Race","paid_fallback_enabled":true,"paid_fallback_limit_credits":1})),
            &actor(),
            "create",
        )
        .await?;
        let id = wire_json(&key.body)["id"]
            .as_str()
            .ok_or("created fixture key id missing")?
            .to_owned();
        prepare_paid_fixture_custody(&store, &host).await?;
        let raced = PaidRaceStore {
            store: &store,
            host: &host,
            key_id: id.clone(),
            armed: Cell::new(true),
        };
        let authority =
            gateway_authority::store::Authority::with_clock(raced, &host, PaidFixtureClock);
        let request = PaidRequest {
            key_id: ApiKeyId(id.clone()),
            request_id: RequestId("racing-request".into()),
            attempt_id: PaidAttemptId("racing-attempt".into()),
            provider: PaidProvider::Surplus,
            model: "paid-model".into(),
            route: "responses".into(),
            path: "/v1/responses".into(),
            stream: false,
            reasoning: None,
            created_at_ms: 1_000_000,
        };
        let receipt = authority
            .authorize_paid_route(
                &request,
                &PaidRequestContext {
                    original_model: request.model.clone(),
                    body: source(&json!({"model":request.model,"stream":false})),
                    surface: gateway_providers::types::ClientSurface::Responses,
                    synthetic_hop: None,
                },
                1_000_000,
            )
            .await?;
        let receipt = bind_paid_fixture_original(&authority, &store, &request, &receipt).await?;
        assert!(
            store
                .read(&gateway_auth::api_keys::id_key(&id))
                .await?
                .is_some()
        );
        assert!(
            authority
                .reserve_paid(
                    &request,
                    &PaidAuthorization::DirectPaidModel,
                    &receipt,
                    1_000_000
                )
                .await
                .is_err()
        );
        assert!(
            store
                .read(&gateway_auth::api_keys::id_key(&id))
                .await?
                .is_none()
        );
        assert!(
            store
                .read(&gateway_authority::keys::paid_attempt(
                    &id,
                    "racing-attempt"
                ))
                .await?
                .is_none()
        );
        assert!(store.read(&deletion_key(&id)).await?.is_some());
        Ok(())
    })
}

async fn bind_paid_fixture_original(
    authority: &gateway_authority::store::Authority<PaidRaceStore<'_>, &Host>,
    store: &Store,
    request: &gateway_authority::paid_types::PaidRequest,
    receipt: &gateway_authority::paid_control::PaidRoutingReceipt,
) -> Result<gateway_authority::paid_control::PaidRoutingReceipt, GatewayError> {
    // Controlled authority regression: use the canonical finite original
    // fixture chain, not a route-only receipt or a Worker-runtime claim.
    let original = gateway_contracts::identity::RequestContext {
        request_id: request.request_id.clone(),
        principal: gateway_contracts::identity::PrincipalId("fixture-original-principal".into()),
        key_id: Some(request.key_id.clone()),
        route: request.route.clone(),
        started_at_ms: request.created_at_ms,
    };
    let api = authority
        .reserve_api_key(
            &request.key_id,
            &request.request_id,
            &request.route,
            1_000_000,
        )
        .await?;
    let journal_key = gateway_auth::store::key(json!([
        "uos_ai",
        "routing_runtime",
        "v1",
        "session",
        request.request_id.0
    ]));
    store.rows.borrow_mut().insert(
        journal_key.0,
        StoredRecord::json(
            "fixture-original-journal",
            json!({
                "v":1,"context":original,"input_digest":gateway_auth::crypto::sha256_hex(b"fixture-original-input"),
                "api":api,"kernel":null,"cursor":0,"attempts":[],"active":null,
                "model_sibling_retries":0,"refreshed_versions":[],"recovery_attempted":false,"claude_unavailable":false,
                "semantic_committed":false,"terminal_intent":null,"finished":false,"recovering":false,
                "lease_expires_at_ms":1_000_000+gateway_authority::REQUEST_LEASE_MS,"updated_at_ms":1_000_000,"extras_family":null,
                "extras_child_count":null,"extras_dispatch":null,"children":[],"kernel_override":null
            }),
            None,
        ),
    );
    authority
        .bind_paid_original_request(receipt, &original, 1_000_000)
        .await
}
