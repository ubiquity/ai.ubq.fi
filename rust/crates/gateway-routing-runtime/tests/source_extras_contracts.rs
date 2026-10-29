use gateway_authority_contracts::REQUEST_LEASE_MS;
use gateway_contracts::{
    lifecycle::DeliveryOutcome, lifecycle::TransportObservation, provider::UpstreamProvider,
};
use gateway_routing_contract_test_support::{
    fixtures::TestResult, fixtures::complete, fixtures::fixture, fixtures::next_attempt,
    fixtures::refusal,
};
use gateway_routing_runtime::{
    session::RoutingSession, settlement::recover_pending, types::RefusalDecision,
    types::TransportState,
};
use gateway_routing_runtime_test_support::{
    exec::run, host::Host, input::input, seed::seed_key, seed::seed_pool, seed::window,
    store::Store,
};
use serde_json::json;

#[test]
fn extras_children_cannot_refund_started_siblings_or_finish_the_outer_kernel() -> TestResult {
    run(async {
        use gateway_authority_kernel_protocol::KernelTerminal;
        use gateway_extras_contract::family::ExtrasFamily;
        let store = Store::new();
        seed_key(&store);
        let session = RoutingSession::begin_extras(
            input("image-parent", "gpt-test", &[]),
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
            ExtrasFamily::ImageGenerations,
        )
        .await?;
        let handle = session.admission_handle();
        let children = handle.children(2).await?;
        assert_eq!(children[0].request_id(), "image-parent");
        assert_eq!(children[1].request_id(), "image-parent:image:generations:2");
        assert!(session.next_attempt().await.is_err());
        assert!(session.finish(complete()).await.is_err());
        let ticket = handle.before_dispatch(&children[0]).await?;
        handle.transport_started(&children[0], &ticket).await?;
        handle
            .child_finished(&children[1], TransportState::NotStarted)
            .await?;
        assert!(
            handle
                .finish(complete(), KernelTerminal::Committed, "outer complete")
                .await
                .is_err()
        );
        assert!(
            handle
                .child_finished(&children[0], TransportState::NotStarted)
                .await
                .is_err()
        );
        handle
            .child_finished(&children[0], TransportState::Started)
            .await?;
        handle
            .finish(complete(), KernelTerminal::Committed, "outer complete")
            .await?;
        handle
            .finish(complete(), KernelTerminal::Committed, "outer complete")
            .await?;
        assert_eq!(window(&store)["committed_requests"], 1);
        Ok(())
    })
}
#[test]
fn extras_known_no_transport_refunds_but_unknown_intent_recovery_does_not() -> TestResult {
    run(async {
        use gateway_authority_kernel_protocol::KernelTerminal;
        use gateway_extras_contract::family::ExtrasFamily;
        let store = Store::new();
        seed_key(&store);
        let session = RoutingSession::begin_extras(
            input("image-none", "gpt-test", &[]),
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
            ExtrasFamily::ImageEdits,
        )
        .await?;
        let handle = session.admission_handle();
        let children = handle.children(2).await?;
        handle.before_dispatch(&children[0]).await?;
        for child in children {
            handle
                .child_finished(&child, TransportState::NotStarted)
                .await?;
        }
        handle
            .finish(
                TransportObservation::default(),
                KernelTerminal::Released,
                "source cancelled",
            )
            .await?;
        assert_eq!(window(&store)["committed_requests"], 0);
        let session = RoutingSession::begin_extras(
            input("image-unknown", "gpt-test", &[]),
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
            ExtrasFamily::ImageGenerations,
        )
        .await?;
        let handle = session.admission_handle();
        let children = handle.children(1).await?;
        handle.before_dispatch(&children[0]).await?;
        store.advance(REQUEST_LEASE_MS + 1);
        let recovered = recover_pending(
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
            100,
        )
        .await?;
        assert_eq!(recovered.len(), 1);
        assert_eq!(
            recovered[0].observation.delivery,
            DeliveryOutcome::Unobserved
        );
        assert_eq!(window(&store)["committed_requests"], 1);
        Ok(())
    })
}
#[test]
fn sequential_extras_reuse_original_scope_and_cannot_borrow_another_ticket() -> TestResult {
    run(async {
        use gateway_authority_kernel_protocol::KernelTerminal;
        use gateway_extras_contract::family::ExtrasFamily;
        let store = Store::new();
        seed_key(&store);
        let session = RoutingSession::begin_extras(
            input("voyage", "voyage-3", &[]),
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
            ExtrasFamily::Embeddings,
        )
        .await?;
        let handle = session.admission_handle();
        let child = handle.children(1).await?.remove(0);
        let other = RoutingSession::begin_extras(
            input("other", "voyage-3", &[]),
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
            ExtrasFamily::Embeddings,
        )
        .await?
        .admission_handle();
        let other_child = other.children(1).await?.remove(0);
        let other_ticket = other.before_dispatch(&other_child).await?;
        assert!(
            handle
                .transport_started(&child, &other_ticket)
                .await
                .is_err()
        );
        for _ in 0..2 {
            let ticket = handle.before_dispatch(&child).await?;
            handle.transport_started(&child, &ticket).await?;
        }
        handle
            .child_finished(&child, TransportState::Started)
            .await?;
        handle
            .finish(complete(), KernelTerminal::Committed, "voyage complete")
            .await?;
        other
            .child_finished(&other_child, TransportState::NotStarted)
            .await?;
        other
            .finish(
                TransportObservation::default(),
                KernelTerminal::Released,
                "no chunk started",
            )
            .await?;
        assert_eq!(window(&store)["committed_requests"], 1);
        Ok(())
    })
}
#[test]
fn image_child_uses_codex_only_and_aggregate_dispatch_without_second_admission() -> TestResult {
    run(async {
        use gateway_authority_kernel_protocol::KernelTerminal;
        use gateway_extras_contract::family::ExtrasFamily;
        let store = Store::new();
        seed_key(&store);
        seed_pool(&store);
        let session = RoutingSession::begin_extras(
            input("image-codex", "gpt-test", &[]),
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
            ExtrasFamily::ImageGenerations,
        )
        .await?;
        let handle = session.admission_handle();
        let child = handle.children(1).await?.remove(0);
        let attempt=handle.prepare_image_child(&child,fixture(&json!({"model":"gpt-test","input":"image fixture","tools":[{"type":"image_generation"}]}))).await?;
        assert_eq!(attempt.request.provider, UpstreamProvider::ChatgptCodex);
        let ticket = handle.before_image_dispatch(&attempt).await?;
        handle.transport_started(&child, &ticket).await?;
        assert_eq!(
            handle
                .image_refusal(&attempt, refusal(503, TransportState::Started))
                .await?,
            RefusalDecision::Return
        );
        handle
            .child_finished(&child, TransportState::Started)
            .await?;
        handle
            .finish(complete(), KernelTerminal::Committed, "image complete")
            .await?;
        assert_eq!(window(&store)["committed_requests"], 1);
        Ok(())
    })
}
#[test]
fn embedding_job_read_without_retry_releases_and_never_commits_kernel_admission() -> TestResult {
    run(async {
        use gateway_authority_kernel_protocol::KernelTerminal;
        use gateway_extras_contract::family::ExtrasFamily;
        let store = Store::new();
        seed_key(&store);
        let session = RoutingSession::begin_extras(
            input("job-poll", "voyage-3", &[]),
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
            ExtrasFamily::EmbeddingJobPoll,
        )
        .await?;
        let handle = session.admission_handle();
        let child = handle.children(1).await?.remove(0);
        handle
            .child_finished(&child, TransportState::NotStarted)
            .await?;
        assert!(
            handle
                .finish(complete(), KernelTerminal::Committed, "job cached")
                .await
                .is_err()
        );
        handle
            .finish(complete(), KernelTerminal::Released, "job read")
            .await?;
        assert_eq!(window(&store)["committed_requests"], 0);
        Ok(())
    })
}
