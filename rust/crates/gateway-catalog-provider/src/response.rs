//! Catalog wire responses preserve the source document and error envelope.
use gateway_source::source_json::SourceJson;
use gateway_schema::source_request::error::SourceRequestError;
use std::collections::BTreeMap;

#[derive(Clone, Debug, PartialEq)]
pub struct AdminCatalogResponse {
    pub status: u16,
    pub body: SourceJson,
    pub headers: BTreeMap<String, String>,
}
impl AdminCatalogResponse {
    pub fn json(body: SourceJson, no_store: bool) -> Self {
        let mut headers = BTreeMap::from([("content-type".into(), "application/json".into())]);
        if no_store {
            headers.insert("cache-control".into(), "no-store".into());
        }
        Self {
            status: 200,
            body,
            headers,
        }
    }
}
pub fn into_auth_response(
    response: Result<AdminCatalogResponse, SourceRequestError>,
) -> gateway_auth::types::AuthResponse {
    match response {
        Ok(response) => {
            let mut wire =
                gateway_auth::types::AuthResponse::source_json(response.status, response.body);
            wire.headers.extend(response.headers);
            wire
        }
        Err(error) => {
            gateway_auth::types::AuthResponse::source_json(error.status, error.body().clone())
        }
    }
}
