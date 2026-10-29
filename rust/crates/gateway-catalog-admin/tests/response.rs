use gateway_catalog_admin::response::{AdminCatalogResponse, into_auth_response};
use gateway_local_refusal::source_error::SourceRequestError;
use gateway_source::{js_string::JsString16, source_json::SourceJson};
use std::collections::BTreeMap;

#[test]
fn public_catalog_rendering_retains_raw_models_and_source_error_fields()
-> Result<(), Box<dyn std::error::Error>> {
    let body = SourceJson::parse(r#"{"models":[{"id":"model-\ud800"}]}"#)?;
    let response = into_auth_response(Ok(AdminCatalogResponse {
        status: 200,
        body: body.clone(),
        headers: BTreeMap::from([("x-fixture".into(), "source".into())]),
    }));
    assert_eq!(response.status, 200);
    assert_eq!(response.body.to_bytes()?, body.as_bytes());
    assert_eq!(response.headers["x-fixture"], "source");
    assert_eq!(response.headers["cache-control"], "no-store");
    let error = SourceRequestError::new(
        400,
        JsString16::from_units(vec![0xdc00]),
        "invalid_request_error",
        Some("invalid_request_error"),
        Some(Some(JsString16::from_units(vec![0xd800]))),
    );
    let expected = error.body().as_bytes().to_vec();
    let response = into_auth_response(Err(error));
    assert_eq!(response.status, 400);
    assert_eq!(response.body.to_bytes()?, expected);
    assert_eq!(response.headers["content-type"], "application/json");
    Ok(())
}
