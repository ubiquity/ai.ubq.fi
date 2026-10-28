use gateway_provider_source::anthropic::is_claude_model;

pub fn is_claude(model: &str) -> bool {
    is_claude_model(&gateway_contracts::model::ModelId::from_scalar(model))
}
