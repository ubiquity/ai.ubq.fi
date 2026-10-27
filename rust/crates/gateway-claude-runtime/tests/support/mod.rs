pub(crate) type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;

pub(crate) fn required<T>(value: Option<T>) -> TestResult<T> {
    value.ok_or_else(|| "required fixture value missing".into())
}
