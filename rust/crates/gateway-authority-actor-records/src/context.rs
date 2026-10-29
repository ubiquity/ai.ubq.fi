//! Named new-row/source digest RequestContext codec. Historical/general serde remains scalar.
use gateway_contracts::{
    identity::ApiKeyId, identity::PrincipalId, identity::RequestContext, identity::RequestId,
};
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use serde::{Deserialize, Serialize};
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct ContextData {
    request_id: String,
    principal: JsString16,
    key_id: Option<String>,
    route: String,
    started_at_ms: i64,
}
pub fn to_source_json(context: &RequestContext) -> Result<SourceJson, GatewayError> {
    SourceJson::from_serializable(&ContextData {
        request_id: context.request_id.0.clone(),
        principal: context.principal.as_source().clone(),
        key_id: context.key_id.as_ref().map(|key| key.0.clone()),
        route: context.route.clone(),
        started_at_ms: context.started_at_ms,
    })
}
pub fn from_source_json(value: &SourceJson) -> Result<RequestContext, GatewayError> {
    let data: ContextData = value.deserialize()?;
    Ok(RequestContext {
        request_id: RequestId(data.request_id),
        principal: PrincipalId::from_source(data.principal),
        key_id: data.key_id.map(ApiKeyId),
        route: data.route,
        started_at_ms: data.started_at_ms,
    })
}
