use gateway_kv_codec::{
    key::RecordKey,
    tuple_key::{KeyPart, ScalarKeyPart, TupleKey},
};
use gateway_source::js_string::JsString16;
use serde_json::{Value, json};
type TestResult = Result<(), Box<dyn std::error::Error>>;

#[test]
fn typed_scalar_parts_keep_source_bytes_and_integer_rounding() -> TestResult {
    for value in [
        i64::MIN,
        -2,
        0,
        2,
        9_007_199_254_740_991,
        9_007_199_254_740_992,
        9_007_199_254_740_993,
        9_007_199_254_740_994,
        i64::MAX,
    ] {
        let typed = TupleKey::from_scalar_parts(&[
            ScalarKeyPart::String("é\0雪"),
            ScalarKeyPart::Number(value),
            ScalarKeyPart::Boolean(true),
        ]);
        let original = TupleKey::from_json_parts(&[json!("é\0雪"), json!(value), json!(true)])?;
        assert_eq!(typed.sort_bytes(), original.sort_bytes());
        assert_eq!(typed.to_record_key(), original.to_record_key());
    }
    for (value, hex) in [
        (9_007_199_254_740_991_i64, "21c33fffffffffffff"),
        (9_007_199_254_740_992, "21c340000000000000"),
        (9_007_199_254_740_993, "21c340000000000000"),
        (9_007_199_254_740_994, "21c340000000000001"),
        (i64::MAX, "21c3e0000000000000"),
        (i64::MIN, "213c1fffffffffffff"),
    ] {
        assert_eq!(
            TupleKey::from_scalar_parts(&[ScalarKeyPart::Number(value)])
                .to_record_key()
                .0,
            format!("tuple-v1:{hex}")
        );
    }
    assert_eq!(
        TupleKey::from_scalar_parts(&[ScalarKeyPart::String("é\0雪")])
            .to_record_key()
            .0,
        "tuple-v1:02c3a900ffe99baa00"
    );
    assert_eq!(
        TupleKey::from_scalar_parts(&[ScalarKeyPart::Boolean(false), ScalarKeyPart::Boolean(true)])
            .to_record_key()
            .0,
        "tuple-v1:2627"
    );
    assert!(
        TupleKey::from_scalar_parts(&[ScalarKeyPart::Number(2)])
            < TupleKey::from_scalar_parts(&[ScalarKeyPart::Number(10)])
    );
    assert!(
        TupleKey::from_scalar_parts(&[])
            .validate_stored_key()
            .is_err()
    );
    Ok(())
}

#[test]
fn unsigned_and_usize_stay_at_the_existing_checked_json_number_boundary() -> TestResult {
    for value in [
        0_u64,
        9_007_199_254_740_993,
        9_223_372_036_854_775_808,
        u64::MAX,
    ] {
        let json = json!(value);
        let number = json.as_f64().ok_or("finite source JSON number")?;
        let typed = TupleKey::from_parts(&[KeyPart::Number(number)])?;
        assert_eq!(typed, TupleKey::from_json_parts(&[json])?);
    }
    assert_eq!(
        TupleKey::from_json_parts(&[json!(u64::MAX)])?
            .to_record_key()
            .0,
        "tuple-v1:21c3f0000000000000"
    );
    for value in [0_usize, 1, usize::MAX] {
        let json = json!(value);
        let number = json.as_f64().ok_or("finite source usize number")?;
        assert_eq!(
            TupleKey::from_parts(&[KeyPart::Number(number)])?,
            TupleKey::from_json_parts(&[json])?
        );
    }
    Ok(())
}

#[test]
fn source_utf16_replacement_is_only_the_key_boundary_and_raw_identity_stays_exact() -> TestResult {
    let high = JsString16::from_units(vec![0xd800]);
    let low = JsString16::from_units(vec![0xdfff]);
    assert_ne!(high, low);
    for units in [high.as_units(), low.as_units()] {
        assert_eq!(
            TupleKey::from_parts(&[KeyPart::source_string_utf16(units)])?
                .to_record_key()
                .0,
            "tuple-v1:02efbfbd00"
        );
    }
    assert_eq!(
        TupleKey::from_parts(&[KeyPart::source_string_utf16(&[0xd800, 0xdc00])])?
            .to_record_key()
            .0,
        "tuple-v1:02f090808000"
    );
    assert_eq!(
        TupleKey::from_scalar_parts(&[ScalarKeyPart::String(r"\ud800")])
            .to_record_key()
            .0,
        "tuple-v1:025c756438303000"
    );
    for raw in ["tuple-v1:21fff8000000000001", "tuple-v1:02fffe00"] {
        let raw = RecordKey(raw.into());
        assert_eq!(TupleKey::from_record_key(&raw)?.to_record_key(), raw);
    }
    Ok(())
}

#[test]
fn checked_dynamic_parts_refuse_invalid_json_kinds_without_default_keys() -> TestResult {
    for value in [Value::Null, json!({}), json!([])] {
        assert!(TupleKey::from_json_parts(&[value]).is_err());
    }
    for value in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
        assert!(serde_json::Number::from_f64(value).is_none());
        assert!(TupleKey::from_json_parts(&[json!(value)]).is_err());
    }
    Ok(())
}
