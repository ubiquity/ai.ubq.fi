//! Runtime-facing Claude native replay publication and PrepareOptions restoration.
use gateway_core::model::ModelId;
use gateway_core::provider::UpstreamProvider;
use gateway_core::source_json::SourceJson;
use gateway_provider_protocol::{types::AuthMaterial, types::ClientSurface, types::PrepareOptions};
use gateway_provider_tool_grammar::source_tools::ToolMap;
use gateway_providers::source_request::prepare_source_request;
use serde_json::{Value, json};

fn scalar(value: &SourceJson) -> Value {
    value.deserialize().unwrap()
}

fn source(value: &Value) -> SourceJson {
    SourceJson::from_serializable(value).unwrap()
}

fn sources(values: &[Value]) -> Vec<SourceJson> {
    values.iter().map(source).collect()
}

fn context() -> gateway_provider_replay::replay::ReplayContext {
    gateway_provider_replay::replay::ReplayContext {
        scope: "key-a".into(),
        provider: "surplus".into(),
        model: "claude-fixture".into(),
    }
}

fn messages_output() -> Vec<Value> {
    vec![
        json!({"id":"rs_resp_replay_0","type":"reasoning","summary":[{"type":"summary_text","text":"thought"}]}),
        json!({"id":"msg_resp_replay_1","type":"message","role":"assistant","content":[{"type":"output_text","text":"answer"}]}),
        json!({"id":"fc_resp_replay_2","type":"function_call","call_id":"call_1","name":"read_file","arguments":"{\"path\":\"a.txt\"}"}),
    ]
}

fn raw_request(output: &[Value]) -> Value {
    let mut input = vec![
        json!({"type":"message","role":"user","content":[{"type":"input_text","text":"read"}]}),
    ];
    input.extend(output.iter().cloned());
    input.push(json!({"type":"function_call_output","call_id":"call_1","output":[{"type":"input_text","text":"file"}]}));
    input.push(
        json!({"type":"message","role":"user","content":[{"type":"input_text","text":"continue"}]}),
    );
    json!({
        "model": "claude-fixture",
        "max_output_tokens": 16,
        "input": input,
        "tools": [{"type":"function","name":"read_file","parameters":{"type":"object"}}]
    })
}

#[test]
fn decoded_messages_replay_publishes_and_restores_through_prepare_options() -> Result<(), String> {
    let context = context();
    let output = messages_output();
    let native = json!({"role":"assistant","content":[
        {"type":"thinking","thinking":"thought","signature":"private-signature"},
        {"type":"text","text":"answer"},
        {"type":"tool_use","id":"call_1","name":"read_file","input":{"path":"a.txt"}}
    ]});
    let mut store = gateway_provider_replay::replay::ReplayStore::default();
    assert!(
        store
            .remember_decoded_native(
                context.clone(),
                &sources(&output),
                source(&native),
                gateway_provider_protocol::replay::ReplayTransport::Messages,
                &ToolMap::new(),
                1,
            )
            .map_err(|error| format!("{error:?}"))?
    );
    let raw = raw_request(&output);
    let raw_source = SourceJson::from_serializable(&raw).map_err(|error| format!("{error:?}"))?;
    let mut options = PrepareOptions::default();
    assert!(
        store
            .restore_prepare_options(
                &context,
                &raw_source,
                &mut options,
                gateway_provider_protocol::replay::ReplayTransport::Messages,
                2,
            )
            .map_err(|error| format!("{error:?}"))?
    );
    let body = scalar(
        &gateway_providers::anthropic::try_native_messages(&raw, &options)
            .map_err(|error| format!("{error:?}"))?
            .ok_or("missing expected fixture value")?,
    );
    assert_eq!(body["messages"][0]["role"], "user");
    assert_eq!(body["messages"][1]["role"], "assistant");
    assert_eq!(body["messages"][1]["content"][0]["type"], "thinking");
    assert_eq!(
        body["messages"][1]["content"][0]["signature"],
        "private-signature"
    );
    assert_eq!(body["messages"][1]["content"][1]["text"], "answer");
    assert_eq!(body["messages"][1]["content"][2]["type"], "tool_use");
    assert_eq!(body["messages"][2]["content"][0]["type"], "tool_result");
    assert_eq!(body["messages"][2]["content"][1]["text"], "continue");

    Ok(())
}

#[test]
fn decoded_chat_replay_keeps_reasoning_details_and_tool_calls_through_prepare_options()
-> Result<(), String> {
    let context = context();
    let output = messages_output();
    let native = json!({
        "role":"assistant",
        "content":"answer",
        "reasoning_details":[{"type":"reasoning.text","text":"thought","signature":"private-chat-signature"}],
        "tool_calls":[{"id":"call_1","type":"function","function":{"name":"read_file","arguments":"{\"path\":\"a.txt\"}"}}]
    });
    let mut store = gateway_provider_replay::replay::ReplayStore::default();
    assert!(
        store
            .remember_decoded_native(
                context.clone(),
                &sources(&output),
                source(&native),
                gateway_provider_protocol::replay::ReplayTransport::Chat,
                &ToolMap::new(),
                1,
            )
            .map_err(|error| format!("{error:?}"))?
    );
    let raw = raw_request(&output);
    let raw_source = SourceJson::from_serializable(&raw).map_err(|error| format!("{error:?}"))?;
    let mut options = PrepareOptions::default();
    assert!(
        store
            .restore_prepare_options(
                &context,
                &raw_source,
                &mut options,
                gateway_provider_protocol::replay::ReplayTransport::Chat,
                2,
            )
            .map_err(|error| format!("{error:?}"))?
    );
    let body = scalar(
        &gateway_providers::anthropic::try_claude_chat(&raw, &options)
            .map_err(|error| format!("{error:?}"))?
            .ok_or("missing expected fixture value")?
            .body,
    );
    assert_eq!(body["messages"][1]["role"], "assistant");
    assert_eq!(body["messages"][1]["content"], "answer");
    assert_eq!(
        body["messages"][1]["reasoning_details"][0]["signature"],
        "private-chat-signature"
    );
    assert_eq!(body["messages"][1]["tool_calls"][0]["id"], "call_1");
    let mut invalid = native;
    invalid["reasoning_details"][0]["signature"] = json!("");
    assert!(
        !gateway_provider_replay::replay::ReplayStore::default()
            .remember_decoded_native(
                context,
                &sources(&output),
                source(&invalid),
                gateway_provider_protocol::replay::ReplayTransport::Chat,
                &ToolMap::new(),
                1,
            )
            .map_err(|error| format!("{error:?}"))?
    );

    Ok(())
}

#[test]
fn lone_surrogate_native_replay_survives_prepare_source_request() -> Result<(), String> {
    let context = context();
    let native =
        SourceJson::parse(r#"{"role":"assistant","content":[{"type":"text","text":"\ud800"}]}"#)
            .map_err(|error| format!("{error:?}"))?;
    let output = vec![
        SourceJson::parse(
            r#"{"id":"msg_resp_lone_0","type":"message","role":"assistant","content":[{"type":"output_text","text":"\ud800"}]}"#,
        )
        .map_err(|error| format!("{error:?}"))?,
    ];
    let raw = SourceJson::parse(
        r#"{"model":"claude-fixture","max_output_tokens":16,"input":[{"type":"message","role":"user","content":[{"type":"input_text","text":"\ud800"}]},{"id":"msg_resp_lone_0","type":"message","role":"assistant","content":[{"type":"output_text","text":"\ud800"}]}]}"#,
    )
    .map_err(|error| format!("{error:?}"))?;
    let mut store = gateway_provider_replay::replay::ReplayStore::default();
    assert!(
        store
            .remember_decoded_native(
                context.clone(),
                &output,
                native,
                gateway_provider_protocol::replay::ReplayTransport::Messages,
                &ToolMap::new(),
                1,
            )
            .map_err(|error| format!("{error:?}"))?
    );
    let mut options = PrepareOptions {
        auth: AuthMaterial {
            secret: "fixture-secret".into(),
            ..Default::default()
        },
        ..Default::default()
    };
    assert!(
        store
            .restore_prepare_options(
                &context,
                &raw,
                &mut options,
                gateway_provider_protocol::replay::ReplayTransport::Messages,
                2,
            )
            .map_err(|error| format!("{error:?}"))?
    );
    let prepared = prepare_source_request(
        UpstreamProvider::Surplus,
        ClientSurface::Responses,
        &raw,
        &ModelId::from_scalar("claude-fixture"),
        &options,
    )
    .map_err(|error| format!("{error:?}"))?;
    let body = prepared.body.as_str();
    assert!(
        body.contains("\\ud800") || body.contains("\\uD800"),
        "{body}"
    );
    assert!(!body.contains("fffd"), "{body}");

    Ok(())
}

#[test]
fn native_replay_arguments_require_matching_valid_object_bytes()
-> Result<(), Box<dyn std::error::Error>> {
    let cases = [
        ("missing", None, false),
        ("null", Some(Value::Null), false),
        ("number", Some(json!(0)), false),
        ("empty", Some(json!("")), false),
        ("malformed", Some(json!("{")), false),
        ("json_null", Some(json!("null")), false),
        (
            "different_object",
            Some(json!(r#"{"path":"other.txt"}"#)),
            false,
        ),
        (
            "different_object_bytes",
            Some(json!(r#"{ "path": "a.txt" }"#)),
            false,
        ),
        ("matching_object", Some(json!(r#"{"path":"a.txt"}"#)), true),
    ];
    for transport in [
        gateway_provider_protocol::replay::ReplayTransport::Messages,
        gateway_provider_protocol::replay::ReplayTransport::Chat,
    ] {
        let native = match transport {
            gateway_provider_protocol::replay::ReplayTransport::Messages => {
                json!({"role":"assistant","content":[
                    {"type":"thinking","thinking":"thought","signature":"private-signature"},
                    {"type":"text","text":"answer"},
                    {"type":"tool_use","id":"call_1","name":"read_file","input":{"path":"a.txt"}}
                ]})
            }
            gateway_provider_protocol::replay::ReplayTransport::Chat => {
                json!({"role":"assistant","content":"answer","reasoning_content":"thought",
                    "tool_calls":[{"id":"call_1","type":"function","function":{"name":"read_file","arguments":"{\"path\":\"a.txt\"}"}}]
                })
            }
        };
        for (label, arguments, expected) in &cases {
            let mut output = messages_output();
            let fields = output[2]
                .as_object_mut()
                .ok_or("missing function-call fixture object")?;
            if let Some(arguments) = arguments {
                fields.insert("arguments".into(), arguments.clone());
            } else {
                assert!(fields.remove("arguments").is_some());
            }
            assert_eq!(
                gateway_provider_replay::replay::ReplayStore::default()
                    .remember_decoded_native(
                        context(),
                        &sources(&output),
                        source(&native),
                        transport,
                        &ToolMap::new(),
                        1,
                    )
                    .map_err(|error| format!("{error:?}"))?,
                *expected,
                "output arguments: {transport:?}/{label}"
            );
        }
        if transport == gateway_provider_protocol::replay::ReplayTransport::Chat {
            for (label, arguments, expected) in &cases {
                let mut changed = native.clone();
                let fields = changed["tool_calls"][0]["function"]
                    .as_object_mut()
                    .ok_or("missing native function fixture")?;
                if let Some(arguments) = arguments {
                    fields.insert("arguments".into(), arguments.clone());
                } else {
                    assert!(fields.remove("arguments").is_some());
                }
                assert_eq!(
                    gateway_provider_replay::replay::ReplayStore::default()
                        .remember_decoded_native(
                            context(),
                            &sources(&messages_output()),
                            source(&changed),
                            transport,
                            &ToolMap::new(),
                            1,
                        )
                        .map_err(|error| format!("{error:?}"))?,
                    *expected,
                    "native arguments: {label}"
                );
            }
        } else {
            for input in [None, Some(Value::Null)] {
                let mut changed = native.clone();
                let fields = changed["content"][2]
                    .as_object_mut()
                    .ok_or("missing native tool-use fixture")?;
                if let Some(input) = input {
                    fields.insert("input".into(), input);
                } else {
                    assert!(fields.remove("input").is_some());
                }
                assert!(
                    !gateway_provider_replay::replay::ReplayStore::default()
                        .remember_decoded_native(
                            context(),
                            &sources(&messages_output()),
                            source(&changed),
                            transport,
                            &ToolMap::new(),
                            1,
                        )
                        .map_err(|error| format!("{error:?}"))?
                );
            }
        }
    }
    Ok(())
}
