use gateway_catalog_runtime::admin::runtime_error;
use gateway_core::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};
use gateway_schema::source_request::error::SourceRequestError;
use serde_json::Value;

fn member(value: &SourceJson, key: &str) -> Option<SourceJson> {
    source::member(value, key)
}

#[test]
fn runtime_error_keeps_opaque_model_units_and_original_status_mapping() {
    let message = JsString16::from_units(
        "runtime config default model is absent from the catalog: "
            .encode_utf16()
            .chain([0xd800])
            .collect(),
    );
    let error = runtime_error(SourceRequestError::new(
        503,
        message.clone(),
        "server_error",
        Some("runtime_config_invalid"),
        None,
    ));
    assert_eq!(error.status, 409);
    let fields = member(error.body(), "error");
    assert_eq!(
        fields
            .as_ref()
            .and_then(|f| member(f, "message"))
            .as_ref()
            .and_then(source::string),
        Some(message)
    );
    assert_eq!(
        fields
            .as_ref()
            .and_then(|f| member(f, "code"))
            .map(|code| code.as_str().to_owned()),
        Some("\"runtime_config_invalid\"".to_owned())
    );
    assert!(error.body().deserialize::<Value>().is_err());
    let large = runtime_error(SourceRequestError::from_gateway(GatewayError::new(
        503,
        "runtime config is too large",
        Some("runtime_config_invalid"),
    )));
    assert_eq!(large.status, 413);
}
