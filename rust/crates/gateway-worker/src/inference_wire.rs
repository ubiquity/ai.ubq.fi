//! Source upstream-wire facts the relay reads: provider request ids and the Chat wire terminal.
use gateway_core::{
    lifecycle::{ReaderTermination, SemanticTerminal, TransportObservation},
    provider::UpstreamProvider,
};
use gateway_providers::types::{Headers, ProviderEvent};
use gateway_schema::util::js_trim;

/// Source `providerRequestIdFromResponse` and the DeepSeek/Cerebras readers: the first PRESENT
/// header is selected and only then normalized (`normalizeProviderRequestId`), so a malformed
/// preferred value never selects a later header. Source OpenRouter has no body-id fallback.
pub fn provider_request_id(provider: UpstreamProvider, headers: &Headers) -> Option<&str> {
    let names: &[&str] = match provider {
        UpstreamProvider::Deepseek => &["x-request-id", "x-ds-request-id", "x-deepseek-request-id"],
        UpstreamProvider::Cerebras => &[
            "x-request-id",
            "x-api-request-id",
            "x-oneapi-request-id",
            "x-cerebras-request-id",
        ],
        UpstreamProvider::Openrouter => {
            &["x-request-id", "x-api-request-id", "x-oneapi-request-id"]
        }
        _ => &["x-request-id", "request-id"],
    };
    names
        .iter()
        .find_map(|wanted| {
            headers
                .iter()
                .find(|(name, _)| name.eq_ignore_ascii_case(wanted))
                .map(|(_, value)| js_trim(value))
        })
        .filter(|id| {
            !id.is_empty()
                && id.encode_utf16().count() <= 256
                && !id.chars().any(|c| c <= '\u{1f}' || c == '\u{7f}')
        })
}

/// Source `relayChatCompletionStream`: a Chat wire settles only at `[DONE]` (`finishStream`).
/// A finish chunk is relayed like any chunk, so the trailing `include_usage` chunk still
/// reaches the client and the ledger; `[DONE]` without a finish chunk is still completed.
pub fn chat_wire_terminal(
    pending: &mut Option<SemanticTerminal>,
    event: &ProviderEvent,
) -> Option<SemanticTerminal> {
    if event.is_done() {
        return Some(pending.take().unwrap_or(SemanticTerminal::Completed));
    }
    if let Some(terminal) = event.terminal() {
        *pending = Some(terminal);
    }
    None
}

/// The administrative error row's `(terminal_type, failure_kind)`. Source terminal-log passes
/// `resolveSentinelClientFailureObservation`: a cancelled stream keeps `cancelled` and drops every
/// failure kind, so a client abort writes no `/admin/errors` row (and no replay capture), exactly
/// as a cancelled source relay does; its interruption stays in the delivery outcome.
pub fn admin_terminal(
    observation: TransportObservation,
    stream: bool,
    observed_terminal: Option<String>,
    synthetic_failure: Option<String>,
) -> (Option<String>, Option<String>) {
    if observation.reader_termination == Some(ReaderTermination::Cancelled) {
        return (Some("cancelled".into()), None);
    }
    let terminal = observation
        .semantic_terminal
        .map(|terminal| {
            match terminal {
                SemanticTerminal::Completed => "response.completed",
                SemanticTerminal::Failed => "response.failed",
                SemanticTerminal::Incomplete => "response.incomplete",
                SemanticTerminal::Error => "error",
            }
            .to_owned()
        })
        .or(observed_terminal)
        .or_else(|| match observation.reader_termination {
            Some(ReaderTermination::ReadError) => Some("error".into()),
            Some(ReaderTermination::Deadline) => Some("deadline".into()),
            Some(ReaderTermination::Eof) if stream => Some("eof".into()),
            _ => None,
        });
    let failure_kind = match observation.reader_termination {
        Some(ReaderTermination::ReadError) => Some("gateway_stream_read_error".into()),
        Some(ReaderTermination::Deadline) => Some("gateway_timeout".into()),
        _ => synthetic_failure,
    };
    (terminal, failure_kind)
}
