//! Complete readonly native supervisor operation, bound to the original authenticated request.
use gateway_auth_native_proof::native_auth::{AuthenticatedNativeOwner, NativeCapability};
use gateway_auth_native_wire::native_read::{
    AuthStream, NATIVE_FOLLOW_PATH, NATIVE_MAX_BODY_BYTES, NATIVE_RPC_PATH, NativeReadRequest,
    NativeRpcEnvelope, NativeRpcResponse,
};
use gateway_native_http_framing::{
    request::HttpRequest,
    response::{disconnected, now_ms, write_json},
};
use gateway_native_local_transport::unix::UnixSupervisor;
use gateway_native_supervisor::supervisor::{
    FOLLOW_MAX_DURATION_MS, FOLLOW_POLL_MS, FollowUpdate, follow_update,
};
use gateway_native_supervisor_data::data::SupervisorData;
use gateway_source::error::GatewayError;
use serde_json::{Value, json};
use std::{
    collections::BTreeMap,
    io::Write,
    net::TcpStream,
    time::{Duration, Instant},
};

pub fn receive_supervisor_request(
    stream: &mut TcpStream,
    request: &HttpRequest,
    proof: &AuthenticatedNativeOwner,
    data: &SupervisorData,
) -> Result<(), GatewayError> {
    if request.method != "POST"
        || !matches!(request.path.as_str(), NATIVE_RPC_PATH | NATIVE_FOLLOW_PATH)
    {
        return Err(GatewayError::new(
            404,
            "Unknown native operation",
            Some("native_route_invalid"),
        ));
    }
    proof.require_request(
        NativeCapability::SupervisorRead,
        &request.path,
        &request.body,
        now_ms()?,
    )?;
    if request.path == NATIVE_RPC_PATH {
        read_rpc(data, stream, request)
    } else {
        follow(data, stream, request)
    }
}
fn read_rpc(
    data: &SupervisorData,
    stream: &mut TcpStream,
    request: &HttpRequest,
) -> Result<(), GatewayError> {
    let envelope: NativeRpcEnvelope =
        serde_json::from_slice(&request.body).map_err(|_| invalid())?;
    envelope.validate()?;
    let result = (|| -> Result<NativeRpcResponse, GatewayError> {
        Ok(match &envelope.request {
            NativeReadRequest::SupervisorSessions => NativeRpcResponse {
                v: 1,
                status: 200,
                body: data.snapshot(now_ms()?)?,
                stream: None,
            },
            NativeReadRequest::SupervisorOutput {
                source_id,
                thread_id,
                cursor,
            } => {
                data.select(source_id, thread_id, now_ms()?)?;
                NativeRpcResponse {
                    v: 1,
                    status: 200,
                    body: Value::Null,
                    stream: Some(AuthStream::SupervisorFollow {
                        source_id: source_id.clone(),
                        thread_id: thread_id.clone(),
                        cursor: cursor.clone(),
                    }),
                }
            }
            NativeReadRequest::SupervisorBrief {
                source_id,
                thread_id,
            } => {
                let selected = data.select(source_id, thread_id, now_ms()?)?;
                let context = data.brief_context(&selected, now_ms()?)?;
                NativeRpcResponse {
                    v: 1,
                    status: 200,
                    body: json!({"sampledAtMs":selected.sampled_at_ms(),"context":context}),
                    stream: None,
                }
            }
        })
    })();
    let result = match result {
        Ok(response) => response,
        Err(error) => NativeRpcResponse {
            v: 1,
            status: error.status,
            body: serde_json::to_value(&error.envelope).map_err(|_| unavailable())?,
            stream: None,
        },
    };
    result.validate_for(&envelope)?;
    let body = serde_json::to_vec(&result).map_err(|_| unavailable())?;
    if body.len() > NATIVE_MAX_BODY_BYTES {
        return Err(unavailable());
    }
    write_json(stream, 200, &body)
}
fn follow(
    data: &SupervisorData,
    stream: &mut TcpStream,
    request: &HttpRequest,
) -> Result<(), GatewayError> {
    let descriptor: AuthStream = serde_json::from_slice(&request.body).map_err(|_| invalid())?;
    let AuthStream::SupervisorFollow {
        source_id,
        thread_id,
        cursor,
    } = descriptor;
    let selected = data.select(&source_id, &thread_id, now_ms()?)?;
    if cursor.as_ref().is_some_and(|value| value.len() > 240) {
        return Err(invalid());
    }
    stream.write_all(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\nCache-Control: no-store\r\nX-Accel-Buffering: no\r\nConnection: close\r\n\r\n").map_err(|_|unavailable())?;
    if send_event(stream,"ready",json!({"sourceId":source_id,"threadId":thread_id,"machine":selected.machine(),"mode":"recorded","note":"Updates as Codex records output (about every 2s), not a token-level stream. Only assistant messages and command output are shown."})).is_err(){return Ok(());}
    let started = Instant::now();
    let mut revisions = BTreeMap::new();
    let mut cursor = cursor.unwrap_or_default();
    let mut polls = 0u64;
    let mut unavailable_polls = 0u64;
    let mut last_unavailable = String::new();
    while started.elapsed().as_millis() < FOLLOW_MAX_DURATION_MS as u128 {
        if disconnected(stream) {
            return Ok(());
        }
        let update = UnixSupervisor::configured_socket_until(
            selected.configured_socket(),
            Instant::now() + Duration::from_secs(4),
        )
        .and_then(|mut transport| {
            futures_executor::block_on(follow_update(
                &mut transport,
                selected.known_thread(),
                &cursor,
                &mut revisions,
            ))
        });
        let event = follow_event(
            update,
            &mut cursor,
            &mut polls,
            &mut unavailable_polls,
            &mut last_unavailable,
        )?;
        if event.is_some_and(|(name, value)| send_event(stream, name, value).is_err()) {
            return Ok(());
        }
        let until = Instant::now() + Duration::from_millis(FOLLOW_POLL_MS);
        while Instant::now() < until {
            if disconnected(stream) {
                return Ok(());
            }
            std::thread::sleep(Duration::from_millis(50));
        }
    }
    let _ = send_event(
        stream,
        "end",
        json!({"reason":"follow duration limit reached"}),
    );
    Ok(())
}
fn follow_event(
    update: Result<FollowUpdate, GatewayError>,
    cursor: &mut String,
    polls: &mut u64,
    unavailable_polls: &mut u64,
    last_unavailable: &mut String,
) -> Result<Option<(&'static str, Value)>, GatewayError> {
    Ok(match update {
        Ok(update) => {
            *cursor = update.cursor.clone();
            *polls += 1;
            if !update.entries.is_empty() {
                let entries=update.entries.iter().map(|entry|json!({"key":entry.key,"revision":entry.revision,"kind":entry.kind,"turnId":entry.turn_id,"text":entry.text,"command":entry.command,"status":entry.status,"exitCode":entry.exit_code,"atMs":entry.at_ms})).collect::<Vec<_>>();
                Some((
                    "entries",
                    json!({"entries":entries,"cursor":*cursor,"turnId":update.turn_id,"turnStatus":update.turn_status,"enriched":update.enriched,"sampledAtMs":now_ms()?}),
                ))
            } else if polls.is_multiple_of(5) {
                Some((
                    "heartbeat",
                    json!({"turnId":update.turn_id,"turnStatus":update.turn_status,"sampledAtMs":now_ms()?}),
                ))
            } else {
                None
            }
        }
        Err(error) => {
            *unavailable_polls += 1;
            let message = error.envelope.error.message;
            if message != *last_unavailable || unavailable_polls.is_multiple_of(5) {
                *last_unavailable = message.clone();
                Some((
                    "unavailable",
                    json!({"message":message,"sampledAtMs":now_ms()?}),
                ))
            } else {
                None
            }
        }
    })
}
fn send_event(stream: &mut TcpStream, name: &str, value: Value) -> Result<(), GatewayError> {
    let body = serde_json::to_string(&value).map_err(|_| unavailable())?;
    stream
        .write_all(format!("event: {name}\ndata: {body}\n\n").as_bytes())
        .map_err(|_| unavailable())?;
    stream.flush().map_err(|_| unavailable())
}
fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Native operation is unavailable",
        "native_operation_unavailable",
    )
}
fn invalid() -> GatewayError {
    GatewayError::new(
        400,
        "Invalid native HTTP request",
        Some("native_request_invalid"),
    )
}
