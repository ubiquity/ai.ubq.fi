# Replace the console inspection printer

Plan row 54. Target 280–420 net first-party lines. Risk 2. Rebase onto the active console helper lane before taking ownership. [Audit](../findings/cppgw-keep-audit-20261118.md)

Own `src/rt/console.cpp:15–408,440–552`, the `Value`, `ErrorInfo` and formatting declarations in `include/uos/rt/console.hpp:35–123`, and `tests/rt/console_test.cpp`. Keep the existing observer and sink API as the integration seam. Read `src/handler/terminal_log.cpp:124–132`, `src/serve.cpp:32–38,147–150` and the capture consumer in `src/lib/jev_compaction/jev_compaction_smoke.cpp:324–334`.

1. Inventory the argument shapes at `rt::console::{log,info,warn,error,debug}` and `rt::console::Value` references. Verify the full calls, including multiline arguments and variables used as prefixes. Keep the string, structured fields and error argument shapes used there.
2. Use owned C++ strings and Boost.JSON values for text and fields. Serialize JSON values at the sink with the existing safe JSON serializer when they can contain nonfinite numbers or WTF-8. Pass pre-serialized records through as literal text. Keep a small typed error projection with the diagnostic name, message, code, causes and existing OS fields. Remove the duplicate JSON quote/stringify implementation, JS coercion, percent substitution and nested inspection layout. Preserve source-compatible capture accessors used by callers.
3. Preserve stdout for log/info/debug and stderr for warn/error, per-call order and synchronous observation. Retain nested capture, passthrough, stable args during reentrancy, propagated observer exceptions and event prefix/payload pairs. Keep the same redaction boundaries and fields. Keep the original format fixture inputs, verifying their literal or structured output under the new rendering contract. Preserve the existing capture, callback and fd routing assertions.

Run from the repository root using the [isolated driver](../CI.md).

```bash
for test in tests/rt/console_test.cpp tests/serve_test.cpp \
  tests/jev_compaction_smoke_test.cpp tests/serve_delivery_http_test.cpp \
  tests/handler_terminal_admission_stage_test.cpp; do
  bash cpp/gateway/tools/ci-build.sh --config release "$test" || exit 1
done
```

Accept when all selected cases pass, the smoke can still identify its own terminal events from captured arguments, and there is one path from structured diagnostic values to each sink. Count the net deletions including fixture updates.
