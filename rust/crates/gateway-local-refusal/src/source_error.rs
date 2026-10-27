//! Exact source error strings may themselves contain non-scalar UTF16 units.
use gateway_core::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use serde::Serialize;

#[derive(Clone, Debug)]
pub struct SourceRequestError {
    pub status: u16,
    body: SourceJson,
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
        #[derive(Serialize)]
        struct ErrorBody<'a> {
            error: ErrorFields<'a>,
        }
        #[derive(Serialize)]
        struct ErrorFields<'a> {
            message: JsString16,
            #[serde(rename = "type")]
            kind: &'a str,
            #[serde(skip_serializing_if = "Option::is_none")]
            code: Option<&'a str>,
            #[serde(skip_serializing_if = "Option::is_none")]
            param: Option<Option<JsString16>>,
        }
        let body = SourceJson::from_serializable(&ErrorBody {
            error: ErrorFields {
                message: message.into(),
                kind: error_type,
                code,
                param,
            },
        })
        .expect("Source error fields are valid JSON");
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
        let body = SourceJson::from_serializable(&error.envelope)
            .expect("Scalar error fields are valid JSON");
        Self {
            status: error.status,
            local_aggregate_context_overflow: false,
            body,
            diagnostic: Box::new(error),
        }
    }
    pub fn body(&self) -> &SourceJson {
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
        #[derive(serde::Deserialize)]
        struct Body {
            error: Fields,
        }
        #[derive(serde::Deserialize)]
        struct Fields {
            message: String,
            #[serde(rename = "type")]
            kind: String,
            code: Option<String>,
            param: Option<String>,
        }
        match self.body.deserialize::<Body>() {
            Ok(body) => {
                let fields = body.error;
                let mut error =
                    GatewayError::new(self.status, fields.message, fields.code.as_deref())
                        .with_type(&fields.kind);
                if let Some(param) = fields.param {
                    error = error.with_param(Some(&param));
                } else if gateway_core::source_serialization::member(&self.body, "error")
                    .and_then(|body| gateway_core::source_serialization::member(&body, "param"))
                    .is_some()
                {
                    error = error.with_param(None);
                }
                error
            }
            Err(_) => *self.diagnostic,
        }
    }
}
