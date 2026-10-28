//! Fake-only paid boundary derived from source_contracts.rs:63-171,321-373 at 08431133.
use gateway_authority_keys as keys;
use gateway_authority_paid_control_records::{
    PAID_CONTROL_CONFIG_VERSION, PaidControlConfig, PaidControlOwner, PaidProjectionProvenance,
    PaidProviderObservation, PaidRoutingProjection, PaidSourceCheck, paid_projection_source_keys,
};
use gateway_authority_paid_records::PaidRoutingPolicy;
use gateway_contracts::{
    lifecycle::TransportObservation, provider::PaidProvider, provider::UpstreamProvider,
};
use gateway_core::state::AuthorityStore;
use gateway_provider_policy::{policy::HopFailure, policy::paid_hop_identity};
use gateway_routing_runtime::{
    session::RoutingSession,
    types::{AttemptFailure, FailureOrigin, RefusalDecision, RoutingInput, TransportState},
};
use gateway_routing_runtime_test_support::{
    exec::run,
    host::Host,
    input::{NOW, input},
    seed::{seed_key, window},
    store::Store,
};
use gateway_source::source_json::SourceJson;
use serde_json::json;
use std::collections::BTreeMap;

type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;

/// Preserve the existing fake current owner/config/catalog/observation provenance.
fn paid_input(store: &Store, id: &str) -> TestResult<RoutingInput> {
    let mut request = input(id, "ubiquity/deepseek-flash", &["surplus", "openlux"]);
    request.raw = SourceJson::from_serializable(&json!({
        "model":"ubiquity/deepseek-flash", "input":"source fixture",
        "reasoning":{"effort":"high"}, "stream":true
    }))?;
    let models = ["paid-model", "deepseek-v4.1-flash"]
        .map(|id| {
            SourceJson::from_serializable(&json!({
                "id":id,"supported_endpoint_types":["openai-response","openai"],
                "supports_tools":true,"input_price_per_token":0.001,"output_price_per_token":0.002
            }))
        })
        .into_iter()
        .collect::<Result<Vec<_>, _>>()?;
    for provider in ["surplus", "openlux"] {
        request
            .catalog
            .providers
            .get_mut(provider)
            .ok_or("missing provider")?
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
                owner_revision: revision(keys::paid_control_owner())?.ok_or("missing owner")?,
                config_revision: revision(keys::paid_control_config())?.ok_or("missing config")?,
                surplus_observation_revision: revision(keys::paid_control_observation(
                    PaidProvider::Surplus,
                ))?,
                openlux_observation_revision: revision(keys::paid_control_observation(
                    PaidProvider::Metered,
                ))?,
                observed_at_ms: NOW,
                source_valid_until_ms: None,
                source_checks: paid_projection_source_keys()
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

fn failure(kind: HopFailure) -> AttemptFailure {
    let status = match kind {
        HopFailure::Status(status) => Some(status),
        _ => None,
    };
    AttemptFailure {
        origin: if status.is_some() {
            FailureOrigin::Provider
        } else {
            FailureOrigin::Transport
        },
        status,
        headers: BTreeMap::new(),
        body: vec![],
        body_complete: status.is_some(),
        failure: kind,
        transport: TransportState::Started,
        semantic_committed: false,
        cancelled: false,
    }
}

fn exercise(kind: HopFailure) -> TestResult {
    run(async {
        let store = Store::new();
        seed_key(&store);
        let id = "synthetic-paid-boundary";
        let request = paid_input(&store, id)?;
        let session = RoutingSession::begin(
            request,
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await?;
        let first = session
            .next_attempt()
            .await?
            .ok_or("missing Surplus attempt")?;
        assert_eq!(first.request.provider(), UpstreamProvider::Surplus);
        let reservation = first.paid.as_ref().ok_or("missing Surplus reservation")?;
        assert_eq!(
            reservation.attempt_id.0,
            paid_hop_identity(id, PaidProvider::Surplus)
        );
        session.before_dispatch(&first).await?;
        session.transport_started(&first).await?;
        let first_key = keys::paid_attempt(&reservation.key_id.0, &reservation.attempt_id.0);
        let second_key = keys::paid_attempt(
            &reservation.key_id.0,
            &paid_hop_identity(id, PaidProvider::Metered),
        );
        let window_key = keys::paid_window(&reservation.key_id.0, reservation.window_reset_at_ms);
        let retained = store.value(first_key.clone());
        let exposure = store.value(window_key.clone());
        assert_eq!(retained["transport_started"], true);
        assert!(retained["spend_microcredits"].is_null());
        assert_eq!(
            exposure["reserved_microcredits"],
            reservation.reserved_microcredits
        );
        assert!(store.value(second_key.clone()).is_null());
        let advances = matches!(kind, HopFailure::Status(402 | 429));
        let expected = if advances {
            RefusalDecision::Continue
        } else {
            RefusalDecision::Return
        };
        assert_eq!(
            session.refusal(&first, failure(kind)).await?,
            expected,
            "{kind:?}"
        );
        if advances {
            let second = session
                .next_attempt()
                .await?
                .ok_or("missing OpenLux attempt")?;
            assert_eq!(second.request.provider(), UpstreamProvider::Metered);
            let paid = second.paid.as_ref().ok_or("missing OpenLux reservation")?;
            assert_eq!(
                paid.attempt_id.0,
                paid_hop_identity(id, PaidProvider::Metered)
            );
            assert_ne!(reservation.attempt_id, paid.attempt_id);
            assert_eq!(first.original_request_id, second.original_request_id);
            session.before_dispatch(&second).await?;
            let mut cancelled = failure(HopFailure::Cancelled);
            cancelled.cancelled = true;
            cancelled.transport = TransportState::NotStarted;
            assert_eq!(
                session.refusal(&second, cancelled).await?,
                RefusalDecision::Abort
            );
        } else {
            assert!(
                !matches!(session.next_attempt().await, Ok(Some(_))),
                "No second dispatch capability may be prepared"
            );
            assert!(store.value(second_key.clone()).is_null());
            let pending = store.value(first_key.clone());
            assert_eq!(pending["reservation"], retained["reservation"]);
            assert_eq!(pending["transport_started"], true);
            assert!(pending["spend_microcredits"].is_null());
            let current = store.value(window_key.clone());
            assert_eq!(
                current["reserved_microcredits"],
                exposure["reserved_microcredits"]
            );
            assert_eq!(current["pending_count"], exposure["pending_count"]);
        }
        session.finish(TransportObservation::default()).await?;
        assert_eq!(window(&store)["committed_requests"], 1);
        assert_eq!(window(&store)["reserved_requests"], 0);
        if !advances {
            assert!(store.value(second_key).is_null());
            assert!(store.value(first_key)["spend_microcredits"].is_null());
            assert_eq!(
                store.value(window_key)["reserved_microcredits"],
                exposure["reserved_microcredits"]
            );
        }
        Ok(())
    })
}

#[test]
fn authoritative_402_keeps_paid_continuation() -> TestResult {
    exercise(HopFailure::Status(402))
}
#[test]
fn authoritative_429_keeps_paid_continuation() -> TestResult {
    exercise(HopFailure::Status(429))
}
#[test]
fn server_503_cannot_admit_another_paid_tier() -> TestResult {
    exercise(HopFailure::Status(503))
}
#[test]
fn unclassified_403_cannot_admit_another_paid_tier() -> TestResult {
    exercise(HopFailure::Status(403))
}
#[test]
fn transport_error_cannot_admit_another_paid_tier() -> TestResult {
    exercise(HopFailure::Transport)
}
#[test]
fn presemantic_stream_error_cannot_admit_another_paid_tier() -> TestResult {
    exercise(HopFailure::StreamBeforeSemantic)
}
