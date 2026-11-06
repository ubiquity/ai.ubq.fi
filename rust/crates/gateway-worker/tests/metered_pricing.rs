//! Metered pricing projection keeps exact source units, fixed prices and fail-closed metadata.
use gateway_core::error::GatewayError;
use gateway_core::js_string::JsString16;
use gateway_core::model::ModelId;
use gateway_core::source_json::SourceJson;
use gateway_worker::admin_support_host::pricing::{parse_metadata, project};

type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;

fn json(text: &str) -> Result<SourceJson, GatewayError> {
    SourceJson::parse(text)
}

const STATUS: &str = r#"{"success":true,"data":{"setup":true,"quota_per_unit":500000}}"#;

#[test]
fn source_fixed_prices_override_ratios_without_advertising_unknown_models() -> TestResult {
    let pricing = json(
        r#"{"success":true,"data":{"model_ratio":{"a":2},"model_price":{"a":7,"other":1},"completion_ratio":{"a":3}}}"#,
    )?;
    let snapshot = project(
        &["a".into(), "a".into(), "absent".into()],
        &pricing,
        &json(STATUS)?,
        10,
    )?;
    assert_eq!(snapshot.eligible_model_ids, vec![ModelId::from_scalar("a")]);
    assert_eq!(
        snapshot.model_quota_coefficients[&ModelId::from_scalar("a")],
        7.0
    );
    Ok(())
}

#[test]
fn invalid_quota_and_metadata_fail_before_any_key_policy_write() -> TestResult {
    assert!(project(&[], &json(r#"{"success":false}"#)?, &json("null")?, 1).is_err());
    let empty = json(r#"{"success":true,"data":{}}"#)?;
    let zero = json(r#"{"success":true,"data":{"setup":true,"quota_per_unit":0}}"#)?;
    assert!(project(&[], &empty, &zero, 1).is_err());
    Ok(())
}

#[test]
fn source_number_integer_accepts_decimal_json_notation() -> TestResult {
    let pricing = json(r#"{"success":true,"data":{"model_ratio":{},"model_price":{}}}"#)?;
    let status = json(r#"{"success":true,"data":{"setup":true,"quota_per_unit":500000.0}}"#)?;
    let snapshot = project(&[], &pricing, &status, 10)?;
    assert_eq!(snapshot.quota_per_credit, 500000);
    Ok(())
}

#[test]
fn metadata_and_projection_retain_exact_units_and_duplicate_key_last_value() -> TestResult {
    let mut bytes = b"\xef\xbb\xbf".to_vec();
    bytes.extend_from_slice(
        br#"{"success":true,"data":{"model_ratio":{"m-\ud800":2,"m-\udc00":3,"m-\ufffd":5,"duplicate":2,"duplicate":0},"model_price":{"m-\ud800":7},"completion_ratio":{"m-\udc00":2}}}"#,
    );
    let pricing = parse_metadata(&bytes)?;
    let high = ModelId::from_source(JsString16::from_units(vec![109, 45, 0xd800]));
    let low = ModelId::from_source(JsString16::from_units(vec![109, 45, 0xdc00]));
    let replacement = ModelId::from_scalar("m-\u{fffd}");
    let models = [
        ModelId::from_source(JsString16::from_units(vec![0xfeff, 109, 45, 0xd800, 0x20])),
        high.clone(),
        low.clone(),
        replacement.clone(),
        "duplicate".into(),
    ];
    let snapshot = project(&models, &pricing, &json(STATUS)?, 10)?;
    assert_eq!(
        snapshot.eligible_model_ids,
        vec![high.clone(), low.clone(), replacement.clone()]
    );
    assert_eq!(snapshot.model_quota_coefficients[&high], 7.0);
    assert_eq!(snapshot.model_quota_coefficients[&low], 9.0);
    assert_eq!(snapshot.model_quota_coefficients[&replacement], 10.0);
    assert_eq!(snapshot.model_quota_coefficients.len(), 3);
    Ok(())
}
