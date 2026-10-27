//! Retained Jev SystemOne compatibility validation; Decisions has its own schema.
use crate::{
    catalog::{equals, trim},
    request::RequestError,
};
use gateway_core::{
    js_string::JsString16, source_json::SourceJson, source_serialization as source,
};

fn invalid(message: impl Into<JsString16>) -> RequestError {
    RequestError {
        status: 400,
        message: message.into(),
        code: "invalid_request_error",
        param: None,
    }
}
pub fn parse(raw: &SourceJson) -> Result<SourceJson, RequestError> {
    let fields = source::entries(raw).ok_or_else(|| invalid("Invalid JSON body"))?;
    if let Some((key, _)) = fields.iter().find(|(key, _)| {
        !["state", "questions", "model"]
            .iter()
            .any(|name| equals(key, name))
    }) {
        let mut units = "Unsupported key: ".encode_utf16().collect::<Vec<_>>();
        units.extend_from_slice(key.as_units());
        return Err(invalid(JsString16::from_units(units)));
    }
    let state = source::member(raw, "state")
        .filter(|v| source::entries(v).is_some())
        .ok_or_else(|| invalid("state must be an object"))?;
    let questions =
        source::member(raw, "questions").ok_or_else(|| invalid("questions must be an object"))?;
    let rows = source::entries(&questions).ok_or_else(|| invalid("questions must be an object"))?;
    if rows.is_empty() || rows.len() > 24 {
        return Err(invalid("questions must hold between 1 and 24 questions"));
    }
    if rows.iter().any(|(name, question)| {
        trim(name).as_units().is_empty()
            || name.as_units().len() > 120
            || source::entries(question).is_none()
            || !source::member(question, "type")
                .as_ref()
                .and_then(source::string)
                .is_some_and(|kind| ["noul", "choice", "score"].iter().any(|s| equals(&kind, s)))
    }) {
        return Err(invalid(
            "questions must be typed noul, choice, or score objects",
        ));
    }
    let requested = source::member(raw, "model");
    let model = match requested {
        None => "~typesafe/jev-latest".into(),
        Some(value) => source::string(&value).ok_or_else(|| invalid("model must be a string"))?,
    };
    let model = model
        .try_utf8()
        .map_err(|_| invalid("model must be a typesafe model id"))?;
    let base = model.strip_prefix('~').unwrap_or(&model);
    let suffix = base.strip_prefix("typesafe/").unwrap_or("");
    if model.len() > 80
        || suffix.is_empty()
        || suffix.len() > 64
        || !suffix
            .chars()
            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || matches!(c, '.' | '_' | '-'))
        || !suffix
            .chars()
            .next()
            .is_some_and(|c| c.is_ascii_lowercase() || c.is_ascii_digit())
    {
        return Err(invalid("model must be a typesafe model id"));
    }
    let upstream = if model.starts_with('~') {
        model
    } else {
        format!("~{model}")
    };
    Ok(source::object(vec![
        ("model", SourceJson::from_serializable(&upstream)?),
        ("state", state),
        ("questions", questions),
    ])?)
}
