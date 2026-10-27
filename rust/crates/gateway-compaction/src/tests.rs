use crate::{build_compaction_response, engine, hybrid, is_responses_compaction};
use gateway_compaction_history::state as history;
use gateway_compaction_render::hybrid as projection;
use gateway_compaction_source::{
    model::Options,
    state,
    text::{self, Text},
    types::{FailureKind, ResponseOptions},
};
use gateway_compaction_test_support::{
    Host, PendingHost, Recipe, cases, decode, equal, field, fitting_fixture, fitting_input, recipe,
    run, string,
};
use gateway_core::source_json::SourceJson;
#[test]
fn exact_header_predicate_matches_source() {
    for case in cases("metadata") {
        let raw: Option<String> = decode(&field(&case, "raw"));
        assert_eq!(
            is_responses_compaction(raw.as_deref()),
            decode::<bool>(&field(&case, "expected"))
        );
    }
}

#[test]
fn source_local_response_and_all_candidate_failures_match_without_usage_or_partial_output() {
    for case in cases("bodies") {
        let raw: Text = decode(&field(&case, "raw"));
        let body = text::parse_text(&raw).unwrap();
        let original = body.as_str().to_owned();
        let mut host = Host::new(&string(&field(&case, "mode")));
        host.residue = string(&field(&case, "residue"));
        let stream = decode(&field(&case, "stream"));
        let hybrid = decode(&field(&case, "hybrid"));
        let expected = field(&case, "expected");
        let result = run(build_compaction_response(
            &body,
            &host,
            ResponseOptions { stream, hybrid },
        ));
        if string(&field(&expected, "kind")) == "failure" {
            assert_eq!(
                result.unwrap_err().kind.label(),
                string(&field(&expected, "failure"))
            );
        } else {
            let result = result.unwrap();
            let actual=SourceJson::from_serializable(&serde_json::json!({"status":result.status,"contentType":result.content_type,"headers":result.headers,"body":String::from_utf8(result.body).unwrap()})).unwrap();
            equal(&actual, &field(&expected, "outcome"));
        }
        assert_eq!(body.as_str(), original);
        equal(
            &state::json(&*host.asks.borrow()).unwrap(),
            &field(&case, "asks"),
        );
    }
}

#[test]
fn source_windows_batching_pin_rules_and_projected_transcript_match() {
    for case in cases("engines") {
        let r: Recipe = decode(&field(&case, "recipe"));
        let messages = recipe(&r);
        let options: Options = decode(&field(&case, "options"));
        let host = Host::new("drop");
        let result = run(engine::compact(&messages, &host, &options))
            .unwrap_or_else(|error| panic!("source fixture failure: {error:?}"));
        equal(
            &state::json(&result)
                .unwrap_or_else(|error| panic!("source fixture failure: {error:?}")),
            &field(&case, "result"),
        );
        let calls = history::collect_calls(&messages, 2);
        let windows = engine::plan_windows(&messages, &calls, 80_000)
            .into_iter()
            .map(|(start, end)| serde_json::json!({"start":start,"end":end}))
            .collect::<Vec<_>>();
        equal(
            &state::json(&windows)
                .unwrap_or_else(|error| panic!("source fixture failure: {error:?}")),
            &field(&case, "windows"),
        );
    }
}

#[test]
fn source_residue_full_partial_empty_and_all_failure_paths_keep_the_right_chunks() {
    for case in cases("residues") {
        let pure: Text = decode(&field(&case, "pure"));
        let mut host = Host::new("drop");
        host.residue = string(&field(&case, "mode"));
        host.oversized = pure.len() + 1000;
        let result = run(hybrid::run(&pure, &host, 400, 1000));
        let expected: Option<Text> = decode(&field(&case, "expected"));
        assert_eq!(result, expected);
        equal(
            &state::json(&*host.residue_calls.borrow()).unwrap(),
            &field(&case, "requests"),
        );
    }
}

#[test]
fn cancellation_cannot_return_success_or_hide_in_hybrid_fallback() {
    let case = cases("bodies").remove(0);
    let body = text::parse_text(&decode::<Text>(&field(&case, "raw"))).unwrap();
    let host = Host::new("drop");
    host.cancel.set(true);
    assert_eq!(
        run(build_compaction_response(
            &body,
            &host,
            ResponseOptions {
                stream: false,
                hybrid: false
            }
        ))
        .unwrap_err()
        .kind,
        FailureKind::Cancelled
    );
    assert!(host.asks.borrow().is_empty());
    let mut host = Host::new("drop");
    host.cancel_after_ask = true;
    assert_eq!(
        run(build_compaction_response(
            &body,
            &host,
            ResponseOptions {
                stream: false,
                hybrid: false
            }
        ))
        .unwrap_err()
        .kind,
        FailureKind::Cancelled
    );
    assert_eq!(host.asks.borrow().len(), 1);
}

#[test]
fn source_large_summary_fitting_floor_refusal_and_fit_before_reduction_match_exactly() {
    let corpus = fitting_fixture();
    let pattern = string(&field(&corpus, "pattern"));
    let prompt = string(&field(&corpus, "prompt"));
    for case in text::array(&field(&corpus, "cases"))
        .unwrap_or_else(|| panic!("missing source fixture value"))
    {
        let recipe = field(&case, "recipe");
        let name = string(&field(&recipe, "name"));
        let body = fitting_input(&recipe, &pattern, &prompt);
        let host = Host::new(&string(&field(&recipe, "mode")));
        let result = run(build_compaction_response(
            &body,
            &host,
            ResponseOptions {
                stream: false,
                hybrid: false,
            },
        ));
        assert_eq!(
            host.asks.borrow().len(),
            decode::<usize>(&field(&case, "asks")),
            "{name}"
        );
        let expected = field(&case, "expected");
        if string(&field(&expected, "kind")) == "failure" {
            assert_eq!(
                result
                    .err()
                    .unwrap_or_else(|| panic!("expected source failure"))
                    .kind
                    .label(),
                string(&field(&expected, "failure")),
                "{name}"
            );
            continue;
        }
        let result = result.unwrap_or_else(|error| panic!("source fixture failure: {error:?}"));
        assert_eq!(
            result.status,
            decode::<u16>(&field(&expected, "status")),
            "{name}"
        );
        assert_eq!(
            result.content_type,
            string(&field(&expected, "contentType")),
            "{name}"
        );
        equal(
            &state::json(&result.headers)
                .unwrap_or_else(|error| panic!("source fixture failure: {error:?}")),
            &field(&expected, "headers"),
        );
        assert_eq!(
            result.input_estimate_tokens,
            decode::<usize>(&field(&case, "inputTokens")),
            "{name}"
        );
        let mut summary = String::new();
        for part in text::array(&field(&expected, "summary"))
            .unwrap_or_else(|| panic!("missing source fixture value"))
        {
            if let Some(value) = text::string(&part) {
                summary.push_str(
                    &value
                        .try_utf8()
                        .unwrap_or_else(|error| panic!("source fixture failure: {error:?}")),
                );
            } else {
                let (value, count): (String, usize) = decode(&part);
                summary.push_str(&value.repeat(count));
            }
        }
        let units = summary.encode_utf16().collect::<Vec<_>>();
        assert_eq!(
            units.len(),
            decode::<usize>(&field(&expected, "chars")),
            "{name}"
        );
        assert!(
            result.summary.as_units() == units,
            "{name}: exact source summary differs"
        );
    }
}

#[test]
fn source_window_and_residue_worker_bounds_hold_across_pending_host_io() {
    let messages = recipe(&Recipe {
        count: 800,
        text: 0,
        input: 0,
        notes: false,
    });
    let host = PendingHost::new();
    let options = Options {
        preserve_recent_messages: Some(2.0),
        max_state_tokens: Some(800.0),
        max_request_tokens: Some(4000.0),
        ..Options::default()
    };
    let result = run(engine::compact(&messages, &host, &options))
        .unwrap_or_else(|error| panic!("source fixture failure: {error:?}"));
    assert!(result.stats.windows > 3);
    assert_eq!(host.peak.get(), 3);
    assert_eq!(host.active.get(), 0);
    let pure: Text = decode(&field(&cases("residues").remove(0), "pure"));
    let host = PendingHost::new();
    assert!(run(hybrid::run(&pure, &host, 400, 1000)).is_some());
    assert_eq!(host.peak.get(), 4);
    assert_eq!(host.active.get(), 0);
    assert_eq!(
        host.base.residue_calls.borrow().len(),
        projection::chunks(&pure, 400).len()
    );
}

#[test]
fn cancellation_during_residue_cannot_be_hidden_by_best_effort_fallback() {
    let case = cases("bodies").remove(0);
    let body = text::parse_text(&decode::<Text>(&field(&case, "raw"))).unwrap();
    let mut host = Host::new("drop");
    host.residue = "success".into();
    host.cancel_after_residue = true;
    assert_eq!(
        run(build_compaction_response(
            &body,
            &host,
            ResponseOptions {
                stream: false,
                hybrid: true
            }
        ))
        .unwrap_err()
        .kind,
        FailureKind::Cancelled
    );
    assert_eq!(host.residue_calls.borrow().len(), 1);
}
