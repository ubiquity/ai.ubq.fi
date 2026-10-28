use gateway_provider_source::source as s;
use gateway_source::{
    js_string::JsString16, source_json::SourceJson, source_serialization as json,
};
use std::collections::{BTreeMap, BTreeSet};

pub fn roles(
    body: &mut SourceJson,
    field: &str,
    remove_reasoning: bool,
) -> Result<(), gateway_source::error::GatewayError> {
    if let Some(mut messages) = s::present(body, field).and_then(|observed| json::array(&observed))
    {
        for message in &mut messages {
            if s::present(message, "role").is_some_and(|observed| s::is(&observed, "developer")) {
                s::set(message, "role", s::text("system")?)?;
            }
            if remove_reasoning
                && s::present(message, "role").is_some_and(|observed| s::is(&observed, "assistant"))
            {
                s::remove(message, "reasoning_content")?;
            }
        }
        s::set(body, field, s::array(&messages)?)?;
    }
    Ok(())
}

fn schema(
    value: &SourceJson,
    properties: bool,
) -> Result<SourceJson, gateway_source::error::GatewayError> {
    if let Some(values) = json::array(value) {
        return s::array(
            &values
                .iter()
                .map(|v| schema(v, false))
                .collect::<Result<Vec<_>, _>>()?,
        );
    }
    let Some(fields) = json::entries(value) else {
        return Ok(value.clone());
    };
    let mut result = vec![];
    for (key, value) in fields {
        let scalar = key.try_utf8().ok();
        if !properties
            && scalar.as_deref().is_some_and(|key| {
                [
                    "format",
                    "maxItems",
                    "maxLength",
                    "maxProperties",
                    "minItems",
                    "minLength",
                    "minProperties",
                    "pattern",
                    "uniqueItems",
                ]
                .contains(&key)
            })
        {
            continue;
        }
        if !properties && scalar.as_deref() == Some("const") {
            result.push((
                JsString16::from_utf8("enum"),
                s::array(&[schema(&value, false)?])?,
            ));
        } else {
            result.push((
                if !properties && scalar.as_deref() == Some("oneOf") {
                    JsString16::from_utf8("anyOf")
                } else {
                    key
                },
                schema(&value, scalar.as_deref() == Some("properties"))?,
            ));
        }
    }
    s::object_keys(result)
}

pub fn cerebras_schema(
    value: &SourceJson,
) -> Result<SourceJson, gateway_source::error::GatewayError> {
    let projected = schema(value, false)?;
    if s::present(&projected, "type").is_some_and(|observed| s::is(&observed, "object")) {
        return Ok(projected);
    }
    let Some(variants) = s::present(&projected, "anyOf")
        .and_then(|value| json::array(&value))
        .filter(|values| !values.is_empty())
    else {
        return Ok(projected);
    };
    let mut rows = Vec::new();
    for variant in variants {
        if !s::present(&variant, "type").is_some_and(|value| s::is(&value, "object")) {
            return Ok(projected);
        }
        let Some(properties) =
            s::present(&variant, "properties").and_then(|value| json::entries(&value))
        else {
            return Ok(projected);
        };
        rows.push((properties, variant));
    }
    let mut properties: BTreeMap<JsString16, SourceJson> = BTreeMap::new();
    let mut required: Option<BTreeSet<JsString16>> = None;
    for (fields, variant) in rows {
        for (name, value) in fields {
            let value = match properties.get(&name).cloned() {
                Some(old) => merge_property(&name, old, value)?,
                None => value,
            };
            properties.insert(name, value);
        }
        let row = s::present(&variant, "required")
            .and_then(|observed| json::array(&observed))
            .unwrap_or_default()
            .iter()
            .filter_map(s::string)
            .collect::<BTreeSet<_>>();
        required = Some(required.map_or_else(
            || row.clone(),
            |old| old.intersection(&row).cloned().collect(),
        ));
    }
    s::object(vec![
        ("type", s::text("object")?),
        (
            "properties",
            s::object_keys(properties.into_iter().collect())?,
        ),
        (
            "required",
            s::array(
                &required
                    .unwrap_or_default()
                    .iter()
                    .map(s::quoted)
                    .collect::<Result<Vec<_>, _>>()?,
            )?,
        ),
        (
            "additionalProperties",
            s::scalar_json(&serde_json::json!(false))?,
        ),
    ])
}

pub fn cerebras(
    body: &mut SourceJson,
) -> Result<(), gateway_local_refusal::source_error::SourceRequestError> {
    roles(body, "messages", true)?;
    if let Some(messages) = s::present(body, "messages").and_then(|observed| json::array(&observed))
    {
        let mut system = vec![];
        let mut kept = vec![];
        for message in messages {
            if s::present(&message, "role").is_some_and(|observed| s::is(&observed, "system")) {
                system.push(system_text(s::present(&message, "content").as_ref())?);
            } else {
                kept.push(message);
            }
        }
        if !system.is_empty() {
            kept.insert(
                0,
                s::object(vec![
                    ("role", s::text("system")?),
                    ("content", s::quoted(&s::join(&system, "\n\n"))?),
                ])?,
            );
        }
        s::set(body, "messages", s::array(&kept)?)?;
    }
    if let Some(mut tools) = s::present(body, "tools").and_then(|observed| json::array(&observed)) {
        for tool in &mut tools {
            if let Some(mut function) = s::present(tool, "function")
                && let Some(parameters) = s::present(&function, "parameters")
            {
                s::set(&mut function, "parameters", cerebras_schema(&parameters)?)?;
                s::set(tool, "function", function)?;
            }
        }
        s::set(body, "tools", s::array(&tools)?)?;
    }
    Ok(())
}

pub fn prepare(
    body: &mut SourceJson,
) -> Result<(), gateway_local_refusal::source_error::SourceRequestError> {
    s::set(body, "stream", s::scalar_json(&serde_json::json!(false))?)?;
    for key in ["stream_options", "store", "metadata"] {
        s::remove(body, key)?;
    }
    if s::present(body, "logprobs").and_then(|value| s::boolean(&value)) != Some(true) {
        s::remove(body, "top_logprobs")?;
    }
    cerebras(body)
}

fn merge_property(
    name: &JsString16,
    old: SourceJson,
    value: SourceJson,
) -> Result<SourceJson, gateway_source::error::GatewayError> {
    if old == value {
        return Ok(old);
    }
    if name
        .as_units()
        .iter()
        .copied()
        .eq("operationId".encode_utf16())
        && let Some(mut values) = s::present(&old, "enum").and_then(|value| json::array(&value))
        && let Some(next) = s::present(&value, "enum").and_then(|value| json::array(&value))
    {
        for value in next {
            if !values.contains(&value) {
                values.push(value);
            }
        }
        return s::object(vec![("enum", s::array(&values)?)]);
    }
    let mut choices = s::present(&old, "anyOf")
        .and_then(|value| json::array(&value))
        .unwrap_or_else(|| vec![old]);
    if !choices.contains(&value) {
        choices.push(value);
    }
    s::object(vec![("anyOf", s::array(&choices)?)])
}

fn system_text(content: Option<&SourceJson>) -> s::Result<JsString16> {
    let invalid = || {
        s::invalid(
            JsString16::from_utf8(
                "Cerebras supports only text content in system and developer messages.",
            ),
            "messages",
        )
    };
    if let Some(text) = content.and_then(s::string) {
        return Ok(text);
    }
    let parts = content.and_then(json::array).ok_or_else(invalid)?;
    let mut text = Vec::new();
    for part in parts {
        if !s::present(&part, "type").is_some_and(|value| s::is(&value, "text")) {
            return Err(invalid());
        }
        text.push(
            s::present(&part, "text")
                .and_then(|value| s::string(&value))
                .ok_or_else(invalid)?,
        );
    }
    Ok(s::join(&text, ""))
}
