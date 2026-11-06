//! Test-only fallible conversions, so fixtures report a failed value instead of panicking.
pub type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;
/// Converts a missing value or a failed result into the test's error instead of a panic.
pub trait Must<T> {
    fn must(self) -> TestResult<T>;
}
impl<T> Must<T> for Option<T> {
    fn must(self) -> TestResult<T> {
        self.ok_or_else(|| "missing value".into())
    }
}
impl<T, E: std::fmt::Debug> Must<T> for Result<T, E> {
    fn must(self) -> TestResult<T> {
        self.map_err(|error| format!("{error:?}").into())
    }
}
