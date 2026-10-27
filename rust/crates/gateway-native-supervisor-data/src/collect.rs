//! Real selected-source collection stays private with registry/cache currentness.
use gateway_native_local_transport::{
    transport::{SupervisorMethod, SupervisorTransport},
    unix::UnixSupervisor,
};
use gateway_native_supervisor::{
    brief::{
        BriefTranscriptTurn, SupervisorBriefContext, append_rollout_tail, assemble_brief_context,
        brief_turn, dedupe_turns, newest_turn_ms, sanitize_context_metadata,
    },
    rollout::read_rollout_tail,
    supervisor::KnownThread,
};
use gateway_native_supervisor_read::inventory::{epoch_ms, text};
use gateway_source::error::GatewayError;
use serde_json::{Value, json};
use std::{
    path::Path,
    time::{Duration, Instant},
};

fn call(
    socket: &mut UnixSupervisor,
    method: SupervisorMethod,
    params: Value,
) -> Result<Value, GatewayError> {
    futures_executor::block_on(socket.call(method, params))
}
fn fetch_turns(
    socket: &mut UnixSupervisor,
    id: &str,
    direction: &str,
    limit: usize,
    view: &str,
) -> Result<Vec<BriefTranscriptTurn>, GatewayError> {
    let value = call(
        socket,
        SupervisorMethod::ThreadTurnsList,
        json!({"threadId":id,"limit":limit,"sortDirection":direction,"itemsView":view}),
    )?;
    Ok(value
        .get("data")
        .and_then(Value::as_array)
        .map(|v| {
            v.iter()
                .filter_map(|v| brief_turn(v, direction == "asc"))
                .collect()
        })
        .unwrap_or_default())
}
pub(crate) fn collect_context(
    socket_path: &Path,
    home: Option<&Path>,
    thread: &KnownThread,
    machine: &str,
    _now_ms: i64,
) -> Result<SupervisorBriefContext, GatewayError> {
    let mut socket = UnixSupervisor::configured_socket_until(
        socket_path,
        Instant::now() + Duration::from_secs(45),
    )?;
    let result = (|| {
        let reply = call(
            &mut socket,
            SupervisorMethod::ThreadRead,
            json!({"threadId":thread.id(),"includeTurns":false}),
        )?;
        let meta = reply.get("thread").cloned().unwrap_or(Value::Null);
        let status = meta.get("status");
        let state = status
            .and_then(|v| v.get("type"))
            .and_then(Value::as_str)
            .map(str::to_owned);
        let active_flags = status
            .and_then(|v| v.get("activeFlags"))
            .and_then(Value::as_array)
            .map(|v| {
                v.iter()
                    .filter_map(Value::as_str)
                    .map(str::to_owned)
                    .collect::<Vec<_>>()
            })
            .unwrap_or_default();
        let first = fetch_turns(&mut socket, thread.id(), "asc", 1, "summary")?;
        let mut recent = fetch_turns(&mut socket, thread.id(), "desc", 6, "summary")?;
        recent.reverse();
        let opening = first.first().map(|t| t.id.clone());
        let mut projected = dedupe_turns(first.into_iter().chain(recent).collect());
        if projected.last().is_some_and(|t| t.items.is_empty()) {
            let target = projected.last().map(|t| t.id.clone());
            let _opening_priority = opening;
            if let Ok(full) = fetch_turns(&mut socket, thread.id(), "desc", 1, "full")
                && let Some(mut full) = full
                    .into_iter()
                    .next()
                    .filter(|t| !t.items.is_empty() && Some(&t.id) == target.as_ref())
                && let Some(old) = projected.iter_mut().find(|t| t.id == full.id)
            {
                full.dropped_items += old.dropped_items;
                *old = full;
            }
        }
        let rollout_path = text(meta.get("path"));
        let events = read_rollout_tail(home, thread, rollout_path.as_deref());
        let turns = append_rollout_tail(&projected, thread.id(), state.as_deref(), &events);
        let tail_events = turns
            .iter()
            .find(|t| t.fresh_tail == Some(true))
            .map(|t| t.items.len())
            .unwrap_or(0);
        let updated = epoch_ms(meta.get("updatedAt"));
        let newest = newest_turn_ms(&projected);
        let behind = updated
            .filter(|_| newest > 0)
            .map(|n| n.saturating_sub(newest).max(0));
        let assembly = assemble_brief_context(&turns);
        let mut context = SupervisorBriefContext {
            source_id: thread.source().as_str().into(),
            machine: machine.into(),
            thread_id: thread.id().into(),
            title: text(meta.get("name")).or_else(|| text(meta.get("preview"))),
            cwd: text(meta.get("cwd")),
            model: text(meta.get("model")),
            provider: text(meta.get("modelProvider")),
            effort: text(meta.get("reasoningEffort")),
            state,
            active_flags,
            updated_at_ms: updated,
            rollout_path,
            turns: assembly.turns,
            transcript_available: assembly.transcript_available,
            truncated: assembly.truncated,
            redactions: assembly.redactions,
            context_bytes: assembly.context_bytes,
            rollout_tail_events: tail_events,
            projected_history_behind_ms: behind,
        };
        sanitize_context_metadata(&mut context);
        Ok(context)
    })();
    // A failed newest-full read may already have dropped its socket; closing
    // cannot replace the recorded-summary fallback with a transport failure.
    let _ = futures_executor::block_on(socket.close());
    result
}
