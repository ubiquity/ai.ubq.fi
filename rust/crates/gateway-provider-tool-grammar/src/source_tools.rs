/// Invalid grammar remains a native representability rejection; construction failure is explicit.
#[derive(Debug)]
pub enum ToolError {
    Request(gateway_local_refusal::source_error::SourceRequestError),
    Construction(gateway_source::error::GatewayError),
}
impl From<gateway_local_refusal::source_error::SourceRequestError> for ToolError {
    fn from(error: gateway_local_refusal::source_error::SourceRequestError) -> Self {
        Self::Request(error)
    }
}
impl From<gateway_source::error::GatewayError> for ToolError {
    fn from(error: gateway_source::error::GatewayError) -> Self {
        Self::Construction(error)
    }
}
impl From<ToolError> for gateway_local_refusal::source_error::SourceRequestError {
    fn from(error: ToolError) -> Self {
        match error {
            ToolError::Request(error) => error,
            ToolError::Construction(error) => Self::from_gateway(error),
        }
    }
}
use gateway_provider_source::source as s;
use gateway_provider_source::types::ToolIdentity;
use gateway_source::js_string::JsString16;
use gateway_source::source_json::SourceJson;
use gateway_source::source_serialization as json;
use serde::Deserialize;
use serde::Deserializer;
use serde::Serialize;
use serde::Serializer;
use std::collections::BTreeMap;
use std::collections::BTreeSet;

#[derive(Clone, Debug, Default, PartialEq)]
pub struct ToolMap {
    entries: Vec<(JsString16, ToolIdentity)>,
    positions: BTreeMap<JsString16, usize>,
}

impl ToolMap {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn len(&self) -> usize {
        self.entries.len()
    }
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }
    pub fn get(&self, wire: &str) -> Option<&ToolIdentity> {
        self.get_source(&JsString16::from_utf8(wire))
    }
    pub fn get_source(&self, wire: &JsString16) -> Option<&ToolIdentity> {
        self.positions
            .get(wire)
            .map(|&position| &self.entries[position].1)
    }
    pub fn insert(&mut self, wire: String, identity: ToolIdentity) -> Option<ToolIdentity> {
        self.insert_source(JsString16::from_utf8(&wire), identity)
    }
    pub fn insert_source(
        &mut self,
        wire: JsString16,
        identity: ToolIdentity,
    ) -> Option<ToolIdentity> {
        if let Some(&position) = self.positions.get(&wire) {
            Some(std::mem::replace(&mut self.entries[position].1, identity))
        } else {
            self.positions.insert(wire.clone(), self.entries.len());
            self.entries.push((wire, identity));
            None
        }
    }
    pub fn iter(&self) -> impl Iterator<Item = (&JsString16, &ToolIdentity)> {
        self.entries.iter().map(|(wire, identity)| (wire, identity))
    }
}

impl Serialize for ToolMap {
    fn serialize<S: Serializer>(&self, serializer: S) -> std::result::Result<S::Ok, S::Error> {
        let fields = self
            .entries
            .iter()
            .map(|(wire, identity)| {
                SourceJson::from_serializable(identity).map(|identity| (wire.clone(), identity))
            })
            .collect::<std::result::Result<Vec<_>, _>>()
            .map_err(serde::ser::Error::custom)?;
        json::object_keys(fields)
            .map_err(serde::ser::Error::custom)?
            .serialize(serializer)
    }
}

impl<'de> Deserialize<'de> for ToolMap {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> std::result::Result<Self, D::Error> {
        let raw = <SourceJson as Deserialize>::deserialize(deserializer)?;
        let fields = json::entries(&raw)
            .ok_or_else(|| serde::de::Error::custom("Expected source tool map"))?;
        let mut map = Self::new();
        for (wire, identity) in fields {
            map.insert_source(
                wire,
                identity.deserialize().map_err(serde::de::Error::custom)?,
            );
        }
        Ok(map)
    }
}

pub fn flatten(
    value: Option<&SourceJson>,
    limit: usize,
) -> Result<(Vec<SourceJson>, ToolMap), ToolError> {
    let mut tools = vec![];
    let mut names = ToolMap::new();
    let mut used = BTreeSet::new();
    let Some(value) = value else {
        return Ok((tools, names));
    };
    let values = json::array(value)
        .ok_or_else(|| s::invalid(JsString16::from_utf8("tools must be an array"), "tools"))?;
    for tool in values {
        if json::entries(&tool).is_none() {
            return Err(
                s::invalid(JsString16::from_utf8("tools must contain objects"), "tools").into(),
            );
        }
        if s::present(&tool, "type").is_some_and(|observed| s::is(&observed, "namespace")) {
            let nested = s::present(&tool, "tools")
                .and_then(|observed| json::array(&observed))
                .ok_or_else(|| {
                    s::invalid(
                        JsString16::from_utf8("namespace tools must nest a tools array"),
                        "tools.tools",
                    )
                })?;
            let namespace = s::present(&tool, "name").and_then(|observed| s::string(&observed));
            for entry in nested {
                if json::entries(&entry).is_none() {
                    return Err(s::invalid(
                        JsString16::from_utf8("namespace tools must contain objects"),
                        "tools.tools",
                    )
                    .into());
                }
                if s::present(&entry, "type").is_some_and(|observed| s::is(&observed, "function")) {
                    collect(
                        &entry,
                        namespace.as_ref(),
                        limit,
                        &mut tools,
                        &mut names,
                        &mut used,
                    )?;
                }
            }
        } else {
            collect(&tool, None, limit, &mut tools, &mut names, &mut used)?;
        }
    }
    Ok((tools, names))
}

fn collect(
    tool: &SourceJson,
    namespace: Option<&JsString16>,
    limit: usize,
    tools: &mut Vec<SourceJson>,
    names: &mut ToolMap,
    used: &mut BTreeSet<JsString16>,
) -> Result<(), ToolError> {
    let custom = s::present(tool, "type").is_some_and(|observed| s::is(&observed, "custom"));
    let nested = s::present(tool, "function");
    let nested_object = nested
        .as_ref()
        .filter(|value| json::entries(value).is_some());
    let function = if custom {
        tool
    } else {
        nested_object.unwrap_or(tool)
    };
    if !custom
        && nested_object.is_none()
        && !s::present(tool, "type").is_some_and(|observed| s::is(&observed, "function"))
    {
        return Ok(());
    }
    let name = s::present(function, "name")
        .and_then(|observed| s::string(&observed))
        .filter(|value| !value.as_units().is_empty())
        .ok_or_else(|| {
            s::invalid(
                JsString16::from_utf8(if custom {
                    "custom tools require a name"
                } else {
                    "function tools require a name"
                }),
                "tools.name",
            )
        })?;
    let preferred = if used.contains(&name) {
        namespace
            .map(|namespace| s::concat(&[namespace, &JsString16::from_utf8("_"), &name]))
            .unwrap_or_else(|| name.clone())
    } else {
        name.clone()
    };
    let mut projected = preferred.prefix_units(limit);
    let mut suffix = 2usize;
    while used.contains(&projected) {
        let ending = JsString16::from_utf8(&format!("_{suffix}"));
        projected = s::concat(&[
            &preferred.prefix_units(limit.saturating_sub(ending.as_units().len())),
            &ending,
        ]);
        suffix += 1;
    }
    used.insert(projected.clone());
    names.insert_source(
        projected.clone(),
        ToolIdentity {
            name,
            namespace: namespace.cloned(),
            custom,
        },
    );
    let mut body = s::object(vec![("name", s::quoted(&projected)?)])?;
    if let Some(description) =
        s::present(function, "description").filter(|value| s::string(value).is_some())
    {
        s::set(&mut body, "description", description)?;
    }
    if custom {
        s::set(
            &mut body,
            "parameters",
            s::scalar_json(
                &serde_json::json!({"type":"object","properties":{"input":{"type":"string","description":"Freeform input for the tool."}},"required":["input"],"additionalProperties":false}),
            )?,
        )?;
    } else {
        if let Some(parameters) = s::present(function, "parameters") {
            s::set(&mut body, "parameters", parameters)?;
        }
        if let Some(strict) =
            s::present(function, "strict").filter(|value| s::boolean(value).is_some())
        {
            s::set(&mut body, "strict", strict)?;
        }
    }
    tools.push(s::object(vec![
        ("type", s::text("function")?),
        ("function", body),
    ])?);
    Ok(())
}

pub fn choice(
    value: Option<&SourceJson>,
    names: &ToolMap,
) -> Result<Option<SourceJson>, ToolError> {
    let Some(value) = value.filter(|value| !s::is_null(value)) else {
        return Ok(None);
    };
    if let Some(choice) = s::string(value) {
        return if ["none", "auto", "required"]
            .iter()
            .any(|v| choice.as_units().iter().copied().eq(v.encode_utf16()))
        {
            Ok(Some(value.clone()))
        } else {
            Err(s::invalid(
                s::concat(&[
                    &JsString16::from_utf8("tool_choice '"),
                    &choice,
                    &JsString16::from_utf8("' is not supported"),
                ]),
                "tool_choice",
            )
            .into())
        };
    }
    if json::entries(value).is_none() {
        return Err(s::invalid(
            JsString16::from_utf8("tool_choice must be a string or an object"),
            "tool_choice",
        )
        .into());
    }
    let function = s::present(value, "function");
    let name = s::present(value, "name")
        .and_then(|observed| s::string(&observed))
        .or_else(|| {
            function
                .as_ref()
                .and_then(|function| s::present(function, "name"))
                .and_then(|observed| s::string(&observed))
        })
        .filter(|value| !value.as_units().is_empty())
        .ok_or_else(|| {
            s::invalid(
                JsString16::from_utf8("a named tool_choice requires a name"),
                "tool_choice.name",
            )
        })?;
    let namespace = s::present(value, "namespace")
        .or_else(|| {
            function
                .as_ref()
                .and_then(|function| s::present(function, "namespace"))
        })
        .and_then(|v| s::string(&v));
    let projected = names
        .iter()
        .find(|(_, identity)| identity.name == name && identity.namespace == namespace)
        .map(|(wire, _)| wire.clone())
        .unwrap_or_else(|| {
            if namespace.is_none() {
                names
                    .get_source(&name)
                    .map(|identity| identity.name.clone())
                    .unwrap_or(name)
            } else {
                name
            }
        });
    Ok(Some(s::object(vec![
        ("type", s::text("function")?),
        (
            "function",
            s::object(vec![("name", s::quoted(&projected)?)])?,
        ),
    ])?))
}
