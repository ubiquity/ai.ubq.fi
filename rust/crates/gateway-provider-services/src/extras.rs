use gateway_provider_source::source as s;
use gateway_provider_tool_grammar::source_tools::ToolMap;
use gateway_provider_protocol::types::ClientSurface;
use gateway_provider_protocol::types::DecodePlan;
use gateway_provider_protocol::types::Headers;
use gateway_provider_protocol::types::PrepareOptions;
use gateway_provider_protocol::types::PreparedRequest;
use gateway_provider_protocol::types::ResponseDecode;
use gateway_core::error::GatewayError;
use gateway_core::js_string::JsString16;
use gateway_core::model::ModelId;
use gateway_core::provider::UpstreamProvider;
use gateway_core::source_json::SourceJson;
use gateway_core::source_serialization as source;
use gateway_local_refusal::source_error::SourceRequestError;
use gateway_schema::source_request::value::Value;
use serde::Deserialize;
use serde::Serialize;
use serde_json::Value as ScalarValue;
use gateway_provider_source::extras::js_whitespace;
use gateway_provider_source::extras::key_is;
use gateway_provider_source::extras::scalar_error;
use gateway_provider_source::extras::invalid;
use gateway_provider_source::extras::keyed_error;
use gateway_provider_source::extras::lift;



struct AncillaryBody { source: SourceJson, model: ModelId }

fn prepared(provider: UpstreamProvider, url: &str, surface: ClientSurface, raw: &Value, body: AncillaryBody, options: &PrepareOptions, decode: ResponseDecode) -> Result<PreparedRequest, SourceRequestError> {
    let headers = Headers::from([
        ("Authorization".into(), format!("Bearer {}", options.auth.secret)),
        ("Content-Type".into(), "application/json".into()),
        ("Accept".into(), "application/json".into()),
    ]);
    let canonical = source::canonical(&body.source)?;
    let plan = DecodePlan {
        provider, surface, decode,
        model: raw["model"].as_text().map(|text| ModelId::from_source(text.core())).unwrap_or_else(|| body.model.clone()),
        upstream_model: body.model,
        response_id: options.response_id.clone(), created_at: options.created_at, stream: false,
        request: raw.source()?, tool_names: ToolMap::new(), reasoning_summary: None,
    };
    Ok(gateway_provider_protocol::types::prepared_post_data(provider, url.into(), headers, canonical, plan))
}

fn systemone_body(body: SourceJson) -> Result<AncillaryBody, SourceRequestError> {
    let model = source::member(&body, "model").and_then(|value| source::string(&value)).ok_or_else(|| GatewayError::unavailable("Invalid source JSON serialization.", "source_json_serialization_invalid"))?;
    Ok(AncillaryBody { source: body, model: ModelId::from_source(model) })
}


fn systemone_view(
    raw: &Value,
    options: &PrepareOptions,
) -> Result<PreparedRequest, SourceRequestError> {
    let source = raw.source().map_err(SourceRequestError::from_gateway)?;
    let body = gateway_decision_protocol::legacy::parse(&source).map_err(decision_error)?;
    prepared(
        UpstreamProvider::Openrouter,
        "https://openrouter.ai/api/v1/systemone",
        ClientSurface::SystemOne,
        raw,
        systemone_body(body)?,
        options,
        ResponseDecode::SystemOne,
    )
}


fn decision_error(error: gateway_decision_protocol::request::RequestError) -> SourceRequestError {
    SourceRequestError::new(
        error.status,
        error.message,
        "invalid_request_error",
        Some(error.code),
        error.param.map(|p| Some(p.into())),
    )
}


pub fn prepare_source_decisions(
    raw: &SourceJson,
    options: &PrepareOptions,
) -> Result<PreparedRequest, SourceRequestError> {
    let request = gateway_decision_protocol::request::parse(raw).map_err(decision_error)?;
    let model = gateway_decision_protocol::catalog::resolve_decision(
        &request.model,
        &options.allowed_models,
    )
    .ok_or_else(|| {
        decision_error(gateway_decision_protocol::request::RequestError::invalid(
            "The requested model is not served by the OpenRouter Decisions API",
            Some("model"),
        ))
    })?;
    let body = gateway_decision_protocol::request::select(&request, &model)
        .map_err(SourceRequestError::from_gateway)?;
    prepared(
        UpstreamProvider::Openrouter,
        gateway_decision_protocol::catalog::DECISIONS_URL,
        ClientSurface::Decisions,
        &Value::from_source(raw.clone()),
        AncillaryBody { source: body, model },
        options,
        ResponseDecode::Decisions,
    )
}


pub fn decode_systemone(value: &ScalarValue) -> Result<ScalarValue, GatewayError> {
    if !value["answers"].is_object() {
        return Err(GatewayError::new(
            502,
            "System One upstream invalid response",
            Some("openrouter_upstream_invalid_response"),
        ));
    }
    Ok(value.clone())
}


pub fn systemone_usage(value: &ScalarValue) -> Option<ScalarValue> {
    let input = value["input_tokens"].as_f64()?;
    let output = value["output_tokens"].as_f64()?;
    let mut usage = serde_json::json!({"input_tokens":input,"output_tokens":output,"total_tokens":input+output,"input_tokens_details":{"cached_tokens":0}});
    if value["cost"].is_number() {
        usage["cost"] = value["cost"].clone();
    }
    Some(usage)
}


#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct EmbeddingsProfile {
    pub model: String,
    pub inputs: Vec<JsString16>,
    pub input_type: String,
    pub dimensions: u64,
    pub encoding_format: String,
    pub truncation: bool,
    pub cache_profile_key: String,
    pub total_chars: usize,
}


fn embeddings_view(raw: &Value, job: bool) -> Result<EmbeddingsProfile, SourceRequestError> {
    let object = raw
        .as_object()
        .ok_or_else(|| scalar_error(400, "Invalid JSON body", Some("invalid_request_error")))?;
    for (key, _) in object {
        if ![
            "dimensions",
            "encoding_format",
            "input",
            "input_type",
            "model",
            "truncation",
        ]
        .iter()
        .any(|name| key_is(key, name))
            && (!key_is(key, "user") || job)
        {
            return Err(keyed_error(
                "Unrecognized request argument supplied: ",
                key,
                "",
                false,
            ));
        }
    }
    let model = raw
        .get("model")
        .and_then(Value::as_text)
        .filter(|s| !s.trim_matches(js_whitespace).is_empty())
        .ok_or_else(|| invalid("model is required and must be a non-empty string", "model"))?;
    if model.core() != JsString16::from_utf8("voyage-4-large") {
        return Err(SourceRequestError::new(
            400,
            s::concat(&[
                &JsString16::from_utf8("Unsupported embedding model: "),
                &model.core(),
            ]),
            "invalid_request_error",
            Some("model_not_found"),
            Some(Some("model".into())),
        ));
    }
    let model = "voyage-4-large";
    let dimensions = match raw.get("dimensions") {
        None => 1024,
        Some(value) => {
            let value = value
                .as_f64()
                .filter(|v| v.is_finite() && v.fract() == 0.0)
                .ok_or_else(|| invalid("dimensions must be an integer", "dimensions"))?;
            if ![256.0, 512.0, 1024.0, 2048.0].contains(&value) {
                return Err(invalid(
                    "dimensions must be one of: 256, 512, 1024, 2048",
                    "dimensions",
                ));
            }
            value as u64
        }
    };
    let encoding = embeddings_encoding(raw, job)?;
    let input_type = match raw.get("input_type") {
        None if job => {
            return Err(invalid(
                "input_type is required and must be one of: \"query\", \"document\"",
                "input_type",
            ));
        }
        None => "document",
        Some(value) => value
            .as_str()
            .filter(|v| ["query", "document"].contains(v))
            .ok_or_else(|| {
                invalid(
                    "input_type must be one of: \"query\", \"document\"",
                    "input_type",
                )
            })?,
    };
    let truncation = raw
        .get("truncation")
        .map_or(Some(true), Value::as_bool)
        .ok_or_else(|| invalid("truncation must be a boolean", "truncation"))?;
    if raw
        .get("user")
        .is_some_and(|v| !v.is_null() && !v.is_string())
    {
        return Err(invalid("user must be a string", "user"));
    }
    let (inputs, total) = embeddings_inputs(raw)?;
    let cache_profile_key = serde_json::json!([
        "voyage-profile-v2",
        model,
        input_type,
        dimensions,
        "float",
        encoding,
        truncation
    ])
    .to_string();
    Ok(EmbeddingsProfile {
        model: model.into(),
        inputs,
        input_type: input_type.into(),
        dimensions,
        encoding_format: encoding.into(),
        truncation,
        cache_profile_key,
        total_chars: total,
    })
}


fn prepare_embeddings_view(
    raw: &Value,
    options: &PrepareOptions,
) -> Result<PreparedRequest, SourceRequestError> {
    let profile = embeddings_view(raw, false)?;
    let input = if profile.inputs.len() == 1 { s::quoted(&profile.inputs[0])? }
        else { s::array(&profile.inputs.iter().map(s::quoted).collect::<Result<Vec<_>, _>>()?)? };
    let body = s::object(vec![
        ("model", s::text(&profile.model)?), ("input", input), ("input_type", s::text(&profile.input_type)?),
        ("output_dimension", s::scalar_json(&serde_json::json!(profile.dimensions))?),
        ("output_dtype", s::text("float")?), ("truncation", s::scalar_json(&serde_json::json!(profile.truncation))?),
    ])?;
    prepared(
        UpstreamProvider::Voyage, "https://api.voyageai.com/v1/embeddings", ClientSurface::Embeddings,
        raw, AncillaryBody { source: body, model: ModelId::from_scalar(&profile.model) }, options, ResponseDecode::Voyage,
    )
}


pub fn prepare_source_systemone(
    raw: &SourceJson,
    options: &PrepareOptions,
) -> Result<PreparedRequest, SourceRequestError> {
    systemone_view(&Value::from_source(raw.clone()), options)
}


pub fn parse_source_embeddings(
    raw: &SourceJson,
    job: bool,
) -> Result<EmbeddingsProfile, SourceRequestError> {
    embeddings_view(&Value::from_source(raw.clone()), job)
}


pub fn prepare_source_embeddings(
    raw: &SourceJson,
    options: &PrepareOptions,
) -> Result<PreparedRequest, SourceRequestError> {
    prepare_embeddings_view(&Value::from_source(raw.clone()), options)
}


pub fn prepare_systemone(
    raw: &ScalarValue,
    options: &PrepareOptions,
) -> Result<PreparedRequest, GatewayError> {
    prepare_source_systemone(&lift(raw)?, options).map_err(s::scalar_error)
}


pub fn parse_embeddings(raw: &ScalarValue, job: bool) -> Result<EmbeddingsProfile, GatewayError> {
    parse_source_embeddings(&lift(raw)?, job).map_err(s::scalar_error)
}


pub fn prepare_embeddings(
    raw: &ScalarValue,
    options: &PrepareOptions,
) -> Result<PreparedRequest, GatewayError> {
    prepare_source_embeddings(&lift(raw)?, options).map_err(s::scalar_error)
}


pub fn decode_source_systemone(raw: &SourceJson) -> Result<SourceJson, GatewayError> {
    let value = Value::from_source(raw.clone());
    if !value["answers"].is_object() {
        return Err(GatewayError::new(
            502,
            "System One upstream invalid response",
            Some("openrouter_upstream_invalid_response"),
        ));
    }
    Ok(raw.clone())
}


pub fn source_systemone_usage(raw: &SourceJson) -> Option<ScalarValue> {
    let value = Value::from_source(raw.clone());
    let input = value["input_tokens"].as_f64()?;
    let output = value["output_tokens"].as_f64()?;
    let mut usage = serde_json::json!({"input_tokens":input,"output_tokens":output,"total_tokens":input+output,"input_tokens_details":{"cached_tokens":0}});
    if let Some(cost) = value["cost"].as_f64() {
        usage["cost"] = serde_json::json!(cost)
    }
    Some(usage)
}

fn embeddings_encoding(raw: &Value, job: bool) -> Result<&str, SourceRequestError> {
    let encoding = match raw.get("encoding_format") {
        None => "float",
        Some(value) => {
            if !value.is_string() {
                return Err(invalid(
                    "encoding_format must be a string",
                    "encoding_format",
                ));
            }
            let Some(text) = value.as_str() else {
                return Err(invalid(
                    "encoding_format must be one of: \"float\", \"base64\"",
                    "encoding_format",
                ));
            };
            text
        }
    };
    if !["float", "base64"].contains(&encoding) {
        return Err(invalid(
            "encoding_format must be one of: \"float\", \"base64\"",
            "encoding_format",
        ));
    }
    if job && encoding != "float" {
        return Err(invalid(
            "encoding_format must be \"float\" for embeddings jobs",
            "encoding_format",
        ));
    }
    Ok(encoding)
}
fn embeddings_inputs(raw: &Value) -> Result<(Vec<JsString16>, usize), SourceRequestError> {
    let input_error = || invalid("input must be a string or an array of strings", "input");
    let inputs = if let Some(input) = raw["input"].as_text() {
        vec![input.core()]
    } else {
        raw["input"]
            .as_array()
            .ok_or_else(input_error)?
            .iter()
            .map(|v| v.as_text().map(|text| text.core()).ok_or_else(input_error))
            .collect::<Result<Vec<_>, _>>()?
    };
    if inputs.is_empty() {
        return Err(invalid(
            "input must be a non-empty string or a non-empty array",
            "input",
        ));
    }
    if inputs.len() > 128 {
        return Err(invalid(
            format!("Too many inputs: {} (max 128)", inputs.len()),
            "input",
        ));
    }
    let mut total = 0;
    for input in &inputs {
        let len = input.as_units().len();
        if len > 20_000 {
            return Err(invalid(
                format!("Input too large: {len} chars (max 20000)"),
                "input",
            ));
        }
        total += len;
        if total > 100_000 {
            return Err(invalid(
                format!("Request too large: {total} chars total (max 100000)"),
                "input",
            ));
        }
        if String::from_utf16_lossy(input.as_units()).len().div_ceil(4) > 10_000 {
            return Err(invalid(
                format!(
                    "Input too large for embeddings provider: ~{} tokens (max 10000).",
                    String::from_utf16_lossy(input.as_units()).len().div_ceil(4)
                ),
                "input",
            ));
        }
    }
    Ok((inputs, total))
}
