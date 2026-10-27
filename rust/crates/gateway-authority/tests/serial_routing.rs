use gateway_routing_runtime_test_support::authority as support;
mod routing_source_cases;

type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;
