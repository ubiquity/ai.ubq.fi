use super::*;
use serde_json::json;

#[test]
fn frozen_bun_source_fixtures_share_one_engine() -> Result<(), Box<dyn std::error::Error>> {
    // Actual pinned Bun 1.4.0 source extractUsageTokens oracle, 2026-10-19.
    // Function/helper text copied verbatim from src/openai-telemetry.ts and utils.ts.
    let cases: Vec<Value> = serde_json::from_str(include_str!("../source_usage_cases.json"))?;
    assert_eq!(cases.len(), 34);
    for case in cases {
        let name = case["name"].as_str().ok_or("source fixture name missing")?;
        let source = case["raw"].as_str().map(SourceJson::parse).transpose()?;
        let actual = extract_source_usage_tokens(source.as_ref())?;
        assert_eq!(serde_json::to_value(&actual)?, case["expected"], "{name}");
        if let Some(source) = source {
            if let Ok(value) = source.deserialize::<Value>() {
                let value_usage = extract_usage_tokens(Some(&value));
                assert_eq!(value_usage, actual, "{name}: value frontend");
            }
        } else {
            assert_eq!(extract_usage_tokens(None), None);
        }
    }
    Ok(())
}
#[test]
fn provenance_never_invents_present_cache_write_or_buyer_charge()
-> Result<(), Box<dyn std::error::Error>> {
    let usage = json!({"input_tokens":10,"output_tokens":2,"total_tokens":12,"input_tokens_details":{"cached_tokens":0}});
    let absent = extract_usage_tokens(Some(&usage)).ok_or("missing reported usage fixture")?;
    assert_eq!(absent.status, UsageStatus::Reported);
    assert!(!absent.cache_write_tokens_present());
    assert_eq!(absent.cache_write_input_tokens, None);
    assert_eq!(absent.buyer_cost_micro, None);
    let mut present = usage;
    present["input_tokens_details"]["cache_write_tokens"] = Value::Null;
    present["buyer_cost_micro"] = json!("invalid");
    let present = extract_usage_tokens(Some(&present)).ok_or("missing invalid usage fixture")?;
    assert_eq!(present.status, UsageStatus::Invalid);
    assert!(present.cache_write_tokens_present());
    assert_eq!(present.cache_write_input_tokens, None);
    assert_eq!(present.buyer_cost_micro, Some(None));
    Ok(())
}
#[test]
fn invalid_buyer_charge_does_not_reclassify_reported_usage()
-> Result<(), Box<dyn std::error::Error>> {
    let usage = json!({"input_tokens":10,"output_tokens":2,"total_tokens":12,"input_tokens_details":{"cached_tokens":0},"buyer_cost_micro":-1});
    let actual = extract_usage_tokens(Some(&usage)).ok_or("missing reported buyer-cost fixture")?;
    assert_eq!(actual.status, UsageStatus::Reported);
    assert_eq!(actual.buyer_cost_micro, Some(None));
    assert_eq!(actual.status.as_str(), "reported");
    Ok(())
}
