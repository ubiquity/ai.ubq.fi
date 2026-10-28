use gateway_source::js_string::JsString16;
use serde::Deserialize;
use serde::Serialize;

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub enum ChatProfile {
    Deepseek,
    Lithos,
    Cerebras,
    Claude,
}

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct ToolIdentity {
    pub name: JsString16,
    pub namespace: Option<JsString16>,
    pub custom: bool,
}

pub fn default_effort(
    profile: ChatProfile,
    model: &gateway_contracts::model::ModelId,
) -> &'static str {
    match profile {
        ChatProfile::Deepseek => "high",
        ChatProfile::Lithos => "medium",
        _ => {
            if model.matches_scalar("qwen-3.8-27b") {
                "high"
            } else {
                "medium"
            }
        }
    }
}
