//! The finite error envelope only: original fields and immutable JSON wire text.
use gateway_source::{error::GatewayError, js_string::JsString16};

#[derive(Clone, Eq, PartialEq)]
pub struct ClosedErrorBody {
    message: JsString16,
    error_type: String,
    code: Option<String>,
    param: Option<Option<JsString16>>,
    wire: String,
}
impl std::fmt::Debug for ClosedErrorBody {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("ClosedErrorBody")
            .field("bytes", &self.wire.len())
            .finish()
    }
}
impl ClosedErrorBody {
    pub(crate) fn new(
        message: JsString16,
        error_type: &str,
        code: Option<&str>,
        param: Option<Option<JsString16>>,
    ) -> Self {
        let mut wire = String::from("{\"error\":{\"message\":");
        wire.push_str(&message.json_token());
        wire.push_str(",\"type\":");
        wire.push_str(&JsString16::from_utf8(error_type).json_token());
        if let Some(code) = code {
            wire.push_str(",\"code\":");
            wire.push_str(&JsString16::from_utf8(code).json_token());
        }
        if let Some(param) = &param {
            wire.push_str(",\"param\":");
            match param {
                Some(param) => wire.push_str(&param.json_token()),
                None => wire.push_str("null"),
            }
        }
        wire.push_str("}}");
        Self {
            message,
            error_type: error_type.to_owned(),
            code: code.map(str::to_owned),
            param,
            wire,
        }
    }
    pub(crate) fn from_scalar(error: &GatewayError) -> Self {
        let fields = &error.envelope.error;
        Self::new(
            JsString16::from_utf8(&fields.message),
            &fields.error_type,
            fields.code.as_deref(),
            fields
                .param
                .as_ref()
                .map(|param| param.as_deref().map(JsString16::from_utf8)),
        )
    }
    pub fn as_str(&self) -> &str {
        &self.wire
    }
    pub fn as_bytes(&self) -> &[u8] {
        self.wire.as_bytes()
    }
    pub fn message(&self) -> &JsString16 {
        &self.message
    }
    pub fn error_type(&self) -> &str {
        &self.error_type
    }
    pub fn code(&self) -> Option<&str> {
        self.code.as_deref()
    }
    /// None omits the field; Some(None) is JSON null; Some(Some(_)) is text.
    pub fn param(&self) -> Option<Option<&JsString16>> {
        self.param.as_ref().map(Option::as_ref)
    }
}
