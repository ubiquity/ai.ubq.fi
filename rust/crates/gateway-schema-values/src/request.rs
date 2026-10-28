//! Retained request DATA; construction grants no validation or admission authority.
use gateway_source::js_string::JsString16;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RequestSurface {
    ChatCompletions,
    Responses,
}

#[derive(Clone, Debug, Default)]
pub struct RequestExtensions {
    pub deepseek_thinking: bool,
    pub openrouter_cache_control: bool,
}

#[derive(Clone, Debug, PartialEq)]
pub enum SourceField<T> {
    Missing,
    Null,
    Value(T),
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum SourceModelRequest {
    Default,
    Named(JsString16),
}
