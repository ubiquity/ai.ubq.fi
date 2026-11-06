//! Port of the transport-independent assertions in tests/responses-native-headers.test.ts and
//! tests/runtime-early-sse.test.ts (e9b1b90, 9e75476): only an admitted native streamed turn
//! commits SSE headers, once, after its dispatch gate, and the same pending pipeline survives.
use gateway_core::source_json::SourceJson;
use gateway_worker::inference_early::{Gate, drive};
use gateway_worker::inference_source::NativeTurn;
use std::{
    cell::Cell,
    future::Future,
    pin::Pin,
    rc::Rc,
    task::{Context, Poll, Waker},
};

type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;
const METADATA: &str = r#"{"session_id":"session","thread_id":"thread","turn_id":"turn"}"#;

fn json(text: &str) -> TestResult<SourceJson> {
    SourceJson::parse(text).map_err(|error| error.envelope.error.message.into())
}
fn armed(stream: &str, model: &str) -> TestResult<Rc<Gate>> {
    let gate = Gate::for_request(true, Some(METADATA)).ok_or("a native Responses turn")?;
    gate.arm(Some(&json(stream)?), Some(&json(model)?));
    Ok(gate)
}
fn poll_once<F: Future + Unpin>(future: &mut F) -> Poll<F::Output> {
    Pin::new(future).poll(&mut Context::from_waker(Waker::noop()))
}

#[test]
fn ineligible_native_requests_keep_their_original_http_answers() -> TestResult {
    assert!(Gate::for_request(true, None).is_none());
    assert!(Gate::for_request(true, Some("{}")).is_none());
    assert!(
        Gate::for_request(
            true,
            Some(r#"{"session_id":"s","thread_id":"t","turn_id":" "}"#)
        )
        .is_none()
    );
    assert!(Gate::for_request(false, Some(METADATA)).is_none());
    for (stream, model) in [
        ("false", r#""deepseek-ai/DeepSeek-V4.1-Flash""#),
        (r#""true""#, r#""deepseek-ai/DeepSeek-V4.1-Flash""#),
        ("true", r#""gpt-oss-120b""#),
        ("true", r#"" GPT-OSS-120B ""#),
        ("true", r#""qwen-3.8-27b""#),
    ] {
        let gate = armed(stream, model)?;
        gate.commit(false);
        assert!(!gate.committed(), "{stream} {model}");
    }
    Ok(())
}

#[test]
fn admitted_native_dispatch_commits_once_and_an_aborted_caller_commits_nothing() -> TestResult {
    let gate = armed("true", r#""deepseek-ai/DeepSeek-V4.1-Flash""#)?;
    // A failed dispatch admission never reaches the commit.
    assert!(!gate.committed());
    gate.commit(true);
    assert!(!gate.committed());
    gate.commit(false);
    gate.commit(false);
    assert!(gate.committed());
    let turn = NativeTurn {
        early: Some(gate),
        ..NativeTurn::default()
    };
    assert!(turn.early_committed());
    assert!(!NativeTurn::default().early_committed());
    let unknown = armed("true", "7")?;
    unknown.commit(false);
    assert!(unknown.committed());
    Ok(())
}

#[test]
fn an_answer_before_any_dispatch_gate_keeps_the_ordinary_response() -> TestResult {
    let gate = armed("true", r#""model""#)?;
    let mut driven = Box::pin(drive(&gate, std::future::ready(429)));
    assert!(matches!(poll_once(&mut driven), Poll::Ready(Ok(429))));
    assert!(!gate.committed());
    Ok(())
}

/// Commits at its dispatch gate, then waits for selection; it never sends a body byte itself.
struct Pending {
    gate: Rc<Gate>,
    polls: Rc<Cell<u32>>,
    selected: Rc<Cell<bool>>,
}
impl Future for Pending {
    type Output = &'static str;
    fn poll(self: Pin<&mut Self>, _: &mut Context<'_>) -> Poll<Self::Output> {
        self.polls.set(self.polls.get() + 1);
        self.gate.commit(false);
        if self.selected.get() {
            Poll::Ready("selected")
        } else {
            Poll::Pending
        }
    }
}

#[test]
fn committed_headers_hand_back_the_same_pending_pipeline() -> TestResult {
    let gate = armed("true", r#""model""#)?;
    let (polls, selected) = (Rc::new(Cell::new(0)), Rc::new(Cell::new(false)));
    let pipeline = Pending {
        gate: gate.clone(),
        polls: polls.clone(),
        selected: selected.clone(),
    };
    let mut driven = Box::pin(drive(&gate, pipeline));
    let Poll::Ready(Err(mut pending)) = poll_once(&mut driven) else {
        return Err("headers commit while selection is pending".into());
    };
    assert!(gate.committed());
    assert_eq!(polls.get(), 1);
    assert_eq!(poll_once(&mut pending), Poll::Pending);
    selected.set(true);
    assert_eq!(poll_once(&mut pending), Poll::Ready("selected"));
    assert_eq!(polls.get(), 3);
    Ok(())
}
