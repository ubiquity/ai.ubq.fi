use gateway_authority_contracts::REQUEST_LEASE_MS;
use gateway_authority_routing_records::slot::class_name;
use gateway_contracts::{
    lifecycle::DeliveryOutcome, lifecycle::TransportObservation, model::ModelId,
    provider::UpstreamProvider,
};
use gateway_local_refusal::routing_error::SourceRefusalBody;
use gateway_model_selection::selection as model_selection;
use gateway_provider_policy::policy::HopFailure;
use gateway_routing_classification::classify;
use gateway_routing_contract_test_support::{
    fixtures::TestResult, fixtures::complete, fixtures::fixture, fixtures::next_attempt,
    fixtures::refusal, fixtures::set_source, fixtures::source_plan,
};
use gateway_routing_runtime::{
    policy, policy::class, session::RoutingSession, settlement::recover_pending,
    types::AttemptUsage, types::PreparedWireRequest, types::RefusalDecision, types::RoutingHost,
    types::TransportState,
};
use gateway_routing_runtime_test_support::{
    exec::run, host::Host, input::NOW, input::input, seed::seed_key, seed::seed_pool,
    seed::source_account, seed::source_key, seed::window, store::Store,
};
use gateway_source::source_serialization as source;
use serde_json::{Value, json};
use std::{collections::BTreeMap, sync::atomic::Ordering};

#[test]
fn fixture_collator_refuses_unproven_operands_even_when_equal() {
    let collator = Host::new(&Store::new()).source_string_collator();
    assert_eq!(
        collator
            .compare(&"unproven-model".into(), &"unproven-model".into())
            .unwrap_err()
            .envelope
            .error
            .code
            .as_deref(),
        Some("source_catalog_oracle_unknown_pair")
    );
}
#[test]
fn source_availability_gate_preserves_full_compact_paid_and_reserve_cases() -> TestResult {
    let mut request = input("model-gate", "unknown-model", &[]);
    assert!(source_plan(&request).is_err_and(|error| error.error.status == 404));
    request.catalog.snapshot = request.catalog.full_snapshot.take();
    assert!(source_plan(&request).is_err_and(|error| error.error.status == 404));
    request
        .catalog
        .snapshot
        .as_mut()
        .ok_or("missing catalog snapshot")?
        .models
        .clear();
    assert!(source_plan(&request).is_ok());
    request.catalog.snapshot = None;
    assert!(source_plan(&request).is_ok());
    let mut reserve = input("reserve-model", "gpt-reserve", &["surplus"]);
    reserve
        .catalog
        .providers
        .get_mut("surplus")
        .ok_or("missing fixture value")?
        .models = vec![fixture(
        &json!({"id":"gpt-reserve","supported_endpoint_types":["openai-response"],"input_price_per_token":0.001,"output_price_per_token":0.002}),
    )];
    let plan = source_plan(&reserve)?;
    assert!(!plan.paid.metered_only);
    assert!(matches!(plan.hops.get(1), Some(policy::RouteHop::Codex)));
    let mut paid = input("paid-endpoint", "paid-model", &["surplus"]);
    paid.catalog
        .providers
        .get_mut("surplus")
        .ok_or("missing fixture value")?
        .models = vec![fixture(
        &json!({"id":"paid-model","supported_endpoint_types":["openai"],"input_price_per_token":0.001,"output_price_per_token":0.002}),
    )];
    assert!(source_plan(&paid).is_err_and(|error| error.error.status == 404));
    paid.surface = gateway_provider_protocol::types::ClientSurface::Chat;
    paid.raw = fixture(&json!({
        "model":"paid-model",
        "messages":[{"role":"user","content":"source fixture"}],
        "reasoning_effort":"none"
    }));
    assert!(source_plan(&paid).is_ok());
    Ok(())
}
#[test]
fn frozen_source_classifier_quota_classes_and_fallback_matrix() {
    let golden: Value = serde_json::from_str(include_str!("fixtures/source-golden.json")).unwrap();
    assert_eq!(golden["classes"].as_array().unwrap().len(), 9);
    assert_eq!(golden["quota"].as_array().unwrap().len(), 16);
    for case in golden["classes"].as_array().unwrap() {
        assert_eq!(
            class_name(class(&ModelId::from(case["model"].as_str().unwrap_or("")))),
            case["expected"].as_str().unwrap()
        );
    }
    for case in golden["statuses"].as_array().unwrap() {
        let status = case["status"].as_u64().unwrap() as u16;
        let actual =
            gateway_provider_policy::policy::deepseek_decision(HopFailure::Status(status), false)
                == gateway_provider_policy::policy::HopDecision::Advance;
        assert_eq!(actual, case["expected"] == true);
    }
    for case in golden["quota"].as_array().unwrap() {
        let headers = case["retry"]
            .as_str()
            .map(|r| BTreeMap::from([("retry-after".into(), r.into())]))
            .unwrap_or_default();
        let bytes: Vec<u8> = case["bytes"]
            .as_array()
            .map(|v| v.iter().map(|b| b.as_u64().unwrap() as u8).collect())
            .unwrap_or_else(|| case["body"].as_str().unwrap().as_bytes().to_vec());
        let actual = classify::codex_quota(
            Some(case["status"].as_u64().unwrap() as u16),
            &headers,
            &bytes,
            case["complete"] != false,
            case["now"].as_i64().unwrap(),
        );
        if case["expected"].is_null() {
            assert!(actual.is_none(), "{}", case["name"]);
        } else {
            let actual = actual.unwrap();
            assert_eq!(
                actual.blocked_until_ms,
                case["expected"]["retryAtMs"].as_i64().unwrap(),
                "{}",
                case["name"]
            );
            assert_eq!(
                actual.reset_is_stable,
                case["expected"]["resetDeadlineIsStable"] == true
            );
            assert_eq!(
                actual.reset_ambiguous,
                case["expected"]["resetDeadlineConflict"] == true
            );
        }
    }
}
#[test]
fn direct_dispatch_and_finish_count_api_once() {
    run(async {
        let store = Store::new();
        seed_key(&store);
        let session = RoutingSession::begin(
            input("once", "deepseek-flash", &["deepseek"]),
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await
        .unwrap();
        let attempt = session.next_attempt().await.unwrap().unwrap();
        assert_eq!(attempt.request.provider(), UpstreamProvider::Deepseek);
        session.before_dispatch(&attempt).await.unwrap();
        assert!(session.before_dispatch(&attempt).await.is_err());
        session.transport_started(&attempt).await.unwrap();
        session
            .observe_terminal(
                &attempt,
                complete(),
                AttemptUsage {
                    status: Some(200),
                    ..Default::default()
                },
            )
            .await
            .unwrap();
        session.finish(complete()).await.unwrap();
        session.finish(complete()).await.unwrap();
        assert_eq!(window(&store)["committed_requests"], 1);
        assert_eq!(window(&store)["reserved_requests"], 0);
    })
}
#[test]
fn explicit_not_started_abort_compensates_but_ambiguous_intent_does_not() {
    run(async {
        let store = Store::new();
        seed_key(&store);
        let session = RoutingSession::begin(
            input("cancel-safe", "deepseek-flash", &["deepseek"]),
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await
        .unwrap();
        let attempt = session.next_attempt().await.unwrap().unwrap();
        session.before_dispatch(&attempt).await.unwrap();
        let mut failure = refusal(499, TransportState::NotStarted);
        failure.cancelled = true;
        assert_eq!(
            session.refusal(&attempt, failure).await.unwrap(),
            RefusalDecision::Abort
        );
        session
            .finish(TransportObservation::default())
            .await
            .unwrap();
        assert_eq!(window(&store)["committed_requests"], 0);
        let other = RoutingSession::begin(
            input("cancel-unknown", "deepseek-flash", &["deepseek"]),
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await
        .unwrap();
        let attempt = other.next_attempt().await.unwrap().unwrap();
        other.before_dispatch(&attempt).await.unwrap();
        other.finish(TransportObservation::default()).await.unwrap();
        assert_eq!(window(&store)["committed_requests"], 1);
    })
}
#[test]
fn started_transport_cannot_be_refunded_by_client_cancellation() {
    run(async {
        let store = Store::new();
        seed_key(&store);
        let session = RoutingSession::begin(
            input("started", "deepseek-flash", &["deepseek"]),
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await
        .unwrap();
        let attempt = session.next_attempt().await.unwrap().unwrap();
        session.before_dispatch(&attempt).await.unwrap();
        session.transport_started(&attempt).await.unwrap();
        let mut failure = refusal(499, TransportState::Started);
        failure.cancelled = true;
        assert_eq!(
            session.refusal(&attempt, failure).await.unwrap(),
            RefusalDecision::Abort
        );
        session
            .finish(TransportObservation {
                delivery: DeliveryOutcome::Interrupted,
                ..Default::default()
            })
            .await
            .unwrap();
        assert_eq!(window(&store)["committed_requests"], 1);
    })
}
#[test]
fn synthetic_hops_share_original_admission_and_stop_after_semantic_commit() {
    run(async {
        let store = Store::new();
        seed_key(&store);
        let mut request = input(
            "synthetic",
            "ubiquity/deepseek-flash",
            &["lithos", "deepseek"],
        );
        set_source(&mut request.raw, "reasoning", json!({"effort":"high"}));
        let session = RoutingSession::begin(
            request,
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await
        .unwrap();
        let first = session.next_attempt().await.unwrap().unwrap();
        assert_eq!(first.request.provider(), UpstreamProvider::Lithos);
        session.before_dispatch(&first).await.unwrap();
        session.transport_started(&first).await.unwrap();
        assert_eq!(
            session
                .refusal(&first, refusal(503, TransportState::Started))
                .await
                .unwrap(),
            RefusalDecision::Continue
        );
        let second = session.next_attempt().await.unwrap().unwrap();
        assert_eq!(second.request.provider(), UpstreamProvider::Deepseek);
        assert_eq!(first.original_request_id, second.original_request_id);
        session.before_dispatch(&second).await.unwrap();
        session.transport_started(&second).await.unwrap();
        session
            .observe_terminal(&second, complete(), AttemptUsage::default())
            .await
            .unwrap();
        assert_eq!(
            session
                .refusal(&second, refusal(503, TransportState::Started))
                .await
                .unwrap(),
            RefusalDecision::Return
        );
        session.finish(complete()).await.unwrap();
        assert_eq!(window(&store)["committed_requests"], 1);
    })
}
#[test]
fn codex_none_and_ultra_keep_source_wire_translation_and_one_conversation() {
    run(async {
        let store = Store::new();
        seed_key(&store);
        seed_pool(&store);
        let mut request = input("codex", "gpt-test", &[]);
        set_source(&mut request.raw, "reasoning", json!({"effort":"ultra"}));
        let session = RoutingSession::begin(
            request,
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await
        .unwrap();
        let first = session.next_attempt().await.unwrap().unwrap();
        assert_eq!(
            source::member(first.request.body(), "reasoning")
                .and_then(|reasoning| source::member(&reasoning, "effort"))
                .and_then(|value| source::string(&value)),
            Some("max".into())
        );
        let PreparedWireRequest::Ready(first_ready) = &first.request else {
            panic!("expected scalar Ready")
        };
        let conversation = first_ready.headers["conversation_id"].clone();
        session.before_dispatch(&first).await.unwrap();
        session.transport_started(&first).await.unwrap();
        assert_eq!(
            session
                .refusal(&first, refusal(401, TransportState::Started))
                .await
                .unwrap(),
            RefusalDecision::Continue
        );
        let second = session.next_attempt().await.unwrap().unwrap();
        let PreparedWireRequest::Ready(second_ready) = &second.request else {
            panic!("expected scalar Ready")
        };
        assert_eq!(second_ready.headers["conversation_id"], conversation);
        assert_ne!(first.credential, second.credential);
        session.before_dispatch(&second).await.unwrap();
        session.transport_started(&second).await.unwrap();
        session
            .observe_terminal(&second, complete(), AttemptUsage::default())
            .await
            .unwrap();
        session.finish(complete()).await.unwrap();
        assert_eq!(window(&store)["committed_requests"], 1);
    })
}
#[test]
fn codex_generic429_is_not_paid_or_account_exhaustion() {
    run(async {
        let store = Store::new();
        seed_key(&store);
        seed_pool(&store);
        let session = RoutingSession::begin(
            input("generic429", "gpt-test", &[]),
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await
        .unwrap();
        let attempt = session.next_attempt().await.unwrap().unwrap();
        session.before_dispatch(&attempt).await.unwrap();
        session.transport_started(&attempt).await.unwrap();
        let mut failure = refusal(429, TransportState::Started);
        failure.body = br#"{"error":{"type":"rate_limit_error"}}"#.to_vec();
        failure.headers.insert("retry-after".into(), "60".into());
        assert_eq!(
            session.refusal(&attempt, failure).await.unwrap(),
            RefusalDecision::Return
        );
        let active = store.value(gateway_authority_keys::active());
        assert_eq!(
            active["account_id_hash"],
            source_account("a").identity().routing_account_hash.0
        );
        session
            .finish(TransportObservation::default())
            .await
            .unwrap();
    })
}
#[test]
fn credential_generation_change_prevents_dispatch_and_allows_only_proven_compensation() {
    run(async {
        let store = Store::new();
        seed_key(&store);
        seed_pool(&store);
        let session = RoutingSession::begin(
            input("changed", "gpt-test", &[]),
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await
        .unwrap();
        let attempt = session.next_attempt().await.unwrap().unwrap();
        store.seed(
            source_key(),
            json!({"fixture_owner":"native","generation":2}),
        );
        assert!(session.before_dispatch(&attempt).await.is_err());
        let mut failure = refusal(499, TransportState::NotStarted);
        failure.cancelled = true;
        session.refusal(&attempt, failure).await.unwrap();
        session
            .finish(TransportObservation::default())
            .await
            .unwrap();
        assert_eq!(window(&store)["committed_requests"], 0);
    })
}
#[test]
fn expired_journal_recovery_never_replays_or_refunds_started_work() {
    run(async {
        let store = Store::new();
        seed_key(&store);
        let host = Host::new(&store);
        let session = RoutingSession::begin(
            input("restart", "deepseek-flash", &["deepseek"]),
            store.clone(),
            host.clone(),
            store.clone(),
            store.clone(),
        )
        .await
        .unwrap();
        let attempt = session.next_attempt().await.unwrap().unwrap();
        session.before_dispatch(&attempt).await.unwrap();
        session.transport_started(&attempt).await.unwrap();
        store.advance(REQUEST_LEASE_MS + 1);
        let recovered = recover_pending(
            store.clone(),
            host.clone(),
            store.clone(),
            store.clone(),
            100,
        )
        .await
        .unwrap();
        assert_eq!(recovered.len(), 1);
        assert_eq!(recovered[0].observation.semantic_terminal, None);
        assert_eq!(
            recovered[0].observation.delivery,
            DeliveryOutcome::Unobserved
        );
        assert!(
            recover_pending(store.clone(), host, store.clone(), store.clone(), 100)
                .await
                .unwrap()
                .is_empty()
        );
        assert_eq!(window(&store)["committed_requests"], 1);
    })
}
#[test]
fn simultaneous_original_request_identity_has_one_winner() {
    let store = Store::new();
    seed_key(&store);
    let workers: Vec<_> = (0..2)
        .map(|_| {
            let store = store.clone();
            std::thread::spawn(move || {
                run(RoutingSession::begin(
                    input("race", "deepseek-flash", &["deepseek"]),
                    store.clone(),
                    Host::new(&store),
                    store.clone(),
                    store,
                ))
                .is_ok()
            })
        })
        .collect();
    assert_eq!(
        workers
            .into_iter()
            .map(|worker| worker.join().unwrap())
            .filter(|won| *won)
            .count(),
        1
    );
}
#[test]
fn cancellation_before_admission_preserves_original_reason() {
    run(async {
        let store = Store::new();
        seed_key(&store);
        let host = Host::new(&store);
        host.cancelled.store(true, Ordering::SeqCst);
        let error = RoutingSession::begin(
            input("already-cancelled", "deepseek-flash", &["deepseek"]),
            store.clone(),
            host,
            store.clone(),
            store,
        )
        .await
        .err()
        .unwrap();
        assert_eq!(error.error.status, 499);
        assert_eq!(error.error.envelope.error.message, "fixture cancelled");
    })
}
#[test]
fn source_deepseek_extension_uses_one_captured_route_selection() -> TestResult {
    run(async {
        let store = Store::new();
        seed_key(&store);
        let mut request = input("captured-selection", "deepseek-flash", &["deepseek"]);
        request.surface = gateway_provider_protocol::types::ClientSurface::Chat;
        request.raw = fixture(
            &json!({"model":"deepseek-flash","messages":[{"role":"user","content":"fixture"}],"thinking":{"type":"disabled"}}),
        );
        request.catalog.selection = model_selection::ProviderSelection::normalize(
            &json!({"provider_ids":["cerebras"],"updated_at_ms":NOW}),
        );
        let error = source_plan(&request)
            .err()
            .ok_or("expected source rejection")?;
        assert_eq!(error.error.status, 400);
        let SourceRefusalBody::ClosedError(body) =
            error.source_body.ok_or("missing source refusal body")?
        else {
            panic!("expected the original source request error body")
        };
        assert_eq!(
            body.param().map(|param| param.cloned()),
            Some(Some("thinking".into()))
        );
        request.catalog.selection = None;
        let session = RoutingSession::begin(
            request.clone(),
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await?;
        request.catalog.selection = model_selection::ProviderSelection::normalize(
            &json!({"provider_ids":["cerebras"],"updated_at_ms":NOW+1}),
        );
        assert!(!request.catalog.enabled("deepseek"));
        let attempt = next_attempt(&session).await?;
        assert_eq!(attempt.request.provider(), UpstreamProvider::Deepseek);
        assert_eq!(
            source::member(attempt.request.body(), "reasoning_effort")
                .and_then(|value| source::string(&value)),
            Some("none".into())
        );
        session.finish(TransportObservation::default()).await?;
        Ok(())
    })
}
#[test]
fn measured_cancelled_wait_is_recorded_before_original_cancellation_error() {
    run(async {
        let golden: Value =
            serde_json::from_str(include_str!("fixtures/source-currency-wait.json")).unwrap();
        let elapsed = golden["cancelled"]["elapsed_ms"].as_i64().unwrap();
        let store = Store::new();
        seed_key(&store);
        let mut host = Host::new(&store);
        host.cancel_wait_after_ms = Some(elapsed);
        let session = RoutingSession::begin(
            input(
                "wait-cancel",
                "deepseek-ai/DeepSeek-V4.1-Flash",
                &["lithos"],
            ),
            store.clone(),
            host,
            store.clone(),
            store.clone(),
        )
        .await
        .unwrap();
        let attempt = session.next_attempt().await.unwrap().unwrap();
        session.before_dispatch(&attempt).await.unwrap();
        session.transport_started(&attempt).await.unwrap();
        let mut failure = refusal(429, TransportState::Started);
        failure.headers.insert("retry-after".into(), "60".into());
        let error = session.refusal(&attempt, failure).await.unwrap_err();
        assert_eq!(error.error.status, 499);
        assert_eq!(session.elapsed_wait_ms(), elapsed);
        session
            .finish(TransportObservation::default())
            .await
            .unwrap();
        assert_eq!(window(&store)["committed_requests"], 1);
    });
}
#[test]
fn lithos_refusal_headers_use_vendor_windows_and_response_prohibition() {
    run(async {
        for (id, headers, expected_prohibited) in [
            ("lithos-no-window", BTreeMap::new(), false),
            (
                "lithos-short-window",
                BTreeMap::from([("retry-after-ms".into(), "5".into())]),
                false,
            ),
            (
                "lithos-retry-after-ms",
                BTreeMap::from([("retry-after-ms".into(), "90000".into())]),
                false,
            ),
            (
                "lithos-reset-duration",
                BTreeMap::from([("x-ratelimit-reset-requests".into(), "90s".into())]),
                false,
            ),
            (
                "lithos-response-prohibited",
                BTreeMap::from([
                    ("retry-after-ms".into(), "5".into()),
                    ("x-should-retry".into(), "false".into()),
                ]),
                true,
            ),
        ] {
            let store = Store::new();
            seed_key(&store);
            let session = RoutingSession::begin(
                input(id, "deepseek-ai/DeepSeek-V4.1-Flash-ultra", &["lithos"]),
                store.clone(),
                Host::new(&store),
                store.clone(),
                store.clone(),
            )
            .await
            .unwrap();
            let attempt = session.next_attempt().await.unwrap().unwrap();
            session.before_dispatch(&attempt).await.unwrap();
            session.transport_started(&attempt).await.unwrap();
            let mut failure = refusal(429, TransportState::Started);
            failure.headers = headers;
            assert_eq!(
                session.refusal(&attempt, failure).await.unwrap(),
                RefusalDecision::Continue,
                "{id}"
            );
            assert_eq!(session.elapsed_wait_ms(), 0, "{id}");
            let next = session.next_attempt().await.unwrap().unwrap();
            assert!(
                next.request
                    .decode()
                    .model
                    .matches_scalar("deepseek-ai/DeepSeek-V4.1-Flash-fast"),
                "{id}: {:?}",
                next.request.decode().model
            );
            if expected_prohibited {
                assert_eq!(session.elapsed_wait_ms(), 0, "{id}");
            }
            session
                .finish(TransportObservation::default())
                .await
                .unwrap();
        }
    });
}
#[test]
fn lithos_response_prohibition_is_request_local_and_does_not_retry_the_rung() {
    run(async {
        let store = Store::new();
        seed_key(&store);
        let first = RoutingSession::begin(
            input(
                "lithos-local-prohibited",
                "deepseek-ai/DeepSeek-V4.1-Flash-ultra",
                &["lithos"],
            ),
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await
        .unwrap();
        let attempt = first.next_attempt().await.unwrap().unwrap();
        first.before_dispatch(&attempt).await.unwrap();
        first.transport_started(&attempt).await.unwrap();
        let mut refusal = refusal(429, TransportState::Started);
        refusal
            .headers
            .insert("x-should-retry".into(), "false".into());
        assert_eq!(
            first.refusal(&attempt, refusal).await.unwrap(),
            RefusalDecision::Continue
        );
        let next = first.next_attempt().await.unwrap().unwrap();
        assert!(
            next.request
                .decode()
                .model
                .matches_scalar("deepseek-ai/DeepSeek-V4.1-Flash-fast"),
            "the prohibited rung must not be retried: {:?}",
            next.request.decode().model
        );
        first.finish(TransportObservation::default()).await.unwrap();

        let second = RoutingSession::begin(
            input(
                "lithos-local-prohibited-next-request",
                "deepseek-ai/DeepSeek-V4.1-Flash-ultra",
                &["lithos"],
            ),
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await
        .unwrap();
        let attempt = second.next_attempt().await.unwrap().unwrap();
        assert!(
            attempt
                .request
                .decode()
                .model
                .matches_scalar("deepseek-ai/DeepSeek-V4.1-Flash-ultra"),
            "another request must still try the prohibited model: {:?}",
            attempt.request.decode().model
        );
        second
            .finish(TransportObservation::default())
            .await
            .unwrap();
    });
}
#[test]
fn explicit_provider_504_survives_late_abort_and_local_deadline_never_advances() {
    run(async {
        let store = Store::new();
        seed_key(&store);
        let host = Host::new(&store);
        let mut request = input(
            "provider-504",
            "ubiquity/deepseek-flash",
            &["lithos", "deepseek"],
        );
        set_source(&mut request.raw, "reasoning", json!({"effort":"high"}));
        let session = RoutingSession::begin(
            request,
            store.clone(),
            host.clone(),
            store.clone(),
            store.clone(),
        )
        .await
        .unwrap();
        let attempt = session.next_attempt().await.unwrap().unwrap();
        session.before_dispatch(&attempt).await.unwrap();
        session.transport_started(&attempt).await.unwrap();
        host.cancelled.store(true, Ordering::SeqCst);
        assert_eq!(
            session
                .refusal(&attempt, refusal(504, TransportState::Started))
                .await
                .unwrap(),
            RefusalDecision::Return
        );
        session
            .finish(TransportObservation::default())
            .await
            .unwrap();
        let mut request = input(
            "local-504",
            "ubiquity/deepseek-flash",
            &["lithos", "deepseek"],
        );
        set_source(&mut request.raw, "reasoning", json!({"effort":"high"}));
        let session = RoutingSession::begin(
            request,
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await
        .unwrap();
        let attempt = session.next_attempt().await.unwrap().unwrap();
        session.before_dispatch(&attempt).await.unwrap();
        session.transport_started(&attempt).await.unwrap();
        let mut deadline = refusal(504, TransportState::Started);
        deadline.origin = gateway_routing_runtime::types::FailureOrigin::GatewayDeadline;
        assert_eq!(
            session.refusal(&attempt, deadline).await.unwrap(),
            RefusalDecision::Abort
        );
        session
            .finish(TransportObservation::default())
            .await
            .unwrap();
    });
}
