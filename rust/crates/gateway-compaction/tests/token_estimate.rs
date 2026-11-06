//! The calibrated estimator's frozen source vectors, through its public defining module.
use gateway_compaction::token_estimate::estimate_tokens;
use gateway_core::{source_json::SourceJson, source_serialization as json};

type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;
/// Converts a missing value or a failed result into the test's error instead of a panic.
trait Must<T> {
    fn must(self) -> TestResult<T>;
}
impl<T> Must<T> for Option<T> {
    fn must(self) -> TestResult<T> {
        self.ok_or_else(|| "missing value".into())
    }
}
impl<T, E: std::error::Error + 'static> Must<T> for Result<T, E> {
    fn must(self) -> TestResult<T> {
        self.map_err(Into::into)
    }
}

#[test]
fn source_utf16_estimator_matches_including_split_surrogates() -> TestResult {
    let fixture = SourceJson::parse(include_str!("fixtures/source-v1.json")).must()?;
    let cases = json::array(&json::member(&fixture, "tokens").must()?).must()?;
    assert!(!cases.is_empty());
    for case in cases {
        let text = json::string(&json::member(&case, "text").must()?).must()?;
        let expected = json::member(&case, "expected").must()?;
        assert_eq!(
            estimate_tokens(&text),
            expected.deserialize::<usize>().must()?
        );
    }
    Ok(())
}
