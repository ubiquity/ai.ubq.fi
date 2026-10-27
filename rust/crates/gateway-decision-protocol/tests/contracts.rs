use gateway_contracts::model::ModelId;
use gateway_source::source_json::SourceJson;
use gateway_decision_protocol::{catalog, legacy, request, response};
use serde_json::{Value, json};
type Result = std::result::Result<(), Box<dyn std::error::Error>>;
fn source(value: Value) -> std::result::Result<SourceJson, gateway_source::error::GatewayError> {
    SourceJson::from_serializable(&value)
}
fn valid() -> Value {
    json!({"model":"baseten/elephant-1:free","state":{"screen":"ready"},"questions":{
        "bool":{"type":"noul","instructions":"Allowed?","criteria":{"true":"yes","false":"no"}},
        "choice":{"type":"choice","instructions":["Choose"],"criteria":{"a":null,"b":{}}},
        "score":{"type":"score","instructions":{},"criteria":["low",{},[]]},
        "box":{"type":"box","instructions":"Find bounds"}
    }})
}
#[test]
fn decisions_forward_typed_questions_metadata_and_trimmed_model() -> Result {
    let mut raw = valid();
    raw["model"] = json!(" \u{feff}baseten/elephant-1:free ");
    raw["provider"] = Value::Null;
    raw["trace"] = json!({"id":"trace"});
    raw["session_id"] = json!(" ");
    raw["user"] = json!("user");
    let parsed = request::parse(&source(raw.clone())?).map_err(|e| format!("{e:?}"))?;
    raw["model"] = json!("baseten/elephant-1:free");
    assert_eq!(parsed.body.deserialize::<Value>()?, raw);
    assert!(parsed.model.matches_scalar("baseten/elephant-1:free"));
    Ok(())
}
#[test]
fn request_validation_preserves_phase_order_and_exact_errors() -> Result {
    let cases = [
        (json!({"extra":true}), "Unsupported key: extra", None),
        (json!({"model":3}), "model must be a string", Some("model")),
        (
            json!({"model":"single"}),
            "model must be an OpenRouter decision model id",
            Some("model"),
        ),
        (
            json!({"model":"a/b","state":null}),
            "state must be a string, object, or array",
            Some("state"),
        ),
        (
            json!({"model":"a/b","state":[],"questions":{}}),
            "questions must hold between 1 and 24 questions",
            Some("questions"),
        ),
        (
            json!({"model":"a/b","state":[],"questions":{"q":{"type":"box","instructions":"x","criteria":[]}}}),
            "questions.q.criteria is not valid for box questions",
            Some("questions"),
        ),
    ];
    for (raw, message, param) in cases {
        let error = match request::parse(&source(raw)?) {
            Ok(_) => return Err("invalid contract vector accepted".into()),
            Err(error) => error,
        };
        let body = error.body()?.deserialize::<Value>()?;
        assert_eq!(body["error"]["message"], message);
        assert_eq!(body["error"]["code"], "decisions_request_invalid");
        assert_eq!(body["error"].get("param").and_then(Value::as_str), param);
    }
    Ok(())
}
#[test]
fn malformed_guidance_and_metadata_never_produce_a_request() -> Result {
    for (key, value) in [
        ("state", json!(true)),
        ("provider", json!([])),
        ("trace", Value::Null),
        ("user", json!("")),
        ("session_id", json!(3)),
    ] {
        let mut raw = valid();
        raw[key] = value;
        assert!(request::parse(&source(raw)?).is_err(), "{key}");
    }
    for question in [
        json!({"type":"noul","instructions":null}),
        json!({"type":"noul","instructions":"","criteria":{"true":"yes"}}),
        json!({"type":"noul","instructions":"","criteria":{"true":false,"false":[]}}),
        json!({"type":"choice","instructions":"","criteria":{}}),
        json!({"type":"choice","instructions":"","criteria":{" ":"x"}}),
        json!({"type":"score","instructions":"","criteria":[null]}),
        json!({"type":"score","instructions":"","criteria":[]}),
        json!({"type":"box","instructions":"","unknown":true}),
    ] {
        let mut raw = valid();
        raw["questions"] = json!({"q":question});
        assert!(request::parse(&source(raw)?).is_err());
    }
    Ok(())
}
#[test]
fn source_units_and_guidance_shapes_survive_the_decision_wire() -> Result {
    let raw = SourceJson::parse(
        r#"{"model":"a/b","state":"\ud800","questions":{"\udc00":{"type":"box","instructions":["\ud800"]}},"trace":{"x":"\udc00"}}"#,
    )?;
    let parsed = request::parse(&raw).map_err(|e| format!("{e:?}"))?;
    assert!(parsed.body.as_str().contains("\\ud800"));
    assert!(parsed.body.as_str().contains("\\udc00"));
    for state in [json!(""), json!([]), json!({})] {
        let mut raw = valid();
        raw["state"] = state;
        assert!(request::parse(&source(raw)?).is_ok());
    }
    Ok(())
}
#[test]
fn question_and_metadata_bounds_count_utf16_units() -> Result {
    let mut raw = valid();
    raw["questions"] = json!({ "x".repeat(120): {"type":"box","instructions":""}});
    raw["user"] = json!("😀".repeat(128));
    assert!(request::parse(&source(raw.clone())?).is_ok());
    raw["user"] = json!("😀".repeat(129));
    assert!(request::parse(&source(raw)?).is_err());
    let mut raw = valid();
    raw["questions"] = Value::Object(
        (0..25)
            .map(|i| (format!("q{i}"), json!({"type":"box","instructions":""})))
            .collect(),
    );
    assert!(request::parse(&source(raw)?).is_err());
    Ok(())
}
#[test]
fn modalities_separate_native_text_and_decision_models() -> Result {
    let decision = source(json!({"id":"a/b","output_modalities":["decisions"]}))?;
    assert_eq!(catalog::endpoints(&decision), vec!["/v1/decisions"]);
    assert!(!catalog::text_model(&decision));
    assert!(catalog::decision_model(&decision));
    let both = source(json!({"output_modalities":["text","decisions"]}))?;
    assert_eq!(
        catalog::endpoints(&both),
        vec!["/v1/responses", "/v1/chat/completions", "/v1/decisions"]
    );
    assert_eq!(
        catalog::endpoints(&source(json!({}))?),
        vec!["/v1/responses", "/v1/chat/completions"]
    );
    let raw =
        source(json!({"architecture":{"output_modalities":[" decisions ","decisions",3,""]}}))?;
    assert_eq!(catalog::output_modalities(&raw).map(|v| v.len()), Some(1));
    assert_eq!(
        catalog::output_modalities(&source(json!({"architecture":{"output_modalities":[]}}))?),
        None
    );
    Ok(())
}
#[test]
fn jev_alias_requires_a_real_decision_catalog_row() {
    let request = ModelId::from_scalar("typesafe/jev-latest");
    assert!(catalog::resolve_decision(&request, &[]).is_none());
    let expected = ModelId::from_scalar("~typesafe/jev-latest");
    assert_eq!(
        catalog::resolve_decision(&request, std::slice::from_ref(&expected)),
        Some(expected)
    );
}
#[test]
fn answers_and_usage_are_preserved_and_missing_answers_refuse() -> Result {
    let body = SourceJson::parse(
        r#"{"answers":{"x":0.5,"other":"\ud800"},"usage":{"input_tokens":4,"output_tokens":2,"cost":0.1},"trace":{"x":3}}"#,
    )?;
    assert_eq!(response::decode(&body)?, body);
    assert_eq!(
        response::usage(&body),
        Some(
            json!({"input_tokens":4.0,"output_tokens":2.0,"total_tokens":6.0,"input_tokens_details":{"cached_tokens":0},"cost":0.1})
        )
    );
    for raw in [json!({}), json!({"answers":[]}), json!({"answers":null})] {
        assert!(response::decode(&source(raw)?).is_err());
    }
    Ok(())
}
#[test]
fn legacy_systemone_keeps_its_jev_only_envelope() -> Result {
    let body = source(json!({"state":{},"questions":{"q":{"type":"noul"}}}))?;
    let forwarded = legacy::parse(&body)
        .map_err(|e| format!("{e:?}"))?
        .deserialize::<Value>()?;
    assert_eq!(forwarded["model"], "~typesafe/jev-latest");
    assert!(legacy::parse(&source(valid())?).is_err());
    assert!(
        legacy::parse(&source(
            json!({"state":[],"questions":{"q":{"type":"noul"}}})
        )?)
        .is_err()
    );
    Ok(())
}
