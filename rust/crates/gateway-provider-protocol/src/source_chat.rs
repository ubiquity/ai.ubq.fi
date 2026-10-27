use gateway_provider_tool_grammar::source_tools::ToolMap;
use crate::types::Elision;
use gateway_core::source_json::SourceJson;

pub struct Projection {
    pub body: SourceJson,
    pub names: ToolMap,
    pub elisions: Vec<Elision>,
    pub warnings: Vec<String>,
    pub exemptions: Vec<gateway_prompt_policy::summary::SummaryExemption>,
    pub admission: Option<gateway_prompt_policy::admission::PromptAdmission>,
}
