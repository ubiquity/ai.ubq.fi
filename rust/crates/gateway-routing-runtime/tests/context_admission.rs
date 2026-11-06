//! Ports the synthetic waterfall vector of tests/codex-context-admission.test.ts (19d9eec):
//! a local aggregate context failure stops the DeepSeek waterfall before any provider or
//! paid-tail dispatch, and every other hop stays unprepared.
use gateway_core::{lifecycle::TransportObservation, source_json::SourceJson};
use gateway_prompt_admission::admission::is_aggregate_context_overflow;
use gateway_routing_runtime::{session::RoutingSession, types::SourceRefusalBody};
use gateway_routing_runtime_test_support::{
    exec::run,
    host::Host,
    input::input,
    seed::{seed_key, window},
    store::Store,
};
use serde_json::{Value, json};

type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;

fn large_input() -> Vec<Value> {
    let mut input = (0..110)
        .map(|index| {
            json!({"type":"message","role":"user","content":[{"type":"input_text","text":format!("{index}: {}", "x".repeat(60 * 1024))}]})
        })
        .collect::<Vec<_>>();
    input.push(json!({"type":"message","role":"user","content":[{"type":"input_text","text":"PRESERVE_LATEST_INPUT_CANARY"}]}));
    input
}

fn exercise(model: &str, providers: &[&str]) -> TestResult {
    run(async {
        let store = Store::new();
        seed_key(&store);
        let mut request = input("context-admission", model, providers);
        request.raw = SourceJson::from_serializable(&json!({
            "model": model, "input": large_input(), "reasoning": {"effort": "high"}, "stream": true
        }))?;
        let session = RoutingSession::begin(
            request,
            store.clone(),
            Host::new(&store),
            store.clone(),
            store.clone(),
        )
        .await?;
        let error = session
            .next_attempt()
            .await
            .err()
            .ok_or("aggregate overflow must not prepare a dispatch")?;
        assert!(is_aggregate_context_overflow(&error.error), "{model}");
        assert_eq!(error.error.status, 400);
        let Some(SourceRefusalBody::Json(body)) = &error.source_body else {
            return Err("the HTTP 400 source body is required".into());
        };
        let body: Value = body.deserialize()?;
        assert_eq!(body["error"]["code"], "context_length_exceeded");
        assert_eq!(body["error"]["param"], "input");
        assert!(
            body["error"]["message"]
                .as_str()
                .is_some_and(|message| message.contains("no payload was dispatched"))
        );
        assert!(
            !matches!(session.next_attempt().await, Ok(Some(_))),
            "no later hop may be prepared after a local context failure"
        );
        session.finish(TransportObservation::default()).await?;
        assert_eq!(window(&store)["reserved_requests"], 0);
        Ok(())
    })
}

#[test]
fn synthetic_waterfall_stops_local_context_failure_without_any_provider_dispatch() -> TestResult {
    exercise("ubiquity/deepseek-flash", &["lithos", "deepseek"])
}

#[test]
fn direct_deepseek_and_lithos_overflow_is_local_and_terminal() -> TestResult {
    exercise("deepseek-flash", &["deepseek", "lithos"])
}
