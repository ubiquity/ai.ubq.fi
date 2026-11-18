# Use chrono for UTC calendar operations

Plan row 35. Target 90–145 net first-party lines. Risk 3. Rebase after the date helper and passkey ASN.1 lanes. Keep clock and timer ownership in row 39.

Own `src/rt/date.cpp:13–119,616–753`, `include/uos/rt/date.hpp:29–66` and the corresponding cases in `tests/rt/date_test.cpp`. The existing parser starts at line 121. Keep its grammar and provider validity decisions through the first patch.

1. Implement ordinary UTC extraction and normalized date arithmetic with C++20 `sys_days`, `year_month_day`, `weekday` and `hh_mm_ss`. Normalize components before constructing the date. Check year and day bounds before converting into chrono types. Keep the wide arithmetic fallback for TimeClip values and overflow-normalized inputs that exceed chrono's calendar range. Preserve UTC truncation, midnight before epoch, leap dates and invalid outputs.
2. Use existing width and literal-formatting helpers for canonical ISO timestamps and RFC HTTP dates. Test epoch zero, negative epoch milliseconds, year 0–99 handling, signed extended years, UTC offsets, TimeClip endpoints and the weekday. Retain the parse round-trip for provider Retry-After decisions and check its status and effective reset time.
3. Recheck where the UTC setters are reached. If they remain fixture-only, replace their call chains with immutable UTC operations and exercise the same components, normalization, invalid values and resulting milliseconds through the new entrypoints. Keep filesystem timestamp and stored-data comparisons.

```bash
for test in tests/rt/date_test.cpp tests/rt/fs_test.cpp tests/rt/clock_test.cpp \
  tests/native_codex_429_differential_test.cpp tests/native_codex429_conformance_test.cpp \
  tests/cache_kv_migration_usage_coverage_test.cpp tests/serve_delivery_http_test.cpp; do
  bash cpp/gateway/tools/ci-build.sh --config release "$test" || exit 1
done
```

Accept when the ordinary calendar path delegates to chrono, no mutating fixture-only date APIs remain in the runtime, and the accepted health/header, provider-quota and persisted timestamps remain the same. [Date implementation](../../src/rt/date.cpp#L13) · [Retry-After check](../../src/authority/native_codex_429.cpp#L57) · [Driver](../CI.md)
