# Pure prompt policy

This crate ports `deepseek-forwarded-payload/v2`, `codex-compaction-summary/v1` and `chat-prompt-admission/v3` from `b59243d674567d418d1410f8390fc7fe47a08c92`. It performs no I/O, credential lookup, dispatch, admission-lifetime management, logging, retry or response encoding. Classifying a summary grants only the exact field exemption; aggregate admission measures the complete provider projection and never removes steer history.

The defining modules are `tokens` (one calibrated UTF16 estimator also used by local Jev compaction), `summary` (exact original Responses input classification and exemption data), `payload` (source field reduction and parseable argument elision) and `admission` (provider windows, full-body cost, reserve and observed report). A successful estimate is data, not proof that a request was dispatched or delivered. The caller applies the actual provider projection, records successful exemption metadata only after admission, and retains all transport and accounting authority.

The estimator was mechanically moved from `gateway-compaction/src/state.rs:15-51`, originally ported from `0x4007/fast-jev-compaction` commit `c1eab5fdbd6bde7d67f2da070116496558836884`; its MIT notice is retained in LICENSE. The frozen source estimator corpus remains in `gateway-compaction/tests/fixtures/source-v1.json`; integration tests consume it as data without depending on the compaction runtime.

Provider/schema/routing/Worker integration and actual workerd/client acceptance are separate from this initial pure-policy unit. In particular, this crate does not choose native SSE versus public HTTP error encoding, stop a waterfall or settle quota. No compaction family restructuring is included.
