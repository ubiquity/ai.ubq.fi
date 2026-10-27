//! Synthetic source-control fixtures; all production grants still use real public checked operations.
use gateway_authority_keys as keys;
use gateway_authority_paid_control_records::PaidRoutingProjection;
use gateway_authority_paid_control_records::{PAID_CONTROL_CONFIG_VERSION, PaidSourceCheck};
use gateway_authority_paid_control_records::{PaidControlConfig, PaidControlOwner};
use gateway_authority_paid_control_records::{PaidProjectionProvenance, PaidProviderObservation};
use gateway_authority_paid_control_records::{PaidRequestContext, PaidRoutingPolicy};
use gateway_authority_paid_evidence::paid_projection_source_keys;
use gateway_authority_paid_records::PaidRequest;
use gateway_core::{provider::PaidProvider, state::AtomicTransition, state::AuthorityStore};
use gateway_core::{state::RecordKey, state::RecordMutation, state::RevisionCheck};
use gateway_providers::types::ClientSurface;
use gateway_routing_runtime_test_support::authority::NOW;
use gateway_routing_runtime_test_support::{authority::MemoryStore, authority::run};
use serde_json::{Value, json};

pub fn seed_source<T: serde::Serialize + ?Sized>(store: &MemoryStore, key: RecordKey, value: &T) {
    static SEQUENCE: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);
    let revision = format!(
        "source-fixture-{}",
        SEQUENCE.fetch_add(1, std::sync::atomic::Ordering::SeqCst)
    );
    let raw = gateway_core::source_json::SourceJson::from_serializable(value).unwrap();
    store.seed_record(
        key,
        gateway_core::source_control::encode_source_control_record(revision, &raw, None).unwrap(),
    );
}
pub fn set_body(body: &mut gateway_core::source_json::SourceJson, name: &str, value: Value) {
    let mut fields = gateway_core::source_serialization::entries(body).unwrap();
    let name = gateway_core::js_string::JsString16::from_utf8(name);
    let value = gateway_core::source_json::SourceJson::from_serializable(&value).unwrap();
    if let Some((_, old)) = fields.iter_mut().find(|(key, _)| key == &name) {
        *old = value;
    } else {
        fields.push((name, value));
    }
    *body = gateway_core::source_serialization::object_keys(fields).unwrap();
}
pub fn context(input: &PaidRequest) -> PaidRequestContext {
    let mut fields = vec![
        ("model", input.model.to_source_json().unwrap()),
        (
            "stream",
            gateway_core::source_json::SourceJson::from_serializable(&input.stream).unwrap(),
        ),
    ];
    if let Some(effort) = &input.reasoning {
        fields.push((
            "reasoning",
            gateway_core::source_serialization::object(vec![(
                "effort",
                effort.to_source_json().unwrap(),
            )])
            .unwrap(),
        ));
    }
    let body = gateway_core::source_serialization::object(fields).unwrap();
    PaidRequestContext {
        original_model: input.model.clone(),
        body,
        surface: if input.path == "/v1/chat/completions" {
            ClientSurface::Chat
        } else {
            ClientSurface::Responses
        },
        synthetic_hop: None,
    }
}

pub fn revision(store: &MemoryStore, key: &RecordKey) -> Option<String> {
    store.row(key).map(|row| row.revision)
}

pub fn republish(store: &MemoryStore) {
    let row = store.row(&keys::paid_routing()).unwrap();
    let mut projection: PaidRoutingProjection =
        gateway_core::source_control::decode_source_control_record(&row)
            .unwrap()
            .deserialize()
            .unwrap();
    projection.provenance.owner_revision = revision(store, &keys::paid_control_owner()).unwrap();
    projection.provenance.config_revision = revision(store, &keys::paid_control_config()).unwrap();
    projection.provenance.surplus_observation_revision = revision(
        store,
        &keys::paid_control_observation(PaidProvider::Surplus),
    );
    projection.provenance.openlux_observation_revision = revision(
        store,
        &keys::paid_control_observation(PaidProvider::Metered),
    );
    projection.provenance.source_checks = paid_projection_source_keys()
        .into_iter()
        .map(|key| PaidSourceCheck {
            expected_revision: revision(store, &key),
            key: key.0,
        })
        .collect();
    super::paid_control_cases::seed_source(store, keys::paid_routing(), &projection);
}

pub fn seed_controls(store: &MemoryStore) {
    // Dedicated deterministic unit fixture, not a live owner/config installation.
    store.seed(
        keys::paid_control_owner(),
        &PaidControlOwner {
            v: 1,
            owner_id: "fixture-owner".into(),
            owner_epoch: "fixture-epoch".into(),
            key_fingerprint: "fixture-authenticated-key".into(),
        },
    );
    store.seed(
        keys::paid_control_config(),
        &PaidControlConfig {
            v: PAID_CONTROL_CONFIG_VERSION,
            owner_id: "fixture-owner".into(),
            owner_epoch: "fixture-epoch".into(),
            generation: "fixture-config-generation".into(),
            configuration_fingerprint: "fixture-config-fingerprint".into(),
            surplus_binding: Some("fixture-surplus-binding".into()),
            openlux_discovery_binding: Some("fixture-openlux-discovery-binding".into()),
            openlux_binding: Some("fixture-openlux-binding".into()),
            source_checks: vec![],
        },
    );
    let models:Vec<_>=["paid-model","gpt-test","gpt-5.6-sol","deepseek-v4.1-flash"].into_iter().map(|id|gateway_core::source_json::SourceJson::from_serializable(&json!({"id":id,"supported_endpoint_types":["openai-response","openai"],"supports_tools":true,"input_price_per_token":0.001,"output_price_per_token":0.002})).unwrap()).collect();
    for provider in [PaidProvider::Surplus, PaidProvider::Metered] {
        store.seed(
            keys::paid_control_observation(provider),
            &PaidProviderObservation {
                v: 1,
                provider,
                owner_id: "fixture-owner".into(),
                owner_epoch: "fixture-epoch".into(),
                configuration_generation: "fixture-config-generation".into(),
                configuration_fingerprint: "fixture-config-fingerprint".into(),
                credential_binding: if provider == PaidProvider::Surplus {
                    "fixture-surplus-binding"
                } else {
                    "fixture-openlux-discovery-binding"
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
    let projection = PaidRoutingProjection {
        policy: PaidRoutingPolicy {
            v: 2,
            codex_enabled: true,
            codex_models: vec!["gpt-test".into(), "gpt-5.6-sol".into()],
            models: [
                "paid-model",
                "gpt-test",
                "gpt-5.6-sol",
                "deepseek-v4.1-flash",
            ]
            .into_iter()
            .map(|id| {
                (
                    id.into(),
                    vec![PaidProvider::Surplus, PaidProvider::Metered],
                )
            })
            .collect(),
        },
        provenance: PaidProjectionProvenance {
            v: 1,
            owner_revision: String::new(),
            config_revision: String::new(),
            surplus_observation_revision: None,
            openlux_observation_revision: None,
            observed_at_ms: NOW,
            source_valid_until_ms: None,
            source_checks: vec![],
        },
    };
    super::paid_control_cases::seed_source(store, keys::paid_routing(), &projection);
    republish(store);
}

pub fn remove(store: &MemoryStore, key: RecordKey) {
    run(store.commit(AtomicTransition {
        checks: vec![RevisionCheck {
            expected_revision: revision(store, &key),
            key: key.clone(),
        }],
        mutations: vec![RecordMutation::Delete { key }],
    }))
    .unwrap();
}

pub fn no_attempt(store: &MemoryStore, input: &PaidRequest) {
    assert!(
        store
            .row(&keys::paid_attempt(&input.key_id.0, &input.attempt_id.0))
            .is_none()
    );
    assert!(store.row(&keys::paid_gate()).is_none());
}
