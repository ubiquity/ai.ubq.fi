# Store each validated header once

Plan row 28. Target 55–95 net first-party lines. Risk 3. The active Headers lane and this brief need one owner. Recheck the parser and writer callers against the rebased tree.

Own `src/rt/headers.cpp:13–229`, `include/uos/rt/headers.hpp:19–102` and `tests/rt/headers_test.cpp`. Keep the public `Headers` API and `detail::headerWireFields` while parser and transport changes integrate. The Fetch wire writer consumes the first spelling of each field at `src/rt/fetch_transport_http.cpp:179`.

1. Store lowercased name, first spelling and combined value together in one sorted `std::vector` record. Use a single projected lookup for get/has/append/set/delete and a single combined Cookie value. Preserve the separate ordered Set-Cookie vector and the wire projection's first spelling until the field is deleted.
2. Remove the common-name spellings used only to render invalid-value diagnostics. Keep ASCII token validation, trim and ByteString checks before modifying the collection. Preserve every reject/accept outcome, with fresh assertions for rejection classification if a legacy diagnostic string changes.
3. Preserve Cookie joining with `; `, generic combination with `, `, each Set-Cookie line, and case-insensitive lookup. Keep copy independence, current mutation-visible traversal, and explicit suppression/override semantics at the Fetch and server boundaries. HTTP framing, credential stripping and WebSocket handshake filtering remain owned by their existing lanes.

```bash
for test in tests/rt/headers_test.cpp tests/rt/fetch_transport_test.cpp \
  tests/runtime_http_sse_headers_test.cpp tests/responses_native_headers_test.cpp \
  tests/cluster_w04c1_headers_test.cpp; do
  bash cpp/gateway/tools/ci-build.sh --config release "$test" || exit 1
done
```

Accept when the parallel spelling vector, Cookie copy and diagnostic-only name table are gone, original valid wire fields still project in the same order and invalid fields cannot reach either writer. Run the existing generated header cases and the loopback writer fixtures through the [isolated driver](../CI.md).
