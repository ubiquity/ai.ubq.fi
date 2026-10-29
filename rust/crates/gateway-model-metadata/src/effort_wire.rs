//! Finite reasoning wire DATA; only the existing factories normalize its string pairs.
use gateway_source::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use serde::{Deserialize, Deserializer, Serialize, Serializer};

#[derive(Clone, Debug)]
pub struct EffortWireMap(Representation);
#[derive(Clone, Debug)]
enum Representation {
    Raw(SourceJson),
    Normalized(Vec<(JsString16, JsString16)>),
}
impl EffortWireMap {
    pub(crate) fn normalized(pairs: Vec<(JsString16, JsString16)>) -> Self {
        Self(Representation::Normalized(pairs))
    }
    pub fn wire_effort(&self, effort: &JsString16) -> Option<JsString16> {
        match &self.0 {
            Representation::Raw(raw) => source::entries(raw)?
                .into_iter()
                .find(|(key, _)| key == effort)
                .and_then(|(_, value)| source::string(&value)),
            Representation::Normalized(pairs) => pairs
                .iter()
                .find(|(key, _)| key == effort)
                .map(|(_, value)| value.clone()),
        }
    }
    pub fn to_source_json(&self) -> Result<SourceJson, GatewayError> {
        match &self.0 {
            Representation::Raw(raw) => Ok(raw.clone()),
            Representation::Normalized(pairs) => source::object_keys(
                pairs
                    .iter()
                    .map(|(key, value)| Ok((key.clone(), source::quoted(value)?)))
                    .collect::<Result<_, GatewayError>>()?,
            ),
        }
    }
}
impl From<SourceJson> for EffortWireMap {
    fn from(raw: SourceJson) -> Self {
        Self(Representation::Raw(raw))
    }
}
// This is a byte comparison of a finite DATA shape, not a SourceJson constructor.
fn pair_bytes(pairs: &[(JsString16, JsString16)]) -> String {
    let mut text = String::from("{");
    for (index, (key, value)) in pairs.iter().enumerate() {
        if index > 0 {
            text.push(',');
        }
        text.push_str(&key.json_token());
        text.push(':');
        text.push_str(&value.json_token());
    }
    text.push('}');
    text
}
impl PartialEq for EffortWireMap {
    fn eq(&self, other: &Self) -> bool {
        match (&self.0, &other.0) {
            (Representation::Raw(left), Representation::Raw(right)) => left == right,
            (Representation::Normalized(left), Representation::Normalized(right)) => left == right,
            (Representation::Raw(raw), Representation::Normalized(pairs))
            | (Representation::Normalized(pairs), Representation::Raw(raw)) => {
                raw.as_str() == pair_bytes(pairs)
            }
        }
    }
}
impl Serialize for EffortWireMap {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        self.to_source_json()
            .map_err(|_| serde::ser::Error::custom("Invalid reasoning wire JSON"))?
            .serialize(serializer)
    }
}
impl<'de> Deserialize<'de> for EffortWireMap {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        SourceJson::deserialize(deserializer).map(Self::from)
    }
}
