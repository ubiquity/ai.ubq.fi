# Decode bounded inbound upload parts directly

Plan rows 29–30. Target 195–285 net first-party lines, with 75–125 from upload types and 120–160 from form encoding/mutation. Risk 3. One owner for these adjacent regions. Coordinate with the Headers and HTTP parser lanes.

Own the Blob/form sections at `src/rt/http.cpp:42–69,95–233,576–689,690–734,853–884,989–1089` and the two `formData()` entrypoints at `1145,1238`. Own the types and body API at `include/uos/rt/http.hpp:40–148,212–216,302`. Keep the MIME table and sniff rules at `src/rt/http.cpp:234–575`. Own only upload consumers in `src/images.cpp:121–158,232–310` and `src/live/calls.cpp:58–88`. Use `tests/rt/http_test.cpp` for shared body/form fixture updates.

1. Represent parsed parts with an ordered vector of field names plus a `std::variant` of text and a file struct. Store an optional filename, media type and owned bytes. Use the existing UTF-8 decoder for text. If using slices instead of copies, carry the buffer owner and prove it cannot be scrubbed or destroyed before consumption.
2. Pass the already bounded body and validated content type to the inbound decoder from Images and Live. Preserve first-field and all-field lookup, relative order of `image`/`image[]`, mask cardinality, text-versus-file decisions, MIME precedence, and existing boundary and URL-encoded acceptance. Preserve the current raw-body capture/scrub order. Keep the Image byte, mask, count and fanout-work ceilings and the Live 1 MiB/cancellation path.
3. Confirm zero production users of outbound FormData and general Blob constructors before removing those BodyInit alternatives, serialization, chunk emulation and mutation helpers. Retain BodyInit variants used by the application, body consumption and clone, shared response metadata and cancellation. Exercise the existing multipart payloads as raw requests and use their decoded parts to verify field order, collisions, filenames and media classification. Retain general Request/Response tests.

```bash
for test in tests/rt/http_test.cpp tests/images_test.cpp tests/live_calls_http_test.cpp \
  tests/live_handler_differential_test.cpp tests/provider_images_branch_coverage_test.cpp \
  tests/runtime_http_body_test.cpp tests/bounded_response_body_test.cpp; do
  bash cpp/gateway/tools/ci-build.sh --config release "$test" || exit 1
done
```

Accept when no production caller has to construct a generic Blob/File/FormData just to parse an upload, the retired outbound path is gone, and the actual request fixtures preserve status, payload, size/work rejection, cancellation and capture behavior. [Inbound parser](../../src/rt/http.cpp#L601) · [Image receiver](../../src/images.cpp#L293) · [Live receiver](../../src/live/calls.cpp#L63) · [Driver](../CI.md)
