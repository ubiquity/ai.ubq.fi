//! Fixed source-contract cases for the newly defining finite boundary.
use gateway_capacity_projection::{
    math::merge_remote_history,
    scalars::{DAY, is_record, js_whitespace, safe_i64, trim},
};
use gateway_source::error::GatewayError;
use serde_json::{Value, json};
use std::cell::Cell;

#[test]
fn scalar_records_keep_arrays_and_reject_primitives() {
    assert!(is_record(&json!({})));
    assert!(is_record(&json!([])));
    for value in [Value::Null, json!(false), json!(0), json!("record")] {
        assert!(!is_record(&value));
    }
    assert_eq!(DAY, 86_400_000);
}

#[test]
fn safe_integer_boundary_retains_source_number_rules() {
    for (value, expected) in [
        (json!(0), Some(0)),
        (json!(-0.0), Some(0)),
        (json!(42.0), Some(42)),
        (
            json!(9_007_199_254_740_991_u64),
            Some(9_007_199_254_740_991),
        ),
        (json!(9_007_199_254_740_992_u64), None),
        (json!(-1), None),
        (json!(0.5), None),
        (json!("42"), None),
        (json!(true), None),
        (Value::Null, None),
    ] {
        assert_eq!(safe_i64(&value), expected, "{value}");
    }
}

#[test]
fn whitespace_matches_javascript_and_trim_borrows_the_original_text() {
    let whitespace = "\u{0009}\u{000a}\u{000b}\u{000c}\u{000d} \u{00a0}\u{1680}\u{2000}\u{2001}\u{2002}\u{2003}\u{2004}\u{2005}\u{2006}\u{2007}\u{2008}\u{2009}\u{200a}\u{2028}\u{2029}\u{202f}\u{205f}\u{3000}\u{feff}";
    assert!(whitespace.chars().all(js_whitespace));
    assert_eq!(trim(whitespace), "");
    for character in ['\u{0085}', '\u{180e}', '\u{200b}', 'x'] {
        assert!(!js_whitespace(character));
        let text = format!("{character}payload{character}");
        assert_eq!(trim(&text), text);
    }
    let text = format!("{whitespace}payload{whitespace}");
    let expected = &text[whitespace.len()..whitespace.len() + "payload".len()];
    assert_eq!(trim(&text), "payload");
    assert!(std::ptr::eq(trim(&text), expected));
}

fn point(bucket: i64, sampled: i64) -> Value {
    json!({
        "bucket_start_at_ms": bucket,
        "sampled_at_ms": sampled,
        "sources": [
            {"source":"codex", "slot":1, "state":"available", "account_cohort_id":"cohort"},
            {"source":"codex", "slot":2, "failure_kind":"not_configured"}
        ]
    })
}

#[test]
fn remote_history_propagates_the_exact_injected_comparator_error() {
    let local = [point(0, 10)];
    let remote = [
        ("zeta".to_owned(), point(900_000, 900_010)),
        ("alpha".to_owned(), point(900_000, 900_020)),
    ];
    let error = GatewayError::new(502, "fixed comparator failure", Some("label_comparison"))
        .with_type("server_error")
        .with_param(Some("label"));
    let calls = Cell::new(0);
    let result = merge_remote_history(&local, &remote, |_, _| {
        calls.set(calls.get() + 1);
        Err(error.clone())
    });
    assert!(calls.get() > 0);
    assert_eq!(result, Err(error));
}
