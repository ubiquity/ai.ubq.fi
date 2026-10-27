use gateway_provider_tool_grammar::source_tools::ToolMap;
use crate::types::Elision;

#[derive(Clone, Debug)]
pub struct ChatProjection {
    pub body: gateway_core::source_json::SourceJson,
    pub tool_names: ToolMap,
    pub elisions: Vec<Elision>,
    pub warnings: Vec<String>,
}
