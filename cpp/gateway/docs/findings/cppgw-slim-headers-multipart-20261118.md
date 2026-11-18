# C++ gateway Headers and inbound multipart

## Work contract v1

Base `6393434b110e2d1afe93e6c7005848219336d9b9`, tree `89b42863ac7dd6a07610e2d15aa320efc42547af`.
Implement and commit Headers first, then the inbound upload decoder. Measure first-party production lines per brief.

| Work | Owned paths | Behavior to preserve |
| --- | --- | --- |
| Headers | `src/rt/headers.cpp`, `include/uos/rt/headers.hpp`, `tests/rt/headers_test.cpp` | ASCII names and ByteString validation, sorted lookup and iteration, first wire spelling, Cookie and Set-Cookie joins, mutation during traversal |
| Inbound uploads | The assigned regions of `src/rt/http.cpp`, `include/uos/rt/http.hpp`, `src/images.cpp`, `src/live/calls.cpp`, and `tests/rt/http_test.cpp` | Ordered text and file parts, multipart and URL-encoded acceptance, Images limits and MIME precedence, Live 1 MiB limit and cancellation, capture and scrub timing |

Both briefs may append to `docs/DEVIATIONS-FROM-BUN.md`. Existing transport writers, HTTP framing, MIME lookup and sniffing, route and accounting policy, and files in other lanes are outside the implementation scope. Qualification uses local mocks in the driver's OS-isolated fixture environment. Deployment and credential operations are outside the task.

Run each brief's named fixtures with `bash cpp/gateway/tools/ci-build.sh --config release <test>`. Because every runtime object links into each test, run the full ARM64 release and ASan/UBSan inventories and compare any failures with the known small-helpers baseline. A cold build is expected to take tens of minutes per profile. Watch the driver logs and final receipts, retry recoverable toolchain failures, and preserve baseline failures for the integration owner.

## Headers

Each sorted field owns its lowercase name, first spelling, and combined value. Set-Cookie remains in its own ordered
vector. The rebased source already removed the diagnostic-only name table. Both value mutation methods now use the
bounded diagnostic, and validation still precedes every mutation.

- Production lines, measured with `wc -l`, fell from **297 to 246 (51 removed)**. The previous lane's 34-line
  reduction is excluded.
- GCC 14 strict compilation of the changed implementation and test passed. The generated scenarios and wire-projection
  regressions passed **6/6** through the driver's Linux namespace helper. The full driver writer fixtures are pending.

## Inbound multipart

Headers commit `12c14bda0180276d54197c89dd5c7fc435392fbc`. Images and Live are the only production form readers.
The multipart step also migrates the dependent body fixtures in `tests/images_test.cpp`,
`tests/provider_images_branch_coverage_test.cpp`, and the typed-body case in `tests/rt/fetch_transport_test.cpp`.
The new decoder validates the same content-type and boundary rules. Decoded file data owns its bytes independently of
the raw buffer retained for replay or scrubbed on exit.

Images and Live use the same decoder with their existing read ceilings. Images reads scalar values by first occurrence,
preserves the order of `image` and `image[]`, and counts every `mask` part before applying the media, size, and fanout
limits. Live reads the first SDP and session values and decodes file parts with the shared UTF-8 codec. Existing replay
capture still runs only after successful decoding and before Images field validation.

The MIME table and sniffing region are byte-identical. The decoder scans parts without allocating an intermediate
list of boundary slices. The removed production surface includes the Blob and File constructors and reads, FormData
mutation and outbound encoding, and the unused asynchronous body-form adapters.

| Production path | Before | After | Net removed |
| --- | ---: | ---: | ---: |
| `src/rt/http.cpp` | 1,259 | 997 | 262 |
| `include/uos/rt/http.hpp` | 325 | 230 | 95 |
| `src/images.cpp` | 566 | 575 | −9 |
| `src/live/calls.cpp` | 182 | 184 | −2 |
| **Total** | **2,332** | **1,986** | **346** |

The test migration keeps the 346 generated HTTP scenarios and uses raw Requests for the 91 multipart parser fixtures.
The two retired Blob/FormData cases now exercise owned file bytes after scrubbing, MIME precedence, and ordered
text/file collisions. The existing redirect diagnostic expectations remain in the generated test.

Strict GCC 14 syntax checks passed for the three changed translation units and four affected test translation units.
The repository's `bun --no-env-file --no-install run size:check` passed for all 851 TypeScript paths in its inventory.
Isolated release and sanitizer qualification is in progress.
