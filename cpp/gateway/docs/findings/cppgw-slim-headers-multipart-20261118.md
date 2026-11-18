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

## Checkpoint

Headers is ready for its component commit. Multipart caller discovery is in progress.
