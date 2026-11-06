use gateway_core::source_json::SourceJson;
use gateway_core::source_usage::{
    UsageStatus, extract_chat_usage_tokens, extract_source_usage_tokens, extract_usage_tokens,
};
use serde_json::{Value, json};

type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;

#[test]
fn frozen_bun_source_fixtures_share_one_engine() -> Result<(), Box<dyn std::error::Error>> {
    // Actual pinned Bun 1.4.0 source extractUsageTokens oracle, 2026-10-19.
    // Function/helper text copied verbatim from src/openai-telemetry.ts and utils.ts.
    let cases: Vec<Value> = serde_json::from_str(include_str!("../src/source_usage_cases.json"))?;
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
fn provenance_never_invents_present_cache_write_or_buyer_charge() -> TestResult {
    let usage = json!({"input_tokens":10,"output_tokens":2,"total_tokens":12,"input_tokens_details":{"cached_tokens":0}});
    let absent = extract_usage_tokens(Some(&usage)).ok_or("usage must be present")?;
    assert_eq!(absent.status, UsageStatus::Reported);
    assert!(!absent.cache_write_tokens_present());
    assert_eq!(absent.cache_write_input_tokens, None);
    assert_eq!(absent.buyer_cost_micro, None);
    let mut present = usage;
    present["input_tokens_details"]["cache_write_tokens"] = Value::Null;
    present["buyer_cost_micro"] = json!("invalid");
    let present = extract_usage_tokens(Some(&present)).ok_or("usage must be present")?;
    assert_eq!(present.status, UsageStatus::Invalid);
    assert!(present.cache_write_tokens_present());
    assert_eq!(present.cache_write_input_tokens, None);
    assert_eq!(present.buyer_cost_micro, Some(None));
    Ok(())
}
#[test]
fn invalid_buyer_charge_does_not_reclassify_reported_usage() -> TestResult {
    let usage = json!({"input_tokens":10,"output_tokens":2,"total_tokens":12,"input_tokens_details":{"cached_tokens":0},"buyer_cost_micro":-1});
    let actual = extract_usage_tokens(Some(&usage)).ok_or("usage must be present")?;
    assert_eq!(actual.status, UsageStatus::Reported);
    assert_eq!(actual.buyer_cost_micro, Some(None));
    assert_eq!(actual.status.as_str(), "reported");
    Ok(())
}

fn chat_usage(details: Option<Value>) -> Value {
    let mut usage = json!({"prompt_tokens":100,"completion_tokens":10,"total_tokens":110});
    if let Some(details) = details {
        usage["prompt_tokens_details"] = details;
    }
    usage
}

/// Mirrors tests/cache-usage-normalization.test.ts: Chat and Responses counters share one parser.
#[test]
fn chat_and_responses_usage_retain_identical_reported_partial_and_invalid_counters() -> TestResult {
    let cases = [
        None,
        Some(json!({"cached_tokens":0})),
        Some(json!({"cached_tokens":80})),
        Some(json!({})),
        Some(json!({"cached_tokens":null})),
        Some(json!({"cached_tokens":-1})),
        Some(json!({"cached_tokens":101})),
        Some(json!(null)),
        Some(json!([])),
    ];
    for details in cases {
        let mut responses = json!({"input_tokens":100,"output_tokens":10,"total_tokens":110});
        if let Some(details) = details.clone() {
            responses["input_tokens_details"] = details;
        }
        assert_eq!(
            extract_chat_usage_tokens(Some(&chat_usage(details.clone()))),
            extract_usage_tokens(Some(&responses)),
            "{details:?}"
        );
    }
    for cached in [-0.1, 0.9] {
        let usage =
            extract_chat_usage_tokens(Some(&chat_usage(Some(json!({"cached_tokens":cached})))))
                .ok_or("usage must be present")?;
        assert_eq!(usage.cached_input_tokens, None);
        assert_eq!(usage.status, UsageStatus::Invalid);
    }
    Ok(())
}

#[test]
fn chat_usage_preserves_measured_counters_when_another_field_is_missing_or_malformed() -> TestResult
{
    let partial = extract_chat_usage_tokens(Some(
        &json!({"prompt_tokens":100,"prompt_tokens_details":{"cached_tokens":80}}),
    ))
    .ok_or("usage must be present")?;
    assert_eq!(
        (
            partial.input_tokens,
            partial.cached_input_tokens,
            partial.output_tokens
        ),
        (Some(100), Some(80), None)
    );
    assert_eq!(
        (partial.total_tokens, partial.status),
        (None, UsageStatus::Partial)
    );
    for prompt in [json!(-0.1), json!(100.9), json!(9_007_199_254_740_992_u64)] {
        let mut usage = chat_usage(Some(json!({"cached_tokens":80})));
        usage["prompt_tokens"] = prompt;
        let usage = extract_chat_usage_tokens(Some(&usage)).ok_or("usage must be present")?;
        assert_eq!(
            (usage.input_tokens, usage.cached_input_tokens),
            (None, Some(80))
        );
        assert_eq!(
            (usage.output_tokens, usage.total_tokens),
            (Some(10), Some(110))
        );
        assert_eq!(usage.status, UsageStatus::Invalid);
    }
    Ok(())
}

#[test]
fn chat_usage_absence_stays_unknown_and_only_chat_fields_are_read() -> TestResult {
    assert_eq!(extract_chat_usage_tokens(None), None);
    assert_eq!(extract_chat_usage_tokens(Some(&Value::Null)), None);
    assert_eq!(extract_chat_usage_tokens(Some(&json!([]))), None);
    // A Responses-shaped object on the Chat route carries no Chat counter.
    let foreign = extract_chat_usage_tokens(Some(&json!({"input_tokens":1})))
        .ok_or("usage must be present")?;
    assert_eq!(
        (foreign.input_tokens, foreign.status),
        (None, UsageStatus::Partial)
    );
    let deepseek = json!({"prompt_tokens":11,"completion_tokens":7,"total_tokens":18,"prompt_tokens_details":{"cached_tokens":0},"prompt_cache_hit_tokens":0,"buyer_cost_micro":5});
    let usage = extract_chat_usage_tokens(Some(&deepseek)).ok_or("usage must be present")?;
    assert_eq!(
        (usage.input_tokens, usage.output_tokens),
        (Some(11), Some(7))
    );
    assert_eq!(
        (usage.cached_input_tokens, usage.status),
        (Some(0), UsageStatus::Reported)
    );
    assert_eq!(usage.buyer_cost_micro, Some(Some(5)));
    Ok(())
}
