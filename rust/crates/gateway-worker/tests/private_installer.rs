use gateway_worker::private_installer::decode_capture;
use serde_json::{Value, json};

type TestResult = Result<(), Box<dyn std::error::Error>>;

fn envelope() -> Result<Value, serde_json::Error> {
    let capture = json!({
        "v":2,"source_git_sha":"a".repeat(40),"captured_at_ms":1000,
        "source_codec":"bun-jsc-v1","native_schema_version":3,
        "data_revision":{"version":"7","sequence":"2"},
        "queue_entries":0,"running_queue_entries":0,"entries":[]
    });
    Ok(
        json!({"operation":"install_initial_control_owner","v":1,"capture":serde_json::to_vec(&capture)?}),
    )
}

#[test]
fn closed_private_envelope_accepts_capture_data_without_minting_owner_authority() -> TestResult {
    let capture = decode_capture(&serde_json::to_vec(&envelope()?)?)?;
    assert_eq!(capture.source_git_sha, "a".repeat(40));
    assert_eq!(capture.data_revision.version, "7");
    Ok(())
}

#[test]
fn owner_records_capabilities_and_other_operations_are_not_request_fields() -> TestResult {
    for forbidden in [
        "owner",
        "records",
        "capabilities",
        "namespace",
        "physical_custody_verified",
    ] {
        let mut input = envelope()?;
        input[forbidden] = json!(true);
        assert!(decode_capture(&serde_json::to_vec(&input)?).is_err());
    }
    let mut input = envelope()?;
    input["operation"] = json!("describe_target");
    assert!(decode_capture(&serde_json::to_vec(&input)?).is_err());
    input = envelope()?;
    input["v"] = json!(2);
    assert!(decode_capture(&serde_json::to_vec(&input)?).is_err());
    Ok(())
}

#[test]
fn malformed_or_oversized_private_capture_refuses_before_effects() -> TestResult {
    assert!(decode_capture(b"{}").is_err());
    let mut input = envelope()?;
    input["capture"] = json!([123]);
    assert!(decode_capture(&serde_json::to_vec(&input)?).is_err());
    let oversized = vec![0; gateway_migration::operational::PRIVATE_WIRE_MAX_BYTES + 1];
    assert!(decode_capture(&oversized).is_err());
    Ok(())
}
