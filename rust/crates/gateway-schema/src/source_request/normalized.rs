//! Public source normalization results borrow no scalar transcript representation.
use gateway_local_refusal::source_error::SourceRequestError;
use gateway_schema_values::value::Value;
use gateway_source::{js_string::JsString16, source_json::SourceJson};
#[derive(Clone, Debug, PartialEq)]
pub struct SourceChatMessage {
    pub instruction: Option<JsString16>,
    pub instruction_content: Option<Vec<SourceJson>>,
    pub input: Vec<SourceJson>,
}
#[derive(Clone, Debug, PartialEq)]
pub struct SourceChatInput {
    pub input: Vec<SourceJson>,
    pub instructions: Option<JsString16>,
}
fn output(value: Value) -> Result<SourceJson, SourceRequestError> {
    value.source().map_err(SourceRequestError::from_gateway)
}
fn values(values: Vec<Value>) -> Result<Vec<SourceJson>, SourceRequestError> {
    values.into_iter().map(output).collect()
}
pub fn normalize_chat_message(
    value: &SourceJson,
    index: usize,
) -> Result<SourceChatMessage, SourceRequestError> {
    let value =
        super::chat_engine::normalize_chat_message(&Value::from_source(value.clone()), index)?;
    Ok(SourceChatMessage {
        instruction: value.instruction.map(|text| text.core()),
        instruction_content: value.instruction_content.map(values).transpose()?,
        input: values(value.input)?,
    })
}
pub fn normalize_chat_messages(value: &SourceJson) -> Result<SourceChatInput, SourceRequestError> {
    let value = super::chat_engine::normalize_chat_messages(&Value::from_source(value.clone()))?;
    Ok(SourceChatInput {
        input: values(value.input)?,
        instructions: value.instructions.map(|text| text.core()),
    })
}
