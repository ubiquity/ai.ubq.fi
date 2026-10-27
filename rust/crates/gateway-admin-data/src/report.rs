use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use serde::Serialize;
use std::collections::BTreeMap;

#[derive(Clone, Debug, Default, Serialize)]
pub struct Audit {
    pub counts: BTreeMap<String, usize>,
    pub errors: Vec<JsString16>,
}
impl Audit {
    pub fn count(&mut self, name: &str) {
        *self.counts.entry(name.into()).or_default() += 1;
    }
    pub fn merge(&mut self, other: Self) {
        self.counts.extend(other.counts);
        self.errors.extend(other.errors);
    }
    pub fn source_json(&self) -> Result<SourceJson, GatewayError> {
        SourceJson::from_serializable(self)
    }
    pub fn error_message(&self, prefix: &str) -> JsString16 {
        let mut units = prefix.encode_utf16().collect::<Vec<_>>();
        for (index, error) in self.errors.iter().enumerate() {
            if index != 0 {
                units.extend("; ".encode_utf16());
            }
            units.extend_from_slice(error.as_units());
        }
        JsString16::from_units(units)
    }
}
