//! Uploaded reasoning elements remain raw; provider strings are total ordinary DATA.
use gateway_source::{
    js_string::JsString16, source_json::SourceJson, source_serialization as source,
};
use serde::{Deserialize, Deserializer, Serialize, Serializer};

#[derive(Clone, Debug)]
pub struct AdvertisedLevel(Representation);
#[derive(Clone, Debug)]
enum Representation {
    Raw(SourceJson),
    ProviderString(JsString16),
}
impl AdvertisedLevel {
    pub(crate) fn provider(text: &str) -> Self {
        Self(Representation::ProviderString(text.into()))
    }
    pub fn advertised_effort(&self) -> Option<JsString16> {
        match &self.0 {
            Representation::Raw(raw) => crate::reasoning::advertised_effort(raw),
            Representation::ProviderString(text) => {
                let text = gateway_schema_values::source_view::js_trim_source(text);
                (!text.as_units().is_empty()).then_some(text)
            }
        }
    }
    pub fn string(&self) -> Option<JsString16> {
        match &self.0 {
            Representation::Raw(raw) => source::string(raw),
            Representation::ProviderString(text) => Some(text.clone()),
        }
    }
}
impl From<SourceJson> for AdvertisedLevel {
    fn from(raw: SourceJson) -> Self {
        Self(Representation::Raw(raw))
    }
}
impl PartialEq for AdvertisedLevel {
    fn eq(&self, other: &Self) -> bool {
        match (&self.0, &other.0) {
            (Representation::Raw(left), Representation::Raw(right)) => left == right,
            (Representation::ProviderString(left), Representation::ProviderString(right)) => {
                left == right
            }
            (Representation::Raw(raw), Representation::ProviderString(text))
            | (Representation::ProviderString(text), Representation::Raw(raw)) => {
                raw.as_str() == text.json_token()
            }
        }
    }
}
impl Serialize for AdvertisedLevel {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        match &self.0 {
            Representation::Raw(raw) => raw.serialize(serializer),
            Representation::ProviderString(text) => text.serialize(serializer),
        }
    }
}
impl<'de> Deserialize<'de> for AdvertisedLevel {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        SourceJson::deserialize(deserializer).map(Self::from)
    }
}
