use crate::types::Elision;
use gateway_provider_tool_grammar::source_tools::ToolMap;

#[derive(Clone, Debug)]
pub struct ChatProjection {
    pub body: gateway_source::source_json::SourceJson,
    pub tool_names: ToolMap,
    pub elisions: Vec<Elision>,
    pub warnings: Vec<String>,
}
