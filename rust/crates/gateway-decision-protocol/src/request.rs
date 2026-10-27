use crate::catalog::{equals, trim, whitespace};
use gateway_source::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use gateway_contracts::model::ModelId;
use serde::Serialize;

const REQUEST_KEYS: &[&str] = &[
    "model",
    "state",
    "questions",
    "provider",
    "session_id",
    "trace",
    "user",
];
type Fields = Vec<(JsString16, SourceJson)>;
type Result<T> = std::result::Result<T, RequestError>;

#[derive(Clone, Debug)]
pub struct RequestError {
    pub status: u16,
    pub message: JsString16,
    pub code: &'static str,
    pub param: Option<&'static str>,
}
impl RequestError {
    pub fn invalid(message: impl Into<JsString16>, param: Option<&'static str>) -> Self {
        Self {
            status: 400,
            message: message.into(),
            code: "decisions_request_invalid",
            param,
        }
    }
    pub fn body(&self) -> std::result::Result<SourceJson, GatewayError> {
        #[derive(Serialize)]
        struct Envelope<'a> {
            error: Fields<'a>,
        }
        #[derive(Serialize)]
        struct Fields<'a> {
            message: &'a JsString16,
            #[serde(rename = "type")]
            kind: &'static str,
            code: &'static str,
            #[serde(skip_serializing_if = "Option::is_none")]
            param: Option<&'static str>,
        }
        SourceJson::from_serializable(&Envelope {
            error: Fields {
                message: &self.message,
                kind: "invalid_request_error",
                code: self.code,
                param: self.param,
            },
        })
    }
}
impl From<GatewayError> for RequestError {
    fn from(error: GatewayError) -> Self {
        Self {
            status: error.status,
            message: JsString16::from_utf8(&error.envelope.error.message),
            code: "invalid_request_error",
            param: None,
        }
    }
}
#[derive(Clone, Debug)]
pub struct DecisionRequest {
    pub model: ModelId,
    pub body: SourceJson,
}
fn guidance(value: &SourceJson) -> bool {
    matches!(value.as_bytes().first(), Some(b'"' | b'{' | b'['))
}
fn question_error(name: &JsString16, suffix: &str) -> RequestError {
    let mut units = "questions.".encode_utf16().collect::<Vec<_>>();
    units.extend_from_slice(name.as_units());
    units.extend(suffix.encode_utf16());
    RequestError::invalid(JsString16::from_units(units), Some("questions"))
}
fn noul(criteria: Option<SourceJson>, name: &JsString16) -> Result<()> {
    let Some(criteria) = criteria else {
        return Ok(());
    };
    let fields = source::entries(&criteria)
        .ok_or_else(|| question_error(name, ".criteria must be an object"))?;
    if fields.len() != 2
        || !["true", "false"]
            .iter()
            .all(|key| fields.iter().any(|(name, _)| equals(name, key)))
    {
        return Err(question_error(
            name,
            ".criteria must contain exactly true and false",
        ));
    }
    if fields.iter().any(|(_, value)| !guidance(value)) {
        return Err(question_error(
            name,
            ".criteria values must be strings, objects, or arrays",
        ));
    }
    Ok(())
}
fn choice(criteria: Option<SourceJson>, name: &JsString16) -> Result<()> {
    let fields = criteria
        .as_ref()
        .and_then(source::entries)
        .filter(|fields| !fields.is_empty())
        .ok_or_else(|| question_error(name, ".criteria must be a non-empty object"))?;
    if fields.iter().any(|(name, value)| {
        trim(name).as_units().is_empty() || value.as_str() != "null" && !guidance(value)
    }) {
        return Err(question_error(
            name,
            ".criteria entries must have names and string, object, array, or null guidance",
        ));
    }
    Ok(())
}
fn score(criteria: Option<SourceJson>, name: &JsString16) -> Result<()> {
    let values = criteria
        .as_ref()
        .and_then(source::array)
        .filter(|values| !values.is_empty())
        .ok_or_else(|| question_error(name, ".criteria must be a non-empty array"))?;
    if values.iter().any(|value| !guidance(value)) {
        return Err(question_error(
            name,
            ".criteria entries must be strings, objects, or arrays",
        ));
    }
    Ok(())
}
fn question(name: &JsString16, value: &SourceJson) -> Result<()> {
    let fields =
        source::entries(value).ok_or_else(|| question_error(name, " must be an object"))?;
    if fields.iter().any(|(key, _)| {
        !["type", "instructions", "criteria"]
            .iter()
            .any(|s| equals(key, s))
    }) {
        return Err(question_error(name, " contains an unsupported field"));
    }
    let kind = source::member(value, "type")
        .as_ref()
        .and_then(source::string);
    let kind = kind
        .as_ref()
        .and_then(|kind| {
            ["noul", "choice", "score", "box"]
                .into_iter()
                .find(|s| equals(kind, s))
        })
        .ok_or_else(|| question_error(name, ".type must be noul, choice, score, or box"))?;
    if !source::member(value, "instructions").is_some_and(|value| guidance(&value)) {
        return Err(question_error(
            name,
            ".instructions must be a string, object, or array",
        ));
    }
    let criteria = source::member(value, "criteria");
    match kind {
        "noul" => noul(criteria, name),
        "choice" => choice(criteria, name),
        "score" => score(criteria, name),
        _ if criteria.is_some() => Err(question_error(
            name,
            ".criteria is not valid for box questions",
        )),
        _ => Ok(()),
    }
}
fn questions(raw: &SourceJson) -> Result<()> {
    let fields = source::member(raw, "questions")
        .as_ref()
        .and_then(source::entries)
        .ok_or_else(|| RequestError::invalid("questions must be an object", Some("questions")))?;
    if fields.is_empty() || fields.len() > 24 {
        return Err(RequestError::invalid(
            "questions must hold between 1 and 24 questions",
            Some("questions"),
        ));
    }
    for (name, value) in fields {
        if trim(&name).as_units().is_empty() || name.as_units().len() > 120 {
            return Err(RequestError::invalid(
                "question names must be non-empty and at most 120 characters",
                Some("questions"),
            ));
        }
        question(&name, &value)?;
    }
    Ok(())
}
fn metadata(raw: &SourceJson) -> Result<()> {
    if source::member(raw, "provider")
        .is_some_and(|value| value.as_str() != "null" && source::entries(&value).is_none())
    {
        return Err(RequestError::invalid(
            "provider must be an object or null",
            Some("provider"),
        ));
    }
    if source::member(raw, "trace").is_some_and(|value| source::entries(&value).is_none()) {
        return Err(RequestError::invalid(
            "trace must be an object",
            Some("trace"),
        ));
    }
    for key in ["session_id", "user"] {
        if let Some(value) = source::member(raw, key)
            && !source::string(&value)
                .is_some_and(|value| !value.as_units().is_empty() && value.as_units().len() <= 256)
        {
            return Err(RequestError::invalid(
                JsString16::from_utf8(&format!(
                    "{key} must be a non-empty string of at most 256 characters"
                )),
                Some(key),
            ));
        }
    }
    Ok(())
}
fn valid_model(model: &JsString16) -> bool {
    let parts = model
        .as_units()
        .split(|unit| *unit == 47)
        .collect::<Vec<_>>();
    model.as_units().len() <= 200
        && parts.len() == 2
        && parts.iter().all(|part| {
            !part.is_empty()
                && !part
                    .iter()
                    .any(|unit| char::from_u32(u32::from(*unit)).is_some_and(whitespace))
        })
}
pub fn parse(raw: &SourceJson) -> Result<DecisionRequest> {
    let fields: Fields = source::entries(raw).ok_or_else(|| RequestError {
        status: 400,
        message: "Invalid JSON body".into(),
        code: "invalid_request_error",
        param: None,
    })?;
    if let Some((key, _)) = fields
        .iter()
        .find(|(key, _)| !REQUEST_KEYS.iter().any(|s| equals(key, s)))
    {
        let mut text = "Unsupported key: ".encode_utf16().collect::<Vec<_>>();
        text.extend_from_slice(key.as_units());
        return Err(RequestError::invalid(JsString16::from_units(text), None));
    }
    let model = source::member(raw, "model")
        .as_ref()
        .and_then(source::string)
        .ok_or_else(|| RequestError::invalid("model must be a string", Some("model")))?;
    let model = trim(&model);
    if !valid_model(&model) {
        return Err(RequestError::invalid(
            "model must be an OpenRouter decision model id",
            Some("model"),
        ));
    }
    if !source::member(raw, "state").is_some_and(|state| guidance(&state)) {
        return Err(RequestError::invalid(
            "state must be a string, object, or array",
            Some("state"),
        ));
    }
    questions(raw)?;
    metadata(raw)?;
    let fields = fields
        .into_iter()
        .map(|(key, value)| {
            let value = if equals(&key, "model") {
                source::quoted(&model)?
            } else {
                value
            };
            Ok((key, value))
        })
        .collect::<std::result::Result<Vec<_>, GatewayError>>()?;
    Ok(DecisionRequest {
        model: ModelId::from_source(model),
        body: source::object_keys(fields)?,
    })
}
pub fn select(
    request: &DecisionRequest,
    upstream: &ModelId,
) -> std::result::Result<SourceJson, GatewayError> {
    let fields = source::entries(&request.body)
        .ok_or_else(|| GatewayError::new(500, "Invalid decision request", Some("server_error")))?;
    source::object_keys(
        fields
            .into_iter()
            .map(|(key, value)| {
                let value = if equals(&key, "model") {
                    upstream.to_source_json()?
                } else {
                    value
                };
                Ok((key, value))
            })
            .collect::<std::result::Result<Vec<_>, GatewayError>>()?,
    )
}
