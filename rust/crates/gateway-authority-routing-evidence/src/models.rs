//! Ordinary account model observations, preserving source UTF16 model identities.
use gateway_core::{js_string::JsString16, model::ModelId};
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
