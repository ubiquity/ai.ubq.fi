pub fn is_claude_model(model: &gateway_contracts::model::ModelId) -> bool {
    let original = model.as_js_string().as_units();
    let without_tilde = original.strip_prefix(&[126]).unwrap_or(original);
    let prefix = "anthropic/".encode_utf16().collect::<Vec<_>>();
    let model = without_tilde
        .strip_prefix(prefix.as_slice())
        .unwrap_or(original);
    model.starts_with(&"claude-".encode_utf16().collect::<Vec<_>>())
}
