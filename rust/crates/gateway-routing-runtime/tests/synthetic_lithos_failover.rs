//! Source 7c6ebdb (src/deepseek/waterfall-handler.ts, src/provider/lithos-rate-limits.ts):
//! a synthetic waterfall Lithos hop uses `LITHOS_IMMEDIATE_FAILOVER_WAIT_POLICY`, so a quota or
//! payment refusal advances to the next hop at once instead of spending the vendor reset window.
use gateway_core::{
    js_string::JsString16, lifecycle::TransportObservation, provider::UpstreamProvider,
    source_json::SourceJson, source_serialization as source,
};
use gateway_providers::policy::HopFailure;
use gateway_routing_runtime::{
    session::RoutingSession,
    types::{AttemptFailure, FailureOrigin, RefusalDecision, TransportState},
};
use gateway_routing_runtime_test_support::{
    exec::run, host::Host, input::input, seed::seed_key, store::Store,
};
use serde_json::json;
use std::collections::BTreeMap;

fn refusal(status: u16) -> AttemptFailure {
    AttemptFailure {
        origin: FailureOrigin::Provider,
        status: Some(status),
        // A reset window the direct Lithos route would wait for.
        headers: BTreeMap::from([
            ("retry-after".into(), "60".into()),
            ("x-ratelimit-reset-requests".into(), "90s".into()),
        ]),
        body: vec![],
        body_complete: true,
        failure: HopFailure::Status(status),
        transport: TransportState::Started,
        semantic_committed: false,
        cancelled: false,
    }
}

type TestResult = Result<(), Box<dyn std::error::Error>>;

async fn advances_without_waiting(status: u16) -> TestResult {
    let id = format!("synthetic-lithos-{status}");
    let store = Store::new();
    seed_key(&store);
    let mut request = input(&id, "ubiquity/deepseek-flash", &["lithos", "deepseek"]);
    // The synthetic catalog advertises explicit efforts; mirror the source contract test.
    let mut fields = source::entries(&request.raw).ok_or("fixture input is an object")?;
    let effort = SourceJson::from_serializable(&json!({"effort": "high"}))?;
    fields.push((JsString16::from_utf8("reasoning"), effort));
    request.raw = source::object_keys(fields)?;
    let host = Host::new(&store);
    let session = RoutingSession::begin(request, store.clone(), host, store.clone(), store.clone());
    let session = session.await?;
    let first = session.next_attempt().await?.ok_or("first hop")?;
    assert_eq!(first.request.provider(), UpstreamProvider::Lithos, "{id}");
    session.before_dispatch(&first).await?;
    session.transport_started(&first).await?;
    let decision = session.refusal(&first, refusal(status)).await?;
    assert_eq!(decision, RefusalDecision::Continue, "{id}");
    assert_eq!(session.elapsed_wait_ms(), 0, "{id}: no reset wait");
    let second = session.next_attempt().await?.ok_or("second hop")?;
    assert_eq!(
        second.request.provider(),
        UpstreamProvider::Deepseek,
        "{id}"
    );
    assert_eq!(session.elapsed_wait_ms(), 0, "{id}: no reset wait");
    session.finish(TransportObservation::default()).await?;
    Ok(())
}

#[test]
fn synthetic_lithos_hop_advances_on_quota_and_payment_refusals_without_waiting() -> TestResult {
    run(async {
        for status in [429, 402] {
            advances_without_waiting(status).await?;
        }
        Ok(())
    })
}
