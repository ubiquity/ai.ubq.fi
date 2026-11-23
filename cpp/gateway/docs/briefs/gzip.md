# Simplify whole-buffer gzip through the existing zlib adapter

Plan row 27. Target 45–75 net first-party lines. Risk 3. Keep the shared streaming inflater and Fetch content-decoding policy.

Own the Blob chunk constants and whole-buffer functions in `src/rt/gzip.cpp:74–75,175–211,384–450`, `include/uos/rt/gzip.hpp:126–149`, the local compression helpers at `src/catalog/store.cpp:139–149` and `src/sentinel/replay_envelope.cpp:421–448`, and `tests/rt/gzip_test.cpp` cases for the buffer adapter.

1. Feed bounded, fixed-size slices into the existing zlib inflater and drain each slice, including the final status. Remove the two-size Blob schedule. Use one scrubbed output accumulator for full-buffer results and the Sentinel capped reader. Enforce the Sentinel limit before each append, and keep buffer replacement, failure and input cleanup on every path. Preserve zlib stream addresses and application bounds on bytes handed to zlib.
2. Collapse wrapper coroutines that only await another adapter and return its value. Keep an observable scheduled completion where tests or actual catalog/Sentinel ordering depend on it. Evaluate both successful and failed work. Never put an owning temporary inside a `co_await` expression.
3. Read the frozen compressed inputs and compare plaintext, failure classification and bounded callbacks. Preserve accepted gzip members and trailing bytes, checksum and truncation failures, plaintext zeroization, existing stored catalog integrity and authenticated Sentinel replay. The new buffer adapter may change internal piece lengths. Exercise the original partition boundaries and verify the joined bytes and callback budget.

```bash
for test in tests/rt/gzip_test.cpp tests/rt/fetch_transport_test.cpp \
  tests/catalog_store_differential_test.cpp tests/sentinel_replay_envelope_coverage_test.cpp \
  tests/sentinel_replay_differential_test.cpp tests/sentinel_replay_limits_test.cpp; do
  bash cpp/gateway/tools/ci-build.sh --config release "$test" || exit 1
done
```

Accept when Blob-sized segmentation is gone, there is one bounded scrubbed accumulation path, and previous gzip, Fetch and stored replay fixtures pass with the same safety limits. [Gzip adapter](../../src/rt/gzip.cpp#L384) · [Sentinel cap](../../src/sentinel/replay_envelope.cpp#L421) · [Driver](../CI.md)
