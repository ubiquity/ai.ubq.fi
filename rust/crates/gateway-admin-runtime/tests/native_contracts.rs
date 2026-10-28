use gateway_admin_contracts::{operations::AdminHost, routes::AdminOperation};
use gateway_admin_runtime::AdminRuntime;
use gateway_admin_runtime_contracts::{
    RuntimeConfig,
    capabilities::{
        CaptureRequest, Context, RuntimeCapabilities, SupervisorBriefTarget, UnboundCapabilities,
    },
};
use gateway_admin_test_support::{
    runtime_fixtures::{WireBody, input, runtime},
    runtime_host::{Host, run},
    runtime_store::Store,
};
use gateway_auth_contracts::types::AuthResponse;
use gateway_auth_native_wire::native_read::NativeReadRequest;
use gateway_source::error::GatewayError;
use serde_json::{Value, json};
use std::cell::RefCell;
type TestResult = Result<(), Box<dyn std::error::Error>>;

#[test]
fn migration_and_replay_preserve_raw_source_modes() {
    let mut request = input(json!("{\"v\":1}\n"));
    request.query.insert("profile".into(), vec!["local".into()]);
    let plan = gateway_admin_runtime_contracts::parse::migration(&request).unwrap();
    assert!(plan.dry_run && plan.include_legacy);
    request.query.insert("write".into(), vec!["true".into()]);
    request.query.insert("dry_run".into(), vec!["true".into()]);
    assert_eq!(
        gateway_admin_runtime_contracts::parse::migration(&request)
            .unwrap_err()
            .status,
        400
    );
    let mut request = input(Value::Null);
    request
        .query
        .insert("request_id".into(), vec!["request-1".into()]);
    request
        .query
        .insert("before_ms".into(), vec!["garbage".into()]);
    assert_eq!(
        gateway_admin_runtime_contracts::parse::capture(&request, false).unwrap(),
        CaptureRequest::Request {
            request_id: "request-1".into()
        }
    );
    request.query.insert(
        "incident_id".into(),
        vec!["provider-12345678-1234-4123-8123-123456789abc".into()],
    );
    assert_eq!(
        gateway_admin_runtime_contracts::parse::capture(&request, false)
            .unwrap_err()
            .status,
        400
    );
}
#[test]
fn missing_native_and_provider_bindings_cannot_return_success() {
    let store = Store::default();
    let host = Host::default();
    let config = RuntimeConfig::default();
    let service = AdminRuntime {
        store: &store,
        host: &host,
        capabilities: &UnboundCapabilities,
        config: &config,
    };
    for op in [
        AdminOperation::Providers,
        AdminOperation::ClaudeAuthGet,
        AdminOperation::ModelsRefresh,
        AdminOperation::SupervisorSessions,
    ] {
        assert_eq!(
            run(service.execute(op, input(Value::Null)))
                .unwrap_err()
                .status,
            503
        )
    }
    let mut request = input(Value::Null);
    request.query.insert("source".into(), vec!["source".into()]);
    request.query.insert("id".into(), vec!["thread".into()]);
    assert_eq!(
        run(service.execute(AdminOperation::SupervisorOutput, request))
            .unwrap_err()
            .envelope
            .error
            .code
            .as_deref(),
        Some("admin_capability_unavailable")
    );
}
#[test]
fn errors_limit_does_not_truncate_five_xx_buckets() {
    let store = Store::default();
    let host = Host::default();
    let config = RuntimeConfig::default();
    let service = runtime(&store, &host, &config);
    for i in 0..205 {
        store.put(json!(["uos_ai","admin_error_log","v1",900000+i,format!("req-{i}")]),json!({"version":1,"request_id":format!("req-{i}"),"route":"/v1/responses","status":502,"provider":"codex","model":null,"reasoning":null,"stream":true,"terminal_type":"error","failure_kind":"upstream","delivery_outcome":"unobserved","created_at_ms":900000+i,"latency_ms":100,"git_sha":null,"deno_revision":null}));
    }
    let mut request = input(Value::Null);
    request.query.insert("limit".into(), vec!["1".into()]);
    let response = run(service.execute(AdminOperation::Errors, request)).unwrap();
    assert_eq!(
        response.body.wire_json()["data"].as_array().unwrap().len(),
        1
    );
    assert_eq!(
        response.body.wire_json()["five_xx_buckets"][0]["count"],
        205
    );
}

#[test]
fn kernel_usage_daily_series_uses_source_thirty_day_utc_window() {
    let store = Store::default();
    let host = Host::default();
    let config = RuntimeConfig::default();
    let service = runtime(&store, &host, &config);
    store.put(
        json!(["ubq_ai", "kernel_auth", "org_usage", "owner"]),
        json!({"owner":"owner","total_requests":5,"last_seen_at_ms":900000}),
    );
    store.put(json!(["ubq_ai","kernel_auth","org_usage_daily","owner"]),json!({"owner":"owner","days":[{"day":"1970-01-01","request_count":5}],"updated_at_ms":900000}));
    let mut request = input(Value::Null);
    request.query.insert("scope".into(), vec!["org".into()]);
    request.query.insert("owner".into(), vec!["owner".into()]);
    request
        .query
        .insert("include_usage".into(), vec!["true".into()]);
    let result = run(service.execute(AdminOperation::KernelUsageGet, request)).unwrap();
    let body = result.body.wire_json();
    let series = body["usage"]["daily_requests"].as_array().unwrap();
    assert_eq!(series.len(), 30);
    assert_eq!(series[29], 5);
    assert_eq!(result.body.wire_json()["usage"]["input_tokens"], 0);
}

#[test]
fn supervisor_follow_dispatches_only_validated_finite_identifiers() -> TestResult {
    struct Follow(RefCell<Option<(String, String, String)>>);
    impl RuntimeCapabilities for Follow {
        async fn native(
            &self,
            request: NativeReadRequest,
            _: Context,
        ) -> Result<AuthResponse, GatewayError> {
            let NativeReadRequest::SupervisorOutput {
                source_id,
                thread_id,
                cursor,
            } = request
            else {
                panic!("wrong finite operation")
            };
            self.0
                .replace(Some((source_id, thread_id, cursor.unwrap_or_default())));
            Err(GatewayError::unavailable(
                "Native source is offline",
                "fixture_native_source_offline",
            ))
        }
    }
    let store = Store::default();
    let host = Host::default();
    let config = RuntimeConfig::default();
    let follow = Follow(RefCell::new(None));
    let service = AdminRuntime {
        store: &store,
        host: &host,
        capabilities: &follow,
        config: &config,
    };
    let mut request = input(Value::Null);
    request
        .query
        .insert("source".into(), vec![" trusted-source ".into()]);
    request.query.insert("id".into(), vec!["thread-1".into()]);
    request
        .query
        .insert("cursor".into(), vec![format!(" {} ", "a".repeat(250))]);
    let error = run(service.execute(AdminOperation::SupervisorOutput, request))
        .err()
        .ok_or("expected native source failure")?;
    assert_eq!(
        error.envelope.error.code.as_deref(),
        Some("fixture_native_source_offline")
    );
    assert_eq!(
        *follow.0.borrow(),
        Some(("trusted-source".into(), "thread-1".into(), "a".repeat(240)))
    );
    Ok(())
}

#[test]
fn shared_native_follow_descriptor_is_preserved_and_json_success_is_rejected() -> TestResult {
    use gateway_auth_native_wire::native_read::AuthStream;
    enum Reply {
        Prepared,
        JsonOnly,
        WrongSource,
    }
    struct Native(Reply);
    impl RuntimeCapabilities for Native {
        async fn native(
            &self,
            request: NativeReadRequest,
            _: Context,
        ) -> Result<AuthResponse, GatewayError> {
            let NativeReadRequest::SupervisorOutput {
                source_id,
                thread_id,
                cursor,
            } = request
            else {
                panic!("wrong finite operation")
            };
            let response = AuthResponse::json(200, Value::Null);
            Ok(match self.0 {
                Reply::Prepared => response.with_stream(AuthStream::SupervisorFollow {
                    source_id,
                    thread_id,
                    cursor,
                }),
                Reply::JsonOnly => response,
                Reply::WrongSource => response.with_stream(AuthStream::SupervisorFollow {
                    source_id: "unselected-source".into(),
                    thread_id,
                    cursor,
                }),
            })
        }
    }
    let store = Store::default();
    let host = Host::default();
    let config = RuntimeConfig::default();
    for reply in [Reply::Prepared, Reply::JsonOnly, Reply::WrongSource] {
        let prepared = matches!(reply, Reply::Prepared);
        let native = Native(reply);
        let service = AdminRuntime {
            store: &store,
            host: &host,
            capabilities: &native,
            config: &config,
        };
        let mut request = input(Value::Null);
        request
            .query
            .insert("source".into(), vec!["trusted-source".into()]);
        request.query.insert("id".into(), vec!["thread".into()]);
        let result = run(service.execute(AdminOperation::SupervisorOutput, request));
        if prepared {
            assert_eq!(
                result?.stream,
                Some(AuthStream::SupervisorFollow {
                    source_id: "trusted-source".into(),
                    thread_id: "thread".into(),
                    cursor: None
                })
            );
        } else {
            assert_eq!(
                result
                    .err()
                    .ok_or("expected invalid native protocol response")?
                    .envelope
                    .error
                    .code
                    .as_deref(),
                Some("native_protocol_invalid")
            );
        }
    }
    Ok(())
}

#[test]
fn public_supervisor_brief_uses_final_operation_and_never_leaks_native_context() {
    struct Brief {
        raw_context: bool,
    }
    impl RuntimeCapabilities for Brief {
        async fn supervisor_brief(
            &self,
            target: SupervisorBriefTarget,
            context: Context,
        ) -> Result<AuthResponse, GatewayError> {
            let body = if self.raw_context {
                json!({"sourceId":target.source_id,"threadId":target.thread_id,"turns":[{"text":"private recorded context"}]})
            } else {
                json!({"sourceId":target.source_id,"threadId":target.thread_id,"machine":"fixture machine","about":"No recorded transcript was available for this session.","status":"Current runtime state is active.","model":null,"effort":null,"state":"active","sampledAtMs":context.now_ms,"generatedAtMs":context.now_ms,"transcript":{"turns":0,"bytes":0,"truncated":false,"redactions":0,"available":false}})
            };
            Ok(AuthResponse::json(200, body))
        }
    }
    let store = Store::default();
    let host = Host::default();
    let config = RuntimeConfig::default();
    for raw_context in [false, true] {
        let capability = Brief { raw_context };
        let service = AdminRuntime {
            store: &store,
            host: &host,
            capabilities: &capability,
            config: &config,
        };
        let response = run(service.execute(
            AdminOperation::SupervisorBrief,
            input(json!({"source":"trusted-source","id":"thread"})),
        ));
        if raw_context {
            assert_eq!(
                response.unwrap_err().envelope.error.code.as_deref(),
                Some("supervisor_brief_invalid_response")
            );
        } else {
            let response = response.unwrap();
            assert_eq!(response.status, 200);
            assert_eq!(response.body.wire_json()["model"], Value::Null);
            assert!(response.body.wire_json().get("turns").is_none());
        }
    }
}
