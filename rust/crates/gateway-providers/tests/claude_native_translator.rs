//! Source f20e3d6, ec4b326 and 59af8eb: native Claude Messages tool, structured
//! output and hosted web-search parity, ported from the TypeScript fixtures.
use gateway_core::{provider::UpstreamProvider, source_json::SourceJson};
use gateway_providers::{
    finalize::CLAUDE_ATTRIBUTION,
    response::decode_response,
    source_request::prepare_source_request,
    types::{
        AuthMaterial, ClaudeBodyOptions, ClaudeCredentialKind, ClientSurface, Headers,
        PrepareOptions, PreparedClaudeBody, PreparedRequest,
    },
};
use serde_json::{Value, json};

type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;
const MODEL: &str = "claude-sonnet-4-5";

fn source(value: &Value) -> TestResult<SourceJson> {
    Ok(SourceJson::from_serializable(value)?)
}
fn scalar(value: &SourceJson) -> TestResult<Value> {
    Ok(serde_json::from_str(value.as_str())?)
}
fn base(extra: Value) -> Value {
    let mut body = json!({"model":MODEL,"input":"hello","max_output_tokens":128});
    for (key, value) in extra.as_object().into_iter().flatten() {
        body[key] = value.clone();
    }
    body
}
/// TS `buildSurplusClaudeRequest` without `omitHostedWebSearch`: the OpenLux native lane.
fn openlux(extra: Value) -> TestResult<PreparedRequest> {
    let options = PrepareOptions {
        auth: AuthMaterial {
            secret: "fixture-key".into(),
            ..Default::default()
        },
        ..Default::default()
    };
    let raw = source(&base(extra))?;
    let prepared = prepare_source_request(
        UpstreamProvider::Metered,
        ClientSurface::Responses,
        &raw,
        &MODEL.into(),
        &options,
    );
    prepared.map_err(|error| format!("{error:?}").into())
}
/// The native Messages body, or None when the request keeps the Responses transport.
fn openlux_native(extra: Value) -> TestResult<Option<Value>> {
    let prepared = openlux(extra)?;
    if prepared.url != "https://api.openlux.ai/v1/messages" {
        return Ok(None);
    }
    Ok(Some(serde_json::from_str(prepared.body.as_str())?))
}
/// TS `buildClaudeCodeRequest`: the direct subscription lane.
fn direct(raw: Value, kind: ClaudeCredentialKind, efforts: &[&str]) -> TestResult<Value> {
    let mut options = ClaudeBodyOptions::new(kind);
    options.native_reasoning_efforts = Some(efforts.iter().map(|value| (*value).into()).collect());
    let prepared = PreparedClaudeBody::prepare(
        ClientSurface::Responses,
        &source(&raw)?,
        &MODEL.into(),
        &options,
    )
    .map_err(|error| format!("{error:?}"))?;
    scalar(prepared.body())
}
fn clock_namespace() -> Value {
    json!({"type":"namespace","name":"clock","description":"Tools for reading time.","tools":[
        {"type":"function","name":"curr_time","description":"Current time","parameters":{"type":"object","properties":{},"additionalProperties":false}}
    ]})
}
fn disabled_search() -> Value {
    json!({"type":"web_search","external_web_access":false,"user_location":{"type":"approximate","country":"US"}})
}
fn enabled_search() -> Value {
    json!({"type":"web_search","external_web_access":true,"user_location":{"type":"approximate","country":"US"}})
}
fn names(values: &Value, key: &str) -> Vec<Value> {
    let values = values.as_array().into_iter().flatten();
    values.map(|value| value[key].clone()).collect()
}

#[test]
fn flattens_namespace_tools_and_omits_disabled_hosted_web_search() -> TestResult {
    let prepared = openlux(json!({
        "input":[
            {"role":"user","content":"What time is it?"},
            {"type":"function_call","name":"curr_time","namespace":"clock","call_id":"call-1","arguments":"{}"},
            {"type":"function_call_output","call_id":"call-1","output":"2026-11-04T22:00:00Z"}
        ],
        "tools":[
            clock_namespace(),
            {"type":"function","name":"read_file","parameters":{"type":"object","properties":{"path":{"type":"string"}}}},
            disabled_search()
        ],
        "tool_choice":{"type":"function","name":"curr_time","namespace":"clock"},
        "parallel_tool_calls":false
    }))?;
    assert_eq!(prepared.url, "https://api.openlux.ai/v1/messages");
    let body = scalar(&prepared.body)?;
    assert_eq!(
        names(&body["tools"], "name"),
        [json!("clock_curr_time"), json!("read_file")]
    );
    let map = &prepared.decode.tool_names;
    let clock = map
        .get("clock_curr_time")
        .ok_or("missing clock_curr_time")?;
    assert_eq!(clock.name.try_utf8()?, "curr_time");
    assert_eq!(
        clock
            .namespace
            .as_ref()
            .map(|ns| ns.try_utf8())
            .transpose()?,
        Some("clock".into())
    );
    let read = map.get("read_file").ok_or("missing read_file")?;
    assert_eq!(read.name.try_utf8()?, "read_file");
    assert!(read.namespace.is_none() && !read.custom);
    assert_eq!(
        body["tool_choice"],
        json!({"type":"tool","name":"clock_curr_time","disable_parallel_tool_use":true})
    );
    assert_eq!(body["messages"][1]["content"][0]["name"], "clock_curr_time");
    assert!(!prepared.body.as_str().contains("web_search"));
    Ok(())
}

#[test]
fn namespace_projection_fails_closed_like_the_source() -> TestResult {
    for extra in [
        // Collision between a flattened namespace member and an ordinary function.
        json!({"tools":[clock_namespace(),{"type":"function","name":"clock_curr_time"}]}),
        json!({"tools":[{"type":"namespace","name":"functions","tools":[]}]}),
        json!({"tools":[{"type":"namespace","name":"clock","tools":[{"type":"custom","name":"x"}]}]}),
        json!({"tools":[{"type":"custom","name":"apply_patch"}]}),
        json!({"tools":[{"type":"function","name":"run","strict":true}]}),
        json!({"tools":[enabled_search()]}),
        // A namespaced history call needs a declared projection.
        json!({"input":[{"type":"function_call","name":"curr_time","namespace":"clock","call_id":"c","arguments":"{}"}]}),
    ] {
        assert!(openlux_native(extra.clone())?.is_none(), "{extra}");
    }
    // A custom forced choice is projected like a function choice.
    let body = openlux_native(json!({"tools":[clock_namespace()],
        "tool_choice":{"type":"custom","name":"curr_time","namespace":"clock"}}))?
    .ok_or("custom choice must stay native")?;
    assert_eq!(
        body["tool_choice"],
        json!({"type":"tool","name":"clock_curr_time"})
    );
    Ok(())
}

#[test]
fn preserves_late_developer_messages_in_the_native_system_channel() -> TestResult {
    let body = openlux_native(json!({"input":[
        {"role":"developer","content":"first instruction"},
        {"role":"user","content":"question"},
        {"role":"developer","content":"later reminder"}
    ]}))?
    .ok_or("late developer text must stay native")?;
    assert_eq!(
        body["system"],
        json!([{"type":"text","text":"first instruction"},{"type":"text","text":"later reminder"}])
    );
    assert_eq!(
        body["messages"],
        json!([{"role":"user","content":[{"type":"text","text":"question","cache_control":{"type":"ephemeral"}}]}])
    );
    Ok(())
}

#[test]
fn accepts_the_codex_concise_reasoning_summary_preference() -> TestResult {
    let raw = base(json!({"reasoning":{"effort":"high","summary":"concise"}}));
    let body = direct(raw, ClaudeCredentialKind::ApiKey, &["high"])?;
    assert_eq!(body["thinking"], json!({"type":"adaptive"}));
    assert_eq!(body["output_config"], json!({"effort":"high"}));
    Ok(())
}

#[test]
fn carries_json_schema_structured_output_with_adaptive_reasoning() -> TestResult {
    let schema = json!({"type":"object","properties":{"title":{"type":"string","minLength":1,"maxLength":36}},"required":["title"],"additionalProperties":false});
    let raw = base(json!({"reasoning":{"effort":"high","summary":"concise"},
        "text":{"format":{"type":"json_schema","strict":true,"schema":schema,"name":"codex_output_schema"}}}));
    let body = direct(raw, ClaudeCredentialKind::ApiKey, &["high"])?;
    assert_eq!(
        body["output_config"],
        json!({"effort":"high","format":{"type":"json_schema","schema":schema}})
    );
    Ok(())
}

#[test]
fn openlux_native_messages_preserves_json_schema_output_config() -> TestResult {
    let schema = json!({"type":"object","properties":{"title":{"type":"string"}},"required":["title"],"additionalProperties":false});
    let body = openlux_native(json!({"input":"Generate a title",
        "text":{"format":{"type":"json_schema","strict":true,"name":"answer","schema":schema}}}))?
    .ok_or("JSON-schema output must stay native")?;
    assert_eq!(
        body["output_config"],
        json!({"format":{"type":"json_schema","schema":schema}})
    );
    Ok(())
}

#[test]
fn default_text_format_is_no_structured_output_on_the_direct_lane() -> TestResult {
    let text = json!({"format":{"type":"text"}});
    let raw = base(json!({"reasoning":{"effort":"high"},"text":text}));
    let body = direct(raw, ClaudeCredentialKind::ApiKey, &["high"])?;
    assert_eq!(body["output_config"], json!({"effort":"high"}));
    let body = direct(
        base(json!({"text":text})),
        ClaudeCredentialKind::ApiKey,
        &["high"],
    )?;
    assert_eq!(body.get("output_config"), None);
    let unknown = base(json!({"text":{"format":{"type":"json_object"}}}));
    assert!(direct(unknown, ClaudeCredentialKind::ApiKey, &["high"]).is_err());
    Ok(())
}

#[test]
fn openlux_native_messages_treat_the_default_text_format_as_absent() -> TestResult {
    let text = json!({"format":{"type":"text"}});
    let body =
        openlux_native(json!({"text":text}))?.ok_or("default text format must stay native")?;
    assert_eq!(body.get("output_config"), None);
    let plain = openlux_native(json!({}))?.ok_or("plain request must stay native")?;
    assert_eq!(body, plain);
    // OpenLux disables native effort, so effort keeps Responses whether or not the default format is present.
    let with_effort = openlux(json!({"reasoning":{"effort":"high"},"text":text}))?;
    assert_eq!(
        with_effort.url,
        openlux(json!({"reasoning":{"effort":"high"}}))?.url
    );
    Ok(())
}

#[test]
fn unrepresentable_text_formats_keep_the_original_responses_request() -> TestResult {
    for format in [
        json!({"type":"json_schema"}),
        json!("text"),
        json!({"type":"json_object"}),
        json!({"type":"text","schema":{"type":"object"}}),
        json!({"type":"json_schema","schema":{"type":"object"},"strict":"yes"}),
        json!({"type":"json_schema","schema":{"type":"object"},"name":7}),
    ] {
        let prepared = openlux(json!({"text":{"format":format}}))?;
        assert_eq!(
            prepared.url, "https://api.openlux.ai/v1/responses",
            "{format}"
        );
    }
    Ok(())
}

#[test]
fn direct_claude_code_omits_enabled_hosted_web_search_while_reseller_adapters_reject_it()
-> TestResult {
    let extra = json!({"tools":[enabled_search()]});
    let body = direct(base(extra.clone()), ClaudeCredentialKind::ApiKey, &[])?;
    assert!(body.get("tools").is_none());
    assert!(openlux_native(extra)?.is_none());
    Ok(())
}

#[test]
fn codex_tui_request_omits_enabled_search_and_serves_namespaced_tools_with_late_developer_context()
-> TestResult {
    let raw = json!({
        "model":MODEL,"stream":true,"store":false,"prompt_cache_key":"tui-conversation",
        "reasoning":{"summary":"concise"},"include":["reasoning.encrypted_content"],
        "parallel_tool_calls":true,"tool_choice":"auto",
        "tools":[
            {"type":"namespace","name":"clock","description":"Tools for reading time.","tools":[
                {"type":"function","name":"curr_time","strict":false,"parameters":{"type":"object","properties":{},"additionalProperties":false}}
            ]},
            enabled_search()
        ],
        "input":[
            {"type":"message","role":"developer","content":[{"type":"input_text","text":"global instructions"}]},
            {"type":"message","role":"user","content":[{"type":"input_text","text":"Reply with TUI_READY"}]},
            {"type":"message","role":"developer","content":[{"type":"input_text","text":"later hook reminder"}]}
        ]
    });
    let body = direct(raw, ClaudeCredentialKind::Oauth, &[])?;
    assert_eq!(
        body["tools"],
        json!([{"name":"clock_curr_time","input_schema":{"type":"object","properties":{},"additionalProperties":false}}])
    );
    assert!(!body.to_string().contains("web_search"));
    assert_eq!(
        names(&body["system"], "text"),
        [
            json!(CLAUDE_ATTRIBUTION),
            json!("global instructions"),
            json!("later hook reminder")
        ]
    );
    Ok(())
}

#[test]
fn anthropic_responses_restores_codex_namespace_tool_names() -> TestResult {
    let mut options = ClaudeBodyOptions::new(ClaudeCredentialKind::ApiKey);
    options.native_reasoning_efforts = Some(vec![]);
    let raw = source(&base(json!({"tools":[clock_namespace()]})))?;
    let prepared =
        PreparedClaudeBody::prepare(ClientSurface::Responses, &raw, &MODEL.into(), &options)
            .map_err(|error| format!("{error:?}"))?;
    let upstream = json!({"id":"msg_1","type":"message","role":"assistant","model":MODEL,
        "content":[{"type":"tool_use","id":"toolu_1","name":"clock_curr_time","input":{}}],
        "stop_reason":"tool_use","usage":{"input_tokens":4,"output_tokens":2}});
    let headers = Headers::from([("content-type".into(), "application/json".into())]);
    let bytes = upstream.to_string();
    let decoded = decode_response(prepared.decode(), 200, &headers, bytes.as_bytes())?;
    let output = &scalar(&decoded.body)?["output"][0];
    assert_eq!(output["type"], "function_call");
    assert_eq!(output["name"], "curr_time");
    assert_eq!(output["namespace"], "clock");
    Ok(())
}
