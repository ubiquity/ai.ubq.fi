# Use byte operations at ASCII protocol call sites

Plan row 32. Target 35–65 net first-party lines. Risk 2. Rebase on the regex lane's result and recheck the retained string boundaries.

Own row 32's `src/rt/string.cpp:137–249,288–319,583–956,994–1013` and `include/uos/rt/string.hpp:1–18,23–32,60–95,108–123`, plus `tests/rt/string_test.cpp` cases touching the edited APIs. Pilot only the header-name checks in `src/codex/quota.cpp:43–54` and fixed message-type prefix in `src/responses_request.cpp:45–52`. The row 2 UTF region includes `fromCodePoint`, which has fixture coverage.

1. Use `std::string_view::starts_with`, `ends_with` and `find` where the needle is a fixed ASCII protocol token and no result is a UTF-16 index. Cover the same classification for arbitrary text around the token, including truncated or malformed UTF-8. Keep byte offsets only in already byte-defined domains.
2. Use native prefix/suffix tests for the verified ASCII and well-formed cases in the remaining shared helpers. Consolidate UTF-16 fallback operations using `std::u16string_view` on the existing converted units. Keep the validated fast paths for non-ASCII text in hot indexing, and measure them before removing any duplicate specialization. Recheck the uncalled `codePointAt` and `trimStart` surface and preserve its range, surrogate, whitespace and empty-input fixtures through the equivalent primitives.
3. Preserve user-text slicing, supplementary and lone-surrogate behavior, coercion where offsets are untyped, expansion limits and stable output. Keep ICU mapping and collation, percent escapes, UTF conversion, regex and provider streamed-text accounting under their existing owners.

```bash
for test in tests/rt/string_test.cpp tests/rt/text_codec_test.cpp tests/rt/regex_test.cpp \
  tests/codex_quota_differential_test.cpp tests/responses_conformance_http_test.cpp \
  tests/anthropic_chat_stream_differential_test.cpp; do
  bash cpp/gateway/tools/ci-build.sh --config release "$test" || exit 1
done
```

Accept when the pilot bypasses UTF-16 conversion on the checked call sites, the shared implementation shrinks and the UTF, regex, wire and fixed-token fixtures still pass. Record code size and any request-path timing change on ARM64. [String search](../../src/rt/string.cpp#L671) · [Unicode truncation](../../src/deepseek/chat_projection.cpp#L130) · [Driver](../CI.md)
