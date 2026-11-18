# Replace general numeric helpers where callers have bounded types

Plan row 18. Target 85–120 net first-party lines. Risk 2. Rebase after the BigInt lane.

Own `parseFloat`, the two-argument `numberToString` and `toFixed` in `src/rt/number.cpp:560–564,606–696` and their declarations at `include/uos/rt/number.hpp:34,45–48`. Coordinate with the BigInt owner for the fractional radix private helper at `number.cpp:476–485`. Own the nibble operation only in `src/codex/dispatch.cpp:376–391` and the affected numeric fixtures.

1. Replace the Codex session UUID's radix formatter with a bounded integer calculation. The SHA-256 digest is lower-case hex. Decode its nibble at byte 16, keep its low two bits, set the RFC UUID variant bits to binary `10` and select the corresponding lower-case hex digit. Preserve the digest material, version nibble, slices and result bytes.
2. Search source and headers for the retired APIs and the private code used only by those paths. Remove the unused fractional-radix and fixed-decimal formatters and the prefix float parser where there are zero remaining uses. Keep radix digits/range constants still shared by BigInt. Retain JS coercion where it interprets untyped input and retain number-to-decimal formatting for JSON, IDs and persisted hashes. Use `std::from_chars` with full-consumption and range checks only for a proven typed grammar.
3. Carry forward the numeric fixture cases for remaining contracts, including decimal ties and signed zero, overflow, safe integer checks and BigInt vectors. Exercise the retired fixed/radix edge inputs at their actual supported typed boundaries where applicable. Keep exact session identity checks on the existing fixtures.

```bash
for test in tests/rt/number_test.cpp tests/rt/json_test.cpp \
  tests/rt/kv_test.cpp tests/codex_dispatch_differential_test.cpp \
  tests/api_key_policy_differential_test.cpp; do
  bash cpp/gateway/tools/ci-build.sh --config release "$test" || exit 1
done
```

Accept when the deterministic UUID, decimal JSON and key bytes stay identical, live inputs keep their range and coercion decisions, and runtime-only radix/fixed helper code is gone. [Numeric implementation](../../src/rt/number.cpp#L560) · [UUID call site](../../src/codex/dispatch.cpp#L376) · [Driver](../CI.md)
