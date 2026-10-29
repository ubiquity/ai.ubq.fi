use gateway_authority_contracts::DAY_MS;
use gateway_authority_keys as keys;
use gateway_contracts::{lifecycle::TransportObservation, provider::UpstreamProvider};
use gateway_routing_contract_test_support::{
    fixtures::TestResult, fixtures::next_attempt, fixtures::paid_input, fixtures::refusal,
    fixtures::set_source,
};
use gateway_routing_runtime::{
    policy, session::RoutingSession, types::RefusalDecision, types::TransportState,
};
use gateway_routing_runtime_test_support::{
    exec::run, host::Host, input::NOW, seed::seed_key, seed::window, store::Store,
};
use gateway_source::source_serialization as source;
use serde_json::{Value, json};

#[test]
fn paid_receipt_matches_current_host_custody_and_rechecks_before_dispatch() -> TestResult {
    run(async {
        let store = Store::new();
        seed_key(&store);
        let request = paid_input(&store, "paid-current", "paid-model")?;
        let session = RoutingSession::begin(
            request,
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await?;
        let attempt = next_attempt(&session).await?;
        assert!(attempt.paid.is_some());
        assert!(matches!(
            attempt.credential,
            gateway_routing_journal::record::CredentialFence::Paid { .. }
        ));
        session.before_dispatch(&attempt).await?;
        session.transport_started(&attempt).await?;
        assert_eq!(
            session
                .refusal(&attempt, refusal(503, TransportState::Started))
                .await?,
            RefusalDecision::Return
        );
        session.finish(TransportObservation::default()).await?;
        assert_eq!(window(&store)["committed_requests"], 1);

        let request = paid_input(&store, "paid-rotated", "paid-model")?;
        let session = RoutingSession::begin(
            request,
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await?;
        let attempt = next_attempt(&session).await?;
        let mut config = store.value(gateway_authority_keys::paid_control_config());
        config["generation"] = json!("next-generation");
        store.seed(gateway_authority_keys::paid_control_config(), config);
        assert!(session.before_dispatch(&attempt).await.is_err());
        let mut failure = refusal(499, TransportState::NotStarted);
        failure.cancelled = true;
        session.refusal(&attempt, failure).await?;
        session.finish(TransportObservation::default()).await?;
        assert_eq!(window(&store)["committed_requests"], 1);
        Ok(())
    })
}
#[test]
fn paid_original_api_release_prevents_new_paid_exposure() -> TestResult {
    use gateway_authority::store::Authority;
    use gateway_authority_api_protocol::ApiReservation;
    use gateway_authority_api_release::ApiReleaseOperations;
    use gateway_kv_codec::{tuple_key::ScalarKeyPart, tuple_key::TupleKey};

    run(async {
        let store = Store::new();
        seed_key(&store);
        let id = "paid-original-released";
        let session = RoutingSession::begin(
            paid_input(&store, id, "paid-model")?,
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await?;
        let journal_key = TupleKey::from_scalar_parts(&[
            ScalarKeyPart::String("uos_ai"),
            ScalarKeyPart::String("routing_runtime"),
            ScalarKeyPart::String("v1"),
            ScalarKeyPart::String("session"),
            ScalarKeyPart::String(id),
        ])
        .to_record_key();
        let original_api: ApiReservation =
            serde_json::from_value(store.value(journal_key)["api"].clone())?;
        Authority::with_clock(store.clone(), store.clone(), store.clone())
            .release_api_key(&original_api, true, "fixture_original_released", NOW)
            .await?;
        assert!(session.next_attempt().await.is_err());
        assert_eq!(
            store.value(keys::paid_window("key-a", NOW + DAY_MS)),
            Value::Null,
            "A stale original API owner must fail before any paid exposure is created"
        );
        Ok(())
    })
}
#[test]
fn paid_dispatch_missing_active_history_cannot_recreate_exposure() -> TestResult {
    use gateway_core::state::{
        AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, RevisionCheck,
    };

    run(async {
        let store = Store::new();
        seed_key(&store);
        let session = RoutingSession::begin(
            paid_input(&store, "paid-active-history-missing", "paid-model")?,
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await?;
        let attempt = next_attempt(&session).await?;
        let paid = attempt.paid.as_ref().ok_or("missing paid reservation")?;
        let key = keys::paid_attempt(&paid.key_id.0, &paid.attempt_id.0);
        let record = store.read(&key).await?.ok_or("missing fixture value")?;
        let window_key = keys::paid_window("key-a", NOW + DAY_MS);
        let exposure = store.value(window_key.clone());
        assert_ne!(exposure, Value::Null);
        assert_eq!(
            store
                .commit(AtomicTransition {
                    checks: vec![RevisionCheck {
                        key: key.clone(),
                        expected_revision: Some(record.revision),
                    }],
                    mutations: vec![RecordMutation::Delete { key: key.clone() }],
                })
                .await?,
            CommitOutcome::Committed
        );
        assert!(session.before_dispatch(&attempt).await.is_err());
        assert!(store.read(&key).await?.is_none());
        assert_eq!(
            store.value(window_key),
            exposure,
            "Existing-only dispatch must not reserve fresh exposure when history vanished"
        );
        assert_eq!(window(&store)["committed_requests"], 0);
        Ok(())
    })
}
#[test]
fn paid_synthetic_hop_uses_server_pinned_context_and_distinct_billing_identity() -> TestResult {
    run(async {
        let store = Store::new();
        seed_key(&store);
        let mut request = paid_input(&store, "synthetic-paid", "ubiquity/deepseek-flash")?;
        set_source(&mut request.raw, "reasoning", json!({"effort":"high"}));
        let session = RoutingSession::begin(
            request,
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await?;
        let first = next_attempt(&session).await?;
        let reservation = first.paid.as_ref().ok_or("missing paid reservation")?;
        assert_eq!(
            reservation.attempt_id.0,
            gateway_provider_policy::policy::paid_hop_identity(
                "synthetic-paid",
                gateway_contracts::provider::PaidProvider::Surplus
            )
        );
        session.before_dispatch(&first).await?;
        session.transport_started(&first).await?;
        assert_eq!(
            session
                .refusal(&first, refusal(429, TransportState::Started))
                .await?,
            RefusalDecision::Continue
        );
        let second = next_attempt(&session).await?;
        assert_eq!(second.request.provider(), UpstreamProvider::Metered);
        assert_ne!(
            first
                .paid
                .ok_or("missing first paid reservation")?
                .attempt_id,
            second
                .paid
                .as_ref()
                .ok_or("missing second paid reservation")?
                .attempt_id
        );
        assert_eq!(first.original_request_id, second.original_request_id);
        let mut failure = refusal(499, TransportState::NotStarted);
        failure.cancelled = true;
        session.refusal(&second, failure).await?;
        session.finish(TransportObservation::default()).await?;
        assert_eq!(window(&store)["committed_requests"], 1);
        Ok(())
    })
}
#[test]
fn paid_capability_endpoint_and_logical_model_stay_separate_from_wire_alias() -> TestResult {
    assert_eq!(
        policy::endpoint(gateway_provider_protocol::types::ClientSurface::Responses),
        "openai-response"
    );
    assert_eq!(
        policy::endpoint(gateway_provider_protocol::types::ClientSurface::Chat),
        "openai"
    );
    let raw = json!({"model":"gpt-5.6-sol","input":"source fixture","reasoning":{"effort":"none"}});
    let options = gateway_provider_protocol::types::PrepareOptions {
        auth: gateway_provider_protocol::types::AuthMaterial {
            secret: "fixture".into(),
            ..Default::default()
        },
        ..Default::default()
    };
    let prepared = gateway_providers::request::prepare_request(
        UpstreamProvider::Metered,
        gateway_provider_protocol::types::ClientSurface::Responses,
        &raw,
        &options,
    )?;
    assert_eq!(raw["model"], "gpt-5.6-sol");
    assert_eq!(
        source::member(&prepared.body, "model").and_then(|value| source::string(&value)),
        Some("gpt-5.6-sol-low".into())
    );
    assert!(source::member(&prepared.body, "reasoning").is_none());
    Ok(())
}
