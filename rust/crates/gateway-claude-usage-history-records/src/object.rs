use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use serde::Serialize;
#[derive(Clone)]
pub struct SourceObject {
    entries: Vec<(JsString16, SourceJson)>,
}
impl SourceObject {
    pub fn parse(document: &SourceJson) -> Result<Self, GatewayError> {
        let document = SourceJson::parse(
            document
                .as_str()
                .trim_matches(|c| matches!(c, ' ' | '\n' | '\r' | '\t')),
        )?;
        let mut entries: Vec<(JsString16, SourceJson)> = Vec::new();
        for (key, value) in document.object_entries()? {
            if let Some((_, old)) = entries.iter_mut().find(|(name, _)| name == &key) {
                *old = value
            } else {
                entries.push((key, value))
            }
        }
        Ok(Self { entries })
    }
    pub fn get(&self, name: &str) -> Option<&SourceJson> {
        let name = JsString16::from_utf8(name);
        self.entries
            .iter()
            .find(|(key, _)| *key == name)
            .map(|(_, v)| v)
    }
    pub fn set<T: Serialize + ?Sized>(
        &mut self,
        name: &str,
        value: &T,
    ) -> Result<(), GatewayError> {
        let name = JsString16::from_utf8(name);
        let value = SourceJson::from_serializable(value)?;
        if let Some((_, old)) = self.entries.iter_mut().find(|(key, _)| *key == name) {
            *old = value
        } else {
            self.entries.push((name, value))
        }
        Ok(())
    }
    pub fn select(&self, names: &[&str]) -> Result<SourceJson, GatewayError> {
        let entries = self
            .entries
            .iter()
            .filter(|(key, _)| {
                names
                    .iter()
                    .any(|name| key.as_units().iter().copied().eq(name.encode_utf16()))
            })
            .cloned()
            .collect();
        Self { entries }.source_json()
    }
    pub fn source_json(&self) -> Result<SourceJson, GatewayError> {
        let mut text = String::from("{");
        for (index, (key, value)) in self.entries.iter().enumerate() {
            if index > 0 {
                text.push(',')
            }
            text.push_str(key.to_source_json()?.as_str());
            text.push(':');
            text.push_str(value.as_str());
        }
        text.push('}');
        SourceJson::parse(&text)
    }
}
