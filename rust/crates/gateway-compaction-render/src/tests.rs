use crate::{hybrid, transcript::truncated_result};
use gateway_compaction_source::{state, text::Text};
use gateway_compaction_test_support::{cases, decode, equal, field};
#[test]
fn source_utf16_estimator_and_bounded_heads_match_including_split_surrogates() {
    for case in cases("tokens") {
        let text: Text = decode(&field(&case, "text"));
        assert_eq!(
            gateway_prompt_policy::tokens::estimate_tokens(text.as_units()),
            decode::<usize>(&field(&case, "expected"))
        );
    }
    for case in cases("truncation") {
        let value: Text = decode(&field(&case, "text"));
        let expected: Text = decode(&field(&case, "expected"));
        assert_eq!(
            truncated_result(
                &value,
                decode(&field(&case, "error")),
                decode(&field(&case, "head"))
            ),
            expected
        );
    }
}

#[test]
fn source_identifier_order_counts_latest_tail_and_block_chunks_match() {
    for case in cases("ledgers") {
        let value: Text = decode(&field(&case, "text"));
        equal(
            &state::json(&hybrid::harvest(&value, 300)).unwrap(),
            &field(&case, "expected"),
        );
        let composed: Text = decode(&field(&case, "composed"));
        assert_eq!(
            hybrid::compose(&Text::from("short memory"), &value),
            composed
        );
        let chunks: Vec<Text> = decode(&field(&case, "chunks"));
        assert_eq!(hybrid::chunks(&value, 30), chunks);
    }
}
