//! The documented Decisions envelope (src/decisions/handlers.ts parseDecisionsRequest).
//! Values stay source JSON; keys, lengths and trims use JavaScript UTF16 semantics.
use gateway_core::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use gateway_schema::source_request::error::SourceRequestError;

pub const MAX_QUESTIONS: usize = 24;
const MAX_MODEL_LENGTH: usize = 200;
const MAX_METADATA_LENGTH: usize = 256;
const MAX_QUESTION_NAME_LENGTH: usize = 120;
const REQUEST_KEYS: [&str; 7] = [
    "model",
    "state",
    "questions",
    "provider",
    "session_id",
    "trace",
    "user",
];
const QUESTION_KEYS: [&str; 3] = ["type", "instructions", "criteria"];
const QUESTION_TYPES: [&str; 4] = ["noul", "choice", "score", "box"];
/// Optional members forwarded verbatim after model, state and questions.
pub const OPTIONAL_KEYS: [&str; 4] = ["provider", "session_id", "trace", "user"];

/// A validated request. `model` is the trimmed client id; `raw` keeps the body.
pub struct DecisionsRequest {
    pub model: JsString16,
    pub raw: SourceJson,
}

#[derive(Clone, Copy, Eq, PartialEq)]
enum Kind {
    Object,
    Array,
    String,
    Other,
}
fn kind(value: &SourceJson) -> Kind {
    match value.as_str().trim_start().as_bytes().first() {
        Some(b'{') => Kind::Object,
        Some(b'[') => Kind::Array,
        Some(b'"') => Kind::String,
        _ => Kind::Other,
    }
}
fn plain(value: Option<&SourceJson>) -> bool {
    value.is_some_and(|value| kind(value) == Kind::Object)
}
/// A string, object, or array is the documented structured-guidance value.
fn guidance(value: Option<&SourceJson>) -> bool {
    value.is_some_and(|value| kind(value) != Kind::Other)
}
fn null(value: &SourceJson) -> bool {
    value.as_str().trim() == "null"
}
fn same(value: &JsString16, literal: &str) -> bool {
    value.as_units().iter().copied().eq(literal.encode_utf16())
}
/// ECMAScript WhiteSpace and LineTerminator units, used by trim and `\s`.
pub fn js_space(unit: u16) -> bool {
    matches!(
        unit,
        0x09..=0x0d | 0x20 | 0xa0 | 0x1680 | 0x2000..=0x200a | 0x2028 | 0x2029 | 0x202f | 0x205f
            | 0x3000
            | 0xfeff
    )
}
pub fn js_trim(value: &JsString16) -> JsString16 {
    let units = value.as_units();
    let start = units.iter().position(|unit| !js_space(*unit));
    let end = units.iter().rposition(|unit| !js_space(*unit));
    match (start, end) {
        (Some(start), Some(end)) => JsString16::from_units(units[start..=end].to_vec()),
        _ => JsString16::default(),
    }
}
fn blank(value: &JsString16) -> bool {
    value.as_units().iter().all(|unit| js_space(*unit))
}
/// `^~?[^/\s]+\/[^/\s]+$`: the optional tilde is itself a non-slash unit.
fn model_id(model: &JsString16) -> bool {
    let units = model.as_units();
    let slashes = units.iter().filter(|unit| **unit == 47).count();
    slashes == 1
        && units.first() != Some(&47)
        && units.last() != Some(&47)
        && !units.iter().any(|unit| js_space(*unit))
}
/// `Object.keys` order and last-duplicate values, including an array's indices.
fn keys(value: &SourceJson) -> Vec<JsString16> {
    if kind(value) == Kind::Array {
        let count = source::array(value).map_or(0, |items| items.len());
        return (0..count)
            .map(|index| JsString16::from_utf8(&index.to_string()))
            .collect();
    }
    entries(value).into_iter().map(|(key, _)| key).collect()
}
fn entries(value: &SourceJson) -> Vec<(JsString16, SourceJson)> {
    source::entries(value).unwrap_or_default()
}
fn named(prefix: &str, name: &JsString16, suffix: &str) -> JsString16 {
    let mut units: Vec<u16> = prefix.encode_utf16().collect();
    units.extend_from_slice(name.as_units());
    units.extend(suffix.encode_utf16());
    JsString16::from_units(units)
}

type Check = Result<(), SourceRequestError>;
/// `decisions_request_invalid`; a None param omits the field like openaiError.
pub fn invalid(message: impl Into<JsString16>, param: Option<&str>) -> SourceRequestError {
    SourceRequestError::new(
        400,
        message,
        "invalid_request_error",
        Some("decisions_request_invalid"),
        param.map(|param| Some(JsString16::from_utf8(param))),
    )
}
fn question_invalid(name: &JsString16, suffix: &str) -> SourceRequestError {
    invalid(named("questions.", name, suffix), Some("questions"))
}

fn model(raw: &SourceJson) -> Result<JsString16, SourceRequestError> {
    let Some(model) = source::member(raw, "model").and_then(|v| source::string(&v)) else {
        return Err(invalid("model must be a string", Some("model")));
    };
    let model = js_trim(&model);
    let length = model.as_units().len();
    if length == 0 || length > MAX_MODEL_LENGTH || !model_id(&model) {
        return Err(invalid(
            "model must be an OpenRouter decision model id",
            Some("model"),
        ));
    }
    Ok(model)
}
fn state(raw: &SourceJson) -> Check {
    if guidance(source::member(raw, "state").as_ref()) {
        Ok(())
    } else {
        Err(invalid(
            "state must be a string, object, or array",
            Some("state"),
        ))
    }
}
fn noul(criteria: Option<&SourceJson>, name: &JsString16) -> Check {
    let Some(criteria) = criteria else {
        return Ok(());
    };
    if !plain(Some(criteria)) {
        return Err(question_invalid(name, ".criteria must be an object"));
    }
    let names = keys(criteria);
    let has = |key: &str| names.iter().any(|name| same(name, key));
    if names.iter().any(|k| !same(k, "true") && !same(k, "false")) || !has("true") || !has("false")
    {
        return Err(question_invalid(
            name,
            ".criteria must contain exactly true and false",
        ));
    }
    if !["true", "false"]
        .iter()
        .all(|key| guidance(source::member(criteria, key).as_ref()))
    {
        return Err(question_invalid(
            name,
            ".criteria values must be strings, objects, or arrays",
        ));
    }
    Ok(())
}
fn choice(criteria: Option<&SourceJson>, name: &JsString16) -> Check {
    let fields = criteria.filter(|c| plain(Some(c))).map(entries);
    let Some(fields) = fields.filter(|fields| !fields.is_empty()) else {
        return Err(question_invalid(
            name,
            ".criteria must be a non-empty object",
        ));
    };
    for (choice, value) in fields {
        if blank(&choice) || (!null(&value) && !guidance(Some(&value))) {
            return Err(question_invalid(
                name,
                ".criteria entries must have names and string, object, array, or null guidance",
            ));
        }
    }
    Ok(())
}
fn score(criteria: Option<&SourceJson>, name: &JsString16) -> Check {
    let items = criteria
        .filter(|c| kind(c) == Kind::Array)
        .and_then(source::array);
    let Some(items) = items.filter(|items| !items.is_empty()) else {
        return Err(question_invalid(
            name,
            ".criteria must be a non-empty array",
        ));
    };
    if items.iter().any(|item| !guidance(Some(item))) {
        return Err(question_invalid(
            name,
            ".criteria entries must be strings, objects, or arrays",
        ));
    }
    Ok(())
}
fn question(name: &JsString16, value: &SourceJson) -> Check {
    if !plain(Some(value)) {
        return Err(question_invalid(name, " must be an object"));
    }
    if keys(value)
        .iter()
        .any(|key| !QUESTION_KEYS.iter().any(|known| same(key, known)))
    {
        return Err(question_invalid(name, " contains an unsupported field"));
    }
    let kind = source::member(value, "type")
        .and_then(|v| source::string(&v))
        .and_then(|t| QUESTION_TYPES.into_iter().find(|known| same(&t, known)));
    let Some(kind) = kind else {
        return Err(question_invalid(
            name,
            ".type must be noul, choice, score, or box",
        ));
    };
    if !guidance(source::member(value, "instructions").as_ref()) {
        return Err(question_invalid(
            name,
            ".instructions must be a string, object, or array",
        ));
    }
    let criteria = source::member(value, "criteria");
    match kind {
        "noul" => noul(criteria.as_ref(), name),
        "choice" => choice(criteria.as_ref(), name),
        "score" => score(criteria.as_ref(), name),
        _ if criteria.is_none() => Ok(()),
        _ => Err(question_invalid(
            name,
            ".criteria is not valid for box questions",
        )),
    }
}
fn questions(raw: &SourceJson) -> Check {
    let value = source::member(raw, "questions");
    let Some(value) = value.filter(|value| plain(Some(value))) else {
        return Err(invalid("questions must be an object", Some("questions")));
    };
    let fields = entries(&value);
    if fields.is_empty() || fields.len() > MAX_QUESTIONS {
        return Err(invalid(
            format!("questions must hold between 1 and {MAX_QUESTIONS} questions").as_str(),
            Some("questions"),
        ));
    }
    for (name, question_value) in &fields {
        if blank(name) || name.as_units().len() > MAX_QUESTION_NAME_LENGTH {
            return Err(invalid(
                "question names must be non-empty and at most 120 characters",
                Some("questions"),
            ));
        }
        question(name, question_value)?;
    }
    Ok(())
}
fn metadata(raw: &SourceJson) -> Check {
    let provider = source::member(raw, "provider");
    if provider
        .as_ref()
        .is_some_and(|p| !null(p) && !plain(Some(p)))
    {
        return Err(invalid(
            "provider must be an object or null",
            Some("provider"),
        ));
    }
    let trace = source::member(raw, "trace");
    if trace.as_ref().is_some_and(|t| !plain(Some(t))) {
        return Err(invalid("trace must be an object", Some("trace")));
    }
    for key in ["session_id", "user"] {
        let Some(value) = source::member(raw, key) else {
            continue;
        };
        let length = source::string(&value).map(|text| text.as_units().len());
        if !length.is_some_and(|length| length > 0 && length <= MAX_METADATA_LENGTH) {
            return Err(invalid(
                JsString16::from_utf8(&format!(
                    "{key} must be a non-empty string of at most {MAX_METADATA_LENGTH} characters"
                )),
                Some(key),
            ));
        }
    }
    Ok(())
}

/// Validates the documented envelope in source order; Err is the client refusal.
pub fn parse(raw: &SourceJson) -> Result<DecisionsRequest, SourceRequestError> {
    if kind(raw) == Kind::Other || kind(raw) == Kind::String {
        return Err(SourceRequestError::new(
            400,
            "Invalid JSON body",
            "invalid_request_error",
            Some("invalid_request_error"),
            None,
        ));
    }
    if let Some(key) = keys(raw)
        .into_iter()
        .find(|key| !REQUEST_KEYS.iter().any(|known| same(key, known)))
    {
        return Err(invalid(named("Unsupported key: ", &key, ""), None));
    }
    let model = model(raw)?;
    state(raw)?;
    questions(raw)?;
    metadata(raw)?;
    Ok(DecisionsRequest {
        model,
        raw: raw.clone(),
    })
}

/// The forwarded body: JSON.stringify of the validated members in source order.
pub fn forward_body(
    request: &DecisionsRequest,
    upstream: &JsString16,
) -> Result<SourceJson, GatewayError> {
    let mut fields = vec![("model", source::quoted(upstream)?)];
    for key in ["state", "questions"].into_iter().chain(OPTIONAL_KEYS) {
        if let Some(value) = source::member(&request.raw, key) {
            fields.push((key, source::canonical(&value)?));
        }
    }
    source::object(fields)
}
