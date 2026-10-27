use crate::types::{ExtrasRoute, ImageKind};
use gateway_core::error::GatewayError;

pub fn route(path: &str, method: &str) -> Result<Option<ExtrasRoute>, GatewayError> {
    let route = match path {
        "/uos/embeddings" => Some(("POST", ExtrasRoute::Embeddings)),
        "/uos/embedding-jobs" => Some(("POST", ExtrasRoute::JobCreate)),
        "/v1/decisions" => Some(("POST", ExtrasRoute::Decisions)),
        "/v1/systemone" => Some(("POST", ExtrasRoute::SystemOne)),
        "/v1/images/generations" => Some(("POST", ExtrasRoute::Images(ImageKind::Generations))),
        "/v1/images/edits" => Some(("POST", ExtrasRoute::Images(ImageKind::Edits))),
        _ => path
            .strip_prefix("/uos/embedding-jobs/")
            .map(|id| ("GET", ExtrasRoute::JobGet(id.into()))),
    };
    match route {
        Some((expected, _)) if expected != method => Err(GatewayError::new(
            405,
            "Method not allowed",
            Some("method_not_allowed"),
        )),
        Some((_, route)) => Ok(Some(route)),
        None => Ok(None),
    }
}

pub fn body_error(route: &ExtrasRoute, error: GatewayError) -> GatewayError {
    if *route == ExtrasRoute::Decisions && error.status == 413 {
        GatewayError::new(
            413,
            "The Decisions request body is too large",
            Some("decisions_request_too_large"),
        )
    } else {
        error
    }
}
