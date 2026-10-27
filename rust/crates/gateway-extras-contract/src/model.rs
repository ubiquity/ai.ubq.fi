use gateway_contracts::model::ModelId;
use gateway_source::{source_json::SourceJson, source_serialization as source};
use gateway_schema::catalog::CatalogInput;
pub fn runtime_default_model(runtime: &SourceJson) -> Option<ModelId> {
    source::member(runtime, "default_model")
        .as_ref()
        .and_then(source::string)
        .map(ModelId::from_source)
}
pub fn default_model(runtime: Option<&SourceJson>, input: &CatalogInput) -> Option<ModelId> {
    runtime
        .and_then(runtime_default_model)
        .or_else(|| input.codex().and_then(|snapshot| snapshot.default_model()))
}
