//! Ordinary account model observations, preserving source UTF16 model identities.
use gateway_core::{js_string::JsString16, model::ModelId, provider::CodexQuotaClass};
use std::collections::BTreeMap;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct AccountCatalog {
    pub client_version: String,
    pub models: Vec<ModelId>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct UnsupportedModel {
    pub observed_at_ms: i64,
    pub detail: JsString16,
}

/// Transient hash-keyed view; the source account-model row uses raw account IDs.
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct ModelAvailability {
    pub catalogs: BTreeMap<String, AccountCatalog>,
    pub unsupported: BTreeMap<String, BTreeMap<ModelId, UnsupportedModel>>,
}

pub fn unsupported_detail(model: &ModelId) -> JsString16 {
    let mut units = "The '".encode_utf16().collect::<Vec<_>>();
    units.extend_from_slice(model.as_js_string().as_units());
    units
        .extend("' model is not supported when using Codex with a ChatGPT account.".encode_utf16());
    JsString16::from_units(units)
}

pub fn model_quota_class(model: &ModelId) -> CodexQuotaClass {
    let trimmed = gateway_schema::reasoning::js_trim_source(model.as_js_string());
    let lower = lower_control_units(trimmed.as_units());
    let same = |s: &str| lower.iter().copied().eq(s.encode_utf16());
    if same("gpt-5.3-codex-spark") {
        CodexQuotaClass::Spark
    } else if same("gpt-oss-120b") {
        CodexQuotaClass::GptOss120b
    } else if model_quota_label(model) == "gptreserve" {
        CodexQuotaClass::Reserve
    } else if lower.is_empty() {
        CodexQuotaClass::Unknown
    } else {
        CodexQuotaClass::Standard
    }
}
fn lower_control_units(units: &[u16]) -> Vec<u16> {
    gateway_core::js_string_case::lowercase_source(&JsString16::from_units(units.to_vec()))
        .as_units()
        .to_vec()
}
pub fn model_quota_label(model: &ModelId) -> String {
    lower_control_units(model.as_js_string().as_units())
        .into_iter()
        .filter_map(|u| {
            u8::try_from(u)
                .ok()
                .filter(u8::is_ascii_alphanumeric)
                .map(char::from)
        })
        .collect()
}
