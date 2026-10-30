//! Keep platform responses intact where workers-rs would drop status text or an upgrade.
use gateway_source::error::GatewayError;
use worker::{Headers, IntoResponse, Response};

pub fn inference_not_found() -> GatewayError {
    GatewayError::new(404, "Not found", Some("not_found"))
}

pub fn public_not_found() -> GatewayError {
    GatewayError::new(
        404,
        "Public resource not found. Read /openapi.json for API endpoints or /llms.txt for agent guidance.",
        Some("not_found"),
    )
    .with_type("not_found")
}

pub enum GatewayResponse {
    Http(Response),
    Raw(worker::web_sys::Response),
}
impl From<Response> for GatewayResponse {
    fn from(response: Response) -> Self {
        Self::Http(response)
    }
}
impl GatewayResponse {
    pub fn status_code(&self) -> u16 {
        match self {
            Self::Http(response) => response.status_code(),
            Self::Raw(response) => response.status(),
        }
    }
    pub fn headers(&self) -> Headers {
        match self {
            // workers-rs Headers::clone copies entries; clone the JS handle so
            // response decoration mutates the actual outgoing headers instead.
            Self::Http(response) => Headers(response.headers().0.clone()),
            Self::Raw(response) => Headers(response.headers()),
        }
    }
    pub fn headers_mut(&mut self) -> Headers {
        self.headers()
    }
}
impl IntoResponse for GatewayResponse {
    fn into_raw(self) -> Result<worker::web_sys::Response, impl Into<Box<dyn std::error::Error>>> {
        Ok::<_, worker::Error>(match self {
            Self::Http(response) => response.into(),
            Self::Raw(response) => response,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn terminal_and_public_not_found_envelopes_stay_distinct() {
        let terminal = inference_not_found();
        assert_eq!(terminal.status, 404);
        assert_eq!(terminal.envelope.error.message, "Not found");
        assert_eq!(terminal.envelope.error.error_type, "invalid_request_error");
        assert_eq!(terminal.envelope.error.code.as_deref(), Some("not_found"));
        assert!(terminal.envelope.error.param.is_none());

        let public = public_not_found();
        assert_eq!(public.status, 404);
        assert_eq!(
            public.envelope.error.message,
            "Public resource not found. Read /openapi.json for API endpoints or /llms.txt for agent guidance."
        );
        assert_eq!(public.envelope.error.error_type, "not_found");
        assert_eq!(public.envelope.error.code.as_deref(), Some("not_found"));
        assert!(public.envelope.error.param.is_none());
    }
}
