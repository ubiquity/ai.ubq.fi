use crate::{state, transcript};
use gateway_compaction_source::{
    model::Options,
    state::json,
    text::{self, Text},
};
use gateway_compaction_test_support::{
    Recipe, cases, decode, equal, field, recipe, state_supplemental_fixture, string,
};
#[test]
fn source_codex_items_and_opaque_redaction_are_exact() {
    for case in cases("parser") {
        let raw: Text = decode(&field(&case, "raw"));
        let input =
            text::parse_text(&raw).unwrap_or_else(|| panic!("missing source fixture value"));
        let actual = transcript::parse_input(&input)
            .unwrap_or_else(|error| panic!("source fixture failure: {error:?}"));
        equal(
            &json(&actual).unwrap_or_else(|error| panic!("source fixture failure: {error:?}")),
            &field(&case, "expected"),
        );
    }
}

#[test]
fn source_state_fitting_ladder_preserves_exact_stages_and_budget_counts() {
    let supplemental = state_supplemental_fixture();
    for case in cases("states").into_iter().chain(
        text::array(&field(&supplemental, "cases"))
            .unwrap_or_else(|| panic!("missing source fixture value")),
    ) {
        let r: Recipe = decode(&field(&case, "recipe"));
        let messages = recipe(&r);
        let calls = state::collect_calls(&messages, 2);
        let opts = Options {
            max_state_tokens: Some(decode::<f64>(&field(&case, "budget"))),
            preserve_recent_messages: Some(2.0),
            ..Options::default()
        }
        .resolve();
        let expected = field(&case, "expected");
        let result = state::fit_state(&messages, &calls, &opts);
        if string(&field(&expected, "kind")) == "failure" {
            assert!(result.is_err());
        } else {
            equal(
                &json(&result.unwrap_or_else(|error| panic!("source fixture failure: {error:?}")))
                    .unwrap_or_else(|error| panic!("source fixture failure: {error:?}")),
                &field(&expected, "fitted"),
            );
        }
    }
}
