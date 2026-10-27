use gateway_catalog_control::control::publish_paid_observation;
use gateway_catalog_control::control::publish_paid_routing;
mod original_request_fixture;
type TestResult = Result<(), Box<dyn std::error::Error>>;

use gateway_routing_runtime_test_support::source_catalog;
type MemoryStore = Store;
type TestAuthority = gateway_authority::store::Authority<Store, Tokens>;
use gateway_authority::{keys, paid_control::*};
use gateway_catalog_inputs::state;
use gateway_catalog_provider::host::{
    CatalogFetch, CatalogFetchResponse, CatalogHost, CatalogProvider, CredentialPublication,
    ProviderBinding,
};
use gateway_catalog_runtime::CatalogRuntime;
use gateway_core::error::GatewayError;
use gateway_core::provider::PaidProvider;
use gateway_core::query::{QueryPage, QueryRequest, QueryStore};
use gateway_core::source_control::decode_source_control_record;
use gateway_core::source_json::SourceJson;
use gateway_core::state::{
    AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, RecordMutation, StoredRecord,
};
use gateway_schema::{
    catalog_store, runtime_config::build_runtime_config, snapshot::ModelSnapshot,
};
use serde_json::{Value, json};
use std::{
    cell::{Cell, RefCell},
    collections::{BTreeMap, VecDeque},
    future::{Future, poll_fn},
    rc::Rc,
    sync::Arc,
    task::{Context, Poll, Waker},
};
fn run<T>(future: impl Future<Output = T>) -> T {
    let mut future = std::pin::pin!(future);
    let mut cx = Context::from_waker(Waker::noop());
    loop {
        if let Poll::Ready(result) = future.as_mut().poll(&mut cx) {
            return result;
        }
    }
}
type Callback = Rc<dyn Fn()>;
#[derive(Clone)]
struct Store {
    rows: Rc<RefCell<BTreeMap<String, StoredRecord>>>,
    now: Rc<Cell<i64>>,
    before_policy: Rc<RefCell<Option<Callback>>>,
}
impl Store {
    fn new() -> Self {
        Self {
            rows: Rc::default(),
            now: Rc::new(Cell::new(1_800_000_000_000)),
            before_policy: Rc::default(),
        }
    }
    fn put(&self, key: RecordKey, revision: &str, value: Value) {
        self.rows
            .borrow_mut()
            .insert(key.0, StoredRecord::json(revision, value, None));
    }
    fn get(&self, key: &RecordKey) -> Option<StoredRecord> {
        self.rows
            .borrow()
            .get(&key.0)
            .filter(|row| row.expires_at_ms.is_none_or(|until| until > self.now.get()))
            .cloned()
    }
    fn config(&self, generation: &str, revision: &str, surplus: bool, openlux: bool) {
        self.put(
            keys::paid_control_config(),
            revision,
            json!(PaidControlConfig {
                v: PAID_CONTROL_CONFIG_VERSION,
                owner_id: "fixture-owner".into(),
                owner_epoch: "epoch-1".into(),
                generation: generation.into(),
                configuration_fingerprint: format!("configuration-{generation}"),
                surplus_binding: surplus.then(|| "surplus-credential".into()),
                openlux_discovery_binding: openlux.then(|| "openlux-discovery-credential".into()),
                openlux_binding: openlux.then(|| "openlux-credential".into()),
                source_checks: Vec::new()
            }),
        );
    }
    fn install(&self, surplus: bool, openlux: bool) {
        self.put(
            keys::paid_control_owner(),
            "owner-rev",
            json!(PaidControlOwner {
                v: 1,
                owner_id: "fixture-owner".into(),
                owner_epoch: "epoch-1".into(),
                key_fingerprint: "ephemeral-fixture-carrier-fingerprint".into()
            }),
        );
        self.config("generation-1", "config-rev", surplus, openlux);
        let snapshot = ModelSnapshot {
            models: vec![source_catalog::json(
                &json!({"slug":"gpt-known","context_window":128000,"supported_reasoning_levels":[{"effort":null},{"effort":"custom-tier"}]}),
            )],
            source: "chatgpt_codex".into(),
            updated_at_ms: self.now.get(),
            client_version: Some("0.163.0".into()),
        };
        let config = match build_runtime_config(&snapshot, None, None, self.now.get()) {
            Ok(config) => config,
            Err(error) => panic!("fixed runtime fixture configuration failed: {error:?}"),
        };
        let config = match config.deserialize::<Value>() {
            Ok(config) => config,
            Err(error) => panic!("fixed runtime fixture is not scalar JSON: {error:?}"),
        };
        self.put(catalog_store::runtime_config_key(), "runtime-rev", config);
        self.put(
            catalog_store::codex_snapshot_key(),
            "full-rev",
            json!(snapshot),
        );
        self.put(
            catalog_store::auth_generation_key(),
            "catalog-generation-rev",
            json!("catalog-generation"),
        );
    }
    // Reuse the established authority fixture's full journal writer and actual
    // reserve_api_key/bind_paid_original_request flow; do not clone its DTO.
    fn row(&self, key: &RecordKey) -> Option<StoredRecord> {
        self.get(key)
    }
    fn seed(&self, key: RecordKey, value: &Value) {
        self.put(key, "original-fixture", value.clone());
    }
    fn projection(&self) -> PaidRoutingProjection {
        let Some(row) = self.get(&keys::paid_routing()) else {
            panic!("expected paid routing fixture projection");
        };
        match decode_source_control_record(&row).and_then(|document| document.deserialize()) {
            Ok(projection) => projection,
            Err(error) => panic!("invalid paid routing fixture projection: {error:?}"),
        }
    }
}
impl AuthorityStore for Store {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        Ok(self.get(key))
    }
    async fn commit(&self, transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        if transition.mutations.iter().any(
            |mutation| matches!(mutation,RecordMutation::Put{key,..}if *key==keys::paid_routing()),
        ) {
            let callback = self.before_policy.borrow_mut().take();
            if let Some(callback) = callback {
                callback();
            }
        }
        if transition.checks.iter().any(|check| {
            self.get(&check.key).as_ref().map(|row| &row.revision)
                != check.expected_revision.as_ref()
        }) {
            return Ok(CommitOutcome::Conflict);
        }
        for mutation in transition.mutations {
            match mutation {
                RecordMutation::Put { key, record } => {
                    self.rows.borrow_mut().insert(key.0, record);
                }
                RecordMutation::Delete { key } => {
                    self.rows.borrow_mut().remove(&key.0);
                }
            }
        }
        Ok(CommitOutcome::Committed)
    }
}
impl QueryStore for Store {
    async fn query(&self, request: QueryRequest) -> Result<QueryPage, GatewayError> {
        request.validate()?;
        Err(GatewayError::unavailable(
            "This fixed-key publisher must not scan state",
            "unexpected_query",
        ))
    }
}
struct Fetch {
    body: SourceJson,
    status: u16,
    ticks: u32,
    after: Option<Callback>,
}
struct Host {
    store: Store,
    publication: RefCell<CredentialPublication>,
    responses: RefCell<VecDeque<Fetch>>,
    requests: RefCell<Vec<CatalogFetch>>,
    sequence: Cell<u32>,
}
impl Host {
    fn new(store: &Store, surplus: bool, openlux: bool) -> Self {
        let mut providers = BTreeMap::new();
        if surplus {
            providers.insert(
                CatalogProvider::Surplus,
                ProviderBinding {
                    identity: "surplus-credential".into(),
                },
            );
        }
        if openlux {
            providers.insert(
                CatalogProvider::Openlux,
                ProviderBinding {
                    identity: "openlux-discovery-credential".into(),
                },
            );
        }
        Self {
            store: store.clone(),
            publication: RefCell::new(CredentialPublication {
                providers,
                ..CredentialPublication::default()
            }),
            responses: RefCell::default(),
            requests: RefCell::default(),
            sequence: Cell::new(0),
        }
    }
    fn queue(&self, body: Value) {
        self.queue_source(source_catalog::json(&body));
    }
    fn queue_source(&self, body: SourceJson) {
        self.responses.borrow_mut().push_back(Fetch {
            body,
            status: 200,
            ticks: 0,
            after: None,
        });
    }
}
impl CatalogHost for Host {
    fn source_string_collator(&self) -> Arc<dyn gateway_schema::aliases::SourceStringCollator> {
        Arc::new(source_catalog::Collator::captured())
    }
    fn now_ms(&self) -> i64 {
        self.store.now.get()
    }
    fn unique_id(&self) -> Result<String, GatewayError> {
        let sequence = self.sequence.get() + 1;
        self.sequence.set(sequence);
        Ok(format!("observation-{sequence}"))
    }
    async fn wait(&self, milliseconds: u32) -> Result<(), GatewayError> {
        self.store
            .now
            .set(self.store.now.get() + i64::from(milliseconds));
        Ok(())
    }
    async fn publication(&self) -> Result<CredentialPublication, GatewayError> {
        Ok(self.publication.borrow().clone())
    }
    async fn fetch(&self, request: CatalogFetch) -> Result<CatalogFetchResponse, GatewayError> {
        self.requests.borrow_mut().push(request);
        let Some(response) = self.responses.borrow_mut().pop_front() else {
            panic!("unexpected provider discovery");
        };
        let mut ticks = response.ticks;
        poll_fn(|cx| {
            if ticks == 0 {
                Poll::Ready(())
            } else {
                ticks -= 1;
                cx.waker().wake_by_ref();
                Poll::Pending
            }
        })
        .await;
        if let Some(callback) = response.after {
            callback();
        }
        Ok(CatalogFetchResponse::json(
            response.status,
            response.body.as_str().to_owned(),
        ))
    }
}
fn surplus(id: &str) -> Value {
    json!({"data":[{"id":id,"supported_parameters":["tools","tool_choice"],"supported_features":["tools"],"pricing":{"prompt":"0.001","completion":"0.002"}}]})
}

struct Tokens(Cell<u64>);
impl gateway_authority::store::TokenSource for Tokens {
    fn next_token(&self) -> Result<String, GatewayError> {
        let n = self.0.get() + 1;
        self.0.set(n);
        Ok(format!("authority-{n}"))
    }
}
struct Clock(i64);
impl gateway_authority::store::AuthorityClock for Clock {
    fn now_ms(&self) -> Result<i64, GatewayError> {
        Ok(self.0)
    }
}
fn paid_request(
    store: &Store,
    provider: PaidProvider,
    model: &str,
) -> gateway_authority::paid_types::PaidRequest {
    use gateway_core::identity::{ApiKeyId, PaidAttemptId, RequestId};
    gateway_authority::paid_types::PaidRequest {
        key_id: ApiKeyId("fixture-key".into()),
        request_id: RequestId("fixture-request".into()),
        attempt_id: PaidAttemptId("fixture-attempt".into()),
        provider,
        model: model.into(),
        route: "responses".into(),
        path: "/v1/responses".into(),
        stream: false,
        reasoning: None,
        created_at_ms: store.now.get(),
    }
}
fn set_source_field(body: &mut SourceJson, key: &str, value: SourceJson) {
    let mut fields = gateway_core::source_serialization::entries(body).unwrap();
    let key = gateway_core::js_string::JsString16::from_utf8(key);
    if let Some((_, old)) = fields.iter_mut().find(|(name, _)| name == &key) {
        *old = value;
    } else {
        fields.push((key, value));
    }
    *body = gateway_core::source_serialization::object_keys(fields).unwrap();
}
fn context(model: &str) -> Result<PaidRequestContext, GatewayError> {
    Ok(PaidRequestContext {
        original_model: model.into(),
        body: SourceJson::from_serializable(
            &json!({"model":model,"input":"controlled fixture","stream":false,"tools":[]}),
        )?,
        surface: gateway_providers::types::ClientSurface::Responses,
        synthetic_hop: None,
    })
}
#[cfg(not(target_arch = "wasm32"))]
mod native_configuration {
    use super::*;
    use gateway_auth::{
        crypto::{Secret, base64url},
        native::NATIVE_RPC_PATH,
        native_auth::{
            AuthenticatedNativeOwner, NativeAuthHeaders, NativeAuthenticator, NativeCapability,
            NativeNonceAuthority, NativeTransportIdentity,
        },
    };
    use gateway_publication_policy::control_publication::{
        ConfiguredPaidInputs, PreparedConfigurationPublication, bootstrap_fixture_owner,
        fixture_owner_statement,
    };
    use std::collections::BTreeSet;

    struct Nonces<'a>(&'a Store);
    impl NativeNonceAuthority for Nonces<'_> {
        async fn consume_nonce(
            &self,
            key_id: &str,
            nonce: &str,
            valid_through_ms: i64,
            now_ms: i64,
        ) -> Result<(), GatewayError> {
            let key = state::source_key(&["uos_ai", "fixture_native_nonce", key_id, nonce]);
            if self.0.get(&key).is_some() {
                return Err(GatewayError::new(
                    401,
                    "Fixture native nonce replay",
                    Some("native_replay"),
                ));
            }
            let result = self
                .0
                .commit(AtomicTransition {
                    checks: vec![gateway_core::state::RevisionCheck {
                        key: key.clone(),
                        expected_revision: None,
                    }],
                    mutations: vec![RecordMutation::Put {
                        key,
                        record: StoredRecord::json(
                            format!("nonce-{nonce}"),
                            json!({"observed_at_ms":now_ms}),
                            Some(valid_through_ms + 1),
                        ),
                    }],
                })
                .await?;
            if result != CommitOutcome::Committed {
                return Err(GatewayError::new(
                    401,
                    "Fixture native nonce race",
                    Some("native_replay"),
                ));
            }
            Ok(())
        }
    }
    async fn proof(
        store: &Store,
        identity: &NativeTransportIdentity,
        body: &[u8],
        sequence: u8,
    ) -> AuthenticatedNativeOwner {
        let now = store.now.get();
        let nonce = base64url(&[sequence; 32]);
        let mac = identity.sign(NATIVE_RPC_PATH, now, &nonce, body).unwrap();
        let stamp = now.to_string();
        NativeAuthenticator::new(identity, &Nonces(store))
            .authenticate(
                NATIVE_RPC_PATH,
                NativeAuthHeaders {
                    version: "1",
                    key_id: identity.key_id(),
                    time_ms: &stamp,
                    nonce: &nonce,
                    mac: &mac,
                },
                body,
                now,
            )
            .await
            .unwrap()
    }
    fn configured_inputs(secret: &str) -> Result<ConfiguredPaidInputs, GatewayError> {
        ConfiguredPaidInputs::new(
            Some(Secret::new(secret.to_owned())),
            None,
            None,
            json!({"candidate":"controlled-fixture"}),
        )
    }
    #[test]
    fn actual_authenticated_owner_configuration_publisher_drives_the_catalog_bridge() -> TestResult
    {
        run(async {
            let store = Store::new();
            let template = Store::new();
            template.install(false, false);
            for key in [
                catalog_store::runtime_config_key(),
                catalog_store::codex_snapshot_key(),
                catalog_store::auth_generation_key(),
            ] {
                let row = template.get(&key).ok_or("missing template catalog row")?;
                store.put(key, &row.revision, row.value);
            }
            let identity = NativeTransportIdentity::new(
                "fixture-m11-key".into(),
                "fixture-m11-owner".into(),
                "fixture-m11-epoch".into(),
                [19; 32],
                BTreeSet::from([NativeCapability::ConfigurationPublish]),
            )?;
            let statement = fixture_owner_statement(&identity)?;
            let verified = proof(&store, &identity, &statement, 1).await;
            bootstrap_fixture_owner(
                &store,
                &identity,
                &verified,
                store.now.get(),
                "actual-owner",
            )
            .await?;
            let inputs = configured_inputs("fixture-actual-surplus-secret")?;
            let prepared = PreparedConfigurationPublication::capture(
                &store,
                &identity,
                &inputs,
                "configuration-1",
                store.now.get(),
            )
            .await?;
            let verified = proof(&store, &identity, prepared.statement(), 2).await;
            let config = prepared
                .publish(&store, &verified, store.now.get(), "actual-config")
                .await?;
            let host = Host::new(&store, false, false);
            host.publication.borrow_mut().providers.insert(
                CatalogProvider::Surplus,
                ProviderBinding {
                    identity: inputs
                        .binding(PaidProvider::Surplus)
                        .ok_or("missing fixture Surplus binding")?,
                },
            );
            assert_eq!(
                config.surplus_binding,
                inputs.binding(PaidProvider::Surplus)
            );
            let runtime = CatalogRuntime::new();
            host.queue(surplus("actual-owned-model"));
            publish_paid_routing(&runtime, &store, &host).await?;
            assert_eq!(
                store.projection().policy.models[&"actual-owned-model".into()],
                vec![PaidProvider::Surplus]
            );
            let retained = serde_json::to_string(&*store.rows.borrow())?;
            assert!(!retained.contains("fixture-actual-surplus-secret"));
            let next = configured_inputs("fixture-rotated-surplus-secret")?;
            let prepared = PreparedConfigurationPublication::capture(
                &store,
                &identity,
                &next,
                "configuration-2",
                store.now.get(),
            )
            .await?;
            let verified = proof(&store, &identity, prepared.statement(), 3).await;
            prepared
                .publish(&store, &verified, store.now.get(), "actual-config-2")
                .await?;
            assert!(
                store
                    .get(&keys::paid_control_observation(PaidProvider::Surplus))
                    .is_none()
            );
            assert!(publish_paid_routing(&runtime, &store, &host).await.is_err());
            host.publication.borrow_mut().providers.insert(
                CatalogProvider::Surplus,
                ProviderBinding {
                    identity: next
                        .binding(PaidProvider::Surplus)
                        .ok_or("missing rotated Surplus binding")?,
                },
            );
            host.queue(surplus("new-owned-model"));
            // Surplus discovery is public and shares its TTL cache across key changes.
            // A configuration rotation changes spend custody; an explicit metadata refresh
            // is the source operation that replaces a still-fresh public model snapshot.
            publish_paid_observation(&runtime, &store, &host, PaidProvider::Surplus, true).await?;
            publish_paid_routing(&runtime, &store, &host).await?;
            assert!(
                !store
                    .projection()
                    .policy
                    .models
                    .contains_key(&"actual-owned-model".into())
            );
            assert!(
                store
                    .projection()
                    .policy
                    .models
                    .contains_key(&"new-owned-model".into())
            );
            Ok(())
        })
    }
}

mod cases;
