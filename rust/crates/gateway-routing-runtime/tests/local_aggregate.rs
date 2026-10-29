//! Candidate sibling case through actual routing and authority owners; no provider dispatch occurs.
use gateway_contracts::lifecycle::TransportObservation;
use gateway_routing_journal::reader;
use gateway_routing_runtime::session::RoutingSession;
use gateway_routing_runtime_test_support::{
    exec::run,
    host::Host,
    input::input,
    seed::{seed_key, window},
    store::Store,
};
use gateway_source::source_json::SourceJson;
use serde_json::json;
type TestResult = Result<(), Box<dyn std::error::Error>>;
#[test]
fn local_aggregate_error_never_mints_attempt_or_paid_dispatch_and_cleanup_is_once() -> TestResult {
    run(async {
        let store = Store::new();
        seed_key(&store);
        let id = "local-aggregate-refusal";
        let mut request = input(id, "deepseek-flash", &["deepseek", "surplus", "openlux"]);
        request.raw = SourceJson::from_serializable(
            &json!({"model":"deepseek-flash","input":"small prompt","max_output_tokens":9_007_199_254_740_991_u64,"stream":true,"reasoning":{"effort":"none"}}),
        )?;
        let session = RoutingSession::begin(
            request,
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await?;
        let error = match session.next_attempt().await {
            Err(error) => error,
            Ok(_) => return Err("overflow produced an attempt".into()),
        };
        assert!(error.is_local_aggregate_context_overflow());
        assert_eq!(error.error.status, 400);
        let (_, before) = reader::load(&store, id).await?;
        assert!(before.attempts.is_empty());
        assert!(before.active.is_none());
        assert_eq!(before.cursor, 0);
        assert!(!before.semantic_committed);
        session.finish(TransportObservation::default()).await?;
        let (_, after) = reader::load(&store, id).await?;
        assert!(after.finished);
        assert!(after.attempts.is_empty());
        let settled = window(&store);
        assert_eq!(settled["committed_requests"], 0);
        session.finish(TransportObservation::default()).await?;
        assert_eq!(window(&store), settled);
        Ok(())
    })
}
