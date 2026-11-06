//! Pure Chat prompt admission and Codex context-failure encodings. No I/O, credentials or
//! dispatch: callers own transport, telemetry and the request lifecycle.
pub mod admission;
pub mod codex_context;
pub mod forwarding;
pub mod native_failure;
pub mod native_http_failure;
pub mod summary;
