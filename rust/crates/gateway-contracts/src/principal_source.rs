//! Explicit lossless RequestContext DATA codec. Historical scalar serde is unchanged.
use crate::identity::{ApiKeyId, PrincipalId, RequestContext, RequestId, UnixMillis};
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use serde::{Deserialize, Serialize};

#[derive(Deserialize, Serialize)]
struct SourceContext {
    request_id: RequestId,
    principal: JsString16,
    key_id: Option<ApiKeyId>,
    route: String,
    started_at_ms: UnixMillis,
}
impl RequestContext {
    pub fn to_source_json(&self) -> Result<SourceJson, GatewayError> {
        SourceJson::from_serializable(&SourceContext {
            request_id: self.request_id.clone(),
            principal: self.principal.as_source().clone(),
            key_id: self.key_id.clone(),
            route: self.route.clone(),
            started_at_ms: self.started_at_ms,
        })
    }
    pub fn from_source_json(value: &SourceJson) -> Result<Self, GatewayError> {
        let value: SourceContext = value.deserialize()?;
        Ok(Self {
            request_id: value.request_id,
            principal: PrincipalId::from_source(value.principal),
            key_id: value.key_id,
            route: value.route,
            started_at_ms: value.started_at_ms,
        })
    }
}
