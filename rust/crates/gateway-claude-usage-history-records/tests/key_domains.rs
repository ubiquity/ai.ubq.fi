use gateway_claude_usage_history_records::{
    Bucket, claude_history_bucket_record_source, history_key, rollup_key_parts,
};
use gateway_core::source_control::encode_source_control_record;
use gateway_kv_codec::tuple_key::TupleKey;
use gateway_source::source_json::SourceJson;
use serde_json::json;
type TestResult = Result<(), Box<dyn std::error::Error>>;

#[test]
fn finite_float_factories_keep_old_json_bytes_and_reset_kind() -> TestResult {
    for bucket in [-0.0, 0.0, -1.25, 1.25, f64::MIN_POSITIVE, f64::MAX] {
        assert_eq!(
            history_key(bucket, "account\0é", "cohort")?,
            TupleKey::from_json_parts(&[
                json!("uos_ai"),
                json!("provider_capacity"),
                json!("claude"),
                json!("v1"),
                json!("history"),
                json!(bucket),
                json!("account\0é"),
                json!("cohort")
            ])?
            .to_record_key()
        );
        for reset in [None, Some(-0.0), Some(0.0), Some(1.25)] {
            let original = TupleKey::from_json_parts(&[
                json!("uos_ai"),
                json!("provider_capacity"),
                json!("claude"),
                json!("v1"),
                json!("rollup"),
                json!(bucket),
                json!("account"),
                json!("cohort"),
                json!("five_hour"),
                reset.map_or_else(|| json!("unknown"), |n| json!(n)),
            ])?
            .to_record_key();
            assert_eq!(
                rollup_key_parts(bucket, "account", "cohort", "five_hour", reset)?,
                original
            );
        }
    }
    assert_ne!(
        history_key(-0.0, "account", "cohort")?,
        history_key(0.0, "account", "cohort")?
    );
    assert_ne!(
        rollup_key_parts(0.0, "account", "cohort", "five_hour", None)?,
        rollup_key_parts(0.0, "account", "cohort", "five_hour", Some(0.0))?
    );
    Ok(())
}

#[test]
fn valid_stored_negative_zero_bucket_keeps_its_existing_custody_key() -> TestResult {
    let raw = SourceJson::parse(
        r#"{"v":1,"bucket_start_at_ms":-0.0,"account_id":"account","account_cohort_id":"cohort","points":[],"flags":{"normal":0,"unavailable":false,"recovery":false,"five_hour":false,"seven_day":false,"usage":null}}"#,
    )?;
    let bucket: Bucket = raw.deserialize()?;
    assert!(bucket.valid());
    let key = bucket.key()?;
    let row = encode_source_control_record("bucket-current", &raw, None)?;
    assert_eq!(claude_history_bucket_record_source(&key, &row)?, raw);
    assert!(
        claude_history_bucket_record_source(&history_key(0.0, "account", "cohort")?, &row).is_err()
    );
    Ok(())
}

#[test]
fn nonfinite_public_data_refuses_with_existing_family_errors() -> TestResult {
    for number in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
        for result in [
            history_key(number, "account", "cohort"),
            rollup_key_parts(number, "account", "cohort", "five_hour", None),
            rollup_key_parts(0.0, "account", "cohort", "five_hour", Some(number)),
        ] {
            let Err(error) = result else {
                return Err("invalid public DATA must refuse".into());
            };
            assert_eq!(error.status, 503);
            assert_eq!(
                error.envelope.error.code.as_deref(),
                Some("observability_state_invalid")
            );
        }
    }
    Ok(())
}
