//! Exact source error strings may themselves contain non-scalar UTF16 units.
use crate::closed_error_body::ClosedErrorBody;
use gateway_source::{error::GatewayError, js_string::JsString16};

#[derive(Clone, Debug)]
pub struct SourceRequestError {
    pub status: u16,
    body: Box<ClosedErrorBody>,
    diagnostic: Box<GatewayError>,
    local_aggregate_context_overflow: bool,
}
impl SourceRequestError {
    pub fn new(
        status: u16,
        message: impl Into<JsString16>,
        error_type: &str,
        code: Option<&str>,
        param: Option<Option<JsString16>>,
    ) -> Self {
        let body = Box::new(ClosedErrorBody::new(
            message.into(),
            error_type,
            code,
            param,
        ));
        Self {
            status,
            local_aggregate_context_overflow: false,
            body,
            diagnostic: Box::new(
                GatewayError::new(status, "Invalid source request.", code).with_type(error_type),
            ),
        }
    }
    /// Internal data classification only; no serialized field can select this kind.
    pub fn aggregate_context_overflow(message: impl Into<JsString16>) -> Self {
        let mut error = Self::new(
            400,
            message,
            "invalid_request_error",
            Some("context_length_exceeded"),
            Some(Some(JsString16::from_utf8("input"))),
        );
        error.local_aggregate_context_overflow = true;
        error
    }
    pub fn is_local_aggregate_context_overflow(&self) -> bool {
        self.local_aggregate_context_overflow
    }
    pub fn invalid(message: impl Into<JsString16>, param: Option<JsString16>) -> Self {
        Self::new(
            400,
            message,
            "invalid_request_error",
            Some("invalid_request_error"),
            param.map(Some),
        )
    }
    /// Lift an existing scalar error without altering status, fields, or exact body.
    pub fn from_gateway(error: GatewayError) -> Self {
        Self::from_scalar(error)
    }
    pub(crate) fn from_scalar(error: GatewayError) -> Self {
        let body = Box::new(ClosedErrorBody::from_scalar(&error));
        Self {
            status: error.status,
            local_aggregate_context_overflow: false,
            body,
            diagnostic: Box::new(error),
        }
    }
    pub fn body(&self) -> &ClosedErrorBody {
        &self.body
    }
    pub fn diagnostic(&self) -> &GatewayError {
        &self.diagnostic
    }
    pub fn scalar(message: &str, param: Option<&str>) -> Self {
        Self::invalid(
            JsString16::from_utf8(message),
            param.map(JsString16::from_utf8),
        )
    }
    /// Only the old scalar API calls this after supplying an actual serde Value.
    pub fn into_scalar(self) -> GatewayError {
        let message = match self.body.message().try_utf8() {
            Ok(message) => message,
            Err(_) => return *self.diagnostic,
        };
        let param = match self.body.param() {
            Some(Some(param)) => match param.try_utf8() {
                Ok(param) => Some(Some(param)),
                Err(_) => return *self.diagnostic,
            },
            Some(None) => Some(None),
            None => None,
        };
        let mut error = GatewayError::new(self.status, message, self.body.code())
            .with_type(self.body.error_type());
        if let Some(param) = param {
            error = error.with_param(param.as_deref());
        }
        error
    }
}

impl From<GatewayError> for SourceRequestError {
    fn from(error: GatewayError) -> Self {
        Self::from_gateway(error)
    }
}
