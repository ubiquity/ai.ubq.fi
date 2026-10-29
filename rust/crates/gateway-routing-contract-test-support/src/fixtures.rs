use gateway_authority_keys as keys;
use gateway_contracts::{
    lifecycle::BodyOutcome, lifecycle::DeliveryOutcome, lifecycle::ReaderTermination,
    lifecycle::SemanticTerminal, lifecycle::TransportObservation,
};
use gateway_provider_policy::policy::HopFailure;
use gateway_routing_plan::input::RoutingInput;
use gateway_routing_plan::input::RoutingResult;
use gateway_routing_plan::policy;
use gateway_routing_runtime::{
    session::RoutingSession, types::AttemptFailure, types::PreparedAttempt, types::RoutingHost,
    types::TransportState,
};
use gateway_routing_runtime_test_support::{
    exec::run, host::Host, input::NOW, input::input, store::Store,
};
use gateway_source::source_serialization as source;
use gateway_source::{js_string::JsString16, source_json::SourceJson};
use serde_json::{Value, json};
use std::collections::BTreeMap;

pub type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;
pub type TestSession = RoutingSession<Store, Store, Store, Host>;

pub async fn next_attempt(session: &TestSession) -> TestResult<PreparedAttempt> {
    let attempt = session.next_attempt().await?;
    attempt.ok_or_else(|| "missing prepared attempt".into())
}
pub fn fixture(value: &Value) -> SourceJson {
    SourceJson::from_serializable(value).unwrap()
}
pub fn set_source(raw: &mut SourceJson, key: &str, value: Value) {
    let mut fields = source::entries(raw).unwrap();
    let key = JsString16::from_utf8(key);
    if let Some((_, old)) = fields.iter_mut().find(|(name, _)| name == &key) {
        *old = fixture(&value);
    } else {
        fields.push((key, fixture(&value)));
    }
    *raw = source::object_keys(fields).unwrap();
}
pub fn source_plan(input: &RoutingInput) -> RoutingResult<policy::RoutePlan> {
    let collator = Host::new(&Store::new()).source_string_collator();
    policy::plan(input, NOW, collator.as_ref())
}
pub fn paid_input(store: &Store, id: &str, model: &str) -> TestResult<RoutingInput> {
    use gateway_authority_paid_control_records::*;
    use gateway_contracts::provider::PaidProvider;
    use gateway_core::state::AuthorityStore;
    let mut request = input(id, model, &["surplus", "openlux"]);
    let models = ["paid-model", "deepseek-v4.1-flash"]
        .map(|id| {
            fixture(&json!({
                "id":id,"supported_endpoint_types":["openai-response","openai"],
                "supports_tools":true,"input_price_per_token":0.001,"output_price_per_token":0.002,
            }))
        })
        .to_vec();
    for provider in ["surplus", "openlux"] {
        request
            .catalog
            .providers
            .get_mut(provider)
            .ok_or("missing paid provider fixture")?
            .models = models.clone();
    }
    store.seed(
        keys::paid_control_owner(),
        PaidControlOwner {
            v: 1,
            owner_id: "fixture-owner".into(),
            owner_epoch: "fixture-epoch".into(),
            key_fingerprint: "fixture-key".into(),
        },
    );
    store.seed(
        keys::paid_control_config(),
        PaidControlConfig {
            v: PAID_CONTROL_CONFIG_VERSION,
            owner_id: "fixture-owner".into(),
            owner_epoch: "fixture-epoch".into(),
            generation: "fixture-generation".into(),
            configuration_fingerprint: "fixture-config".into(),
            surplus_binding: Some("fixture-surplus".into()),
            openlux_discovery_binding: Some("fixture-openlux".into()),
            openlux_binding: Some("fixture-openlux".into()),
            source_checks: vec![],
        },
    );
    for provider in [PaidProvider::Surplus, PaidProvider::Metered] {
        store.seed(
            keys::paid_control_observation(provider),
            PaidProviderObservation {
                v: 1,
                provider,
                owner_id: "fixture-owner".into(),
                owner_epoch: "fixture-epoch".into(),
                configuration_generation: "fixture-generation".into(),
                configuration_fingerprint: "fixture-config".into(),
                credential_binding: if provider == PaidProvider::Surplus {
                    "fixture-surplus"
                } else {
                    "fixture-openlux"
                }
                .into(),
                observed_at_ms: NOW,
                source_valid_until_ms: None,
                stale: false,
                models: models.clone(),
                source_checks: vec![],
            },
        );
    }
    let revision = |key| run(store.read(&key)).map(|row| row.map(|row| row.revision));
    store.seed(
        keys::paid_routing(),
        PaidRoutingProjection {
            policy: PaidRoutingPolicy {
                v: 2,
                codex_enabled: true,
                codex_models: vec!["gpt-test".into()],
                models: ["paid-model", "deepseek-v4.1-flash"]
                    .map(|model| {
                        (
                            model.into(),
                            vec![PaidProvider::Surplus, PaidProvider::Metered],
                        )
                    })
                    .into_iter()
                    .collect(),
            },
            provenance: PaidProjectionProvenance {
                v: 1,
                owner_revision: revision(keys::paid_control_owner())?
                    .ok_or("missing paid owner revision")?,
                config_revision: revision(keys::paid_control_config())?
                    .ok_or("missing paid config revision")?,
                surplus_observation_revision: revision(keys::paid_control_observation(
                    PaidProvider::Surplus,
                ))?,
                openlux_observation_revision: revision(keys::paid_control_observation(
                    PaidProvider::Metered,
                ))?,
                observed_at_ms: NOW,
                source_valid_until_ms: None,
                source_checks: gateway_authority_paid_evidence::paid_projection_source_keys()
                    .into_iter()
                    .map(|key| {
                        revision(key.clone()).map(|expected_revision| PaidSourceCheck {
                            expected_revision,
                            key: key.0,
                        })
                    })
                    .collect::<Result<_, _>>()?,
            },
        },
    );
    Ok(request)
}
pub fn refusal(status: u16, transport: TransportState) -> AttemptFailure {
    AttemptFailure {
        origin: if status == 499 {
            gateway_routing_runtime::types::FailureOrigin::Transport
        } else {
            gateway_routing_runtime::types::FailureOrigin::Provider
        },
        status: Some(status),
        headers: BTreeMap::new(),
        body: vec![],
        body_complete: true,
        failure: HopFailure::Status(status),
        transport,
        semantic_committed: false,
        cancelled: false,
    }
}
pub fn complete() -> TransportObservation {
    TransportObservation {
        semantic_terminal: Some(SemanticTerminal::Completed),
        reader_termination: Some(ReaderTermination::Eof),
        body: Some(BodyOutcome::Drained),
        delivery: DeliveryOutcome::Unobserved,
    }
}
