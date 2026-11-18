# C++ terminal record and request ownership performance

## Work contract v1

- Start from `b54357a92c0f2669335d83f04047c3f7de32f925` (tree `d0aabad25d3dbe02f2f33ff519e5b77715d19d37`) on Linux ARM64. Reduce CPU per rejected request by at least 10%, with no regression for model listing or a small inference request.
- Source ownership starts with `cpp/gateway/src/handler/terminal_log.cpp` and `cpp/gateway/src/handler/serve_handler.cpp`. Use the baseline profile to confirm the callback change before editing. A change in source ownership requires a new contract version.
- Keep terminal field names, values, types and null/omission semantics, response statuses and bodies, authentication, delivery receipts, and settlement and durable-write ordering. Preserve owners across suspended or detached work. Deployment, other gateway modules, test edits, and provider traffic are outside this work.
- Run the rejected request, authorized model list, and small inference against loopback fixtures. Measure gateway CPU, request rate, allocation calls, and a reject call-stack profile on the same baseline and candidate configuration.
- After measurement, run the exact reverse `tools/links.py` closure under the portable release and ASan+UBSan profiles, and run the full release suite. Use isolated processes with empty home, fresh temp storage, and only mock credentials. Check the path boundary and diff before handing off the branch.
- The supplied checkout omits the benchmark and its report. Both were recovered from `6878261f` on `origin/claude/cpp-gateway-port` with `git archive` into scratch. The reverse link inventory contains 50 affected binaries among 442 total.

## Before changes

The original scenarios exercised the packaged native gateway, real SQLite storage, and the original loopback mock. Each timing trial used four clients and a fresh private state directory. The native startup enforced the loopback-only fixture boundary. The isolated launcher supplied an empty home and temp directory with synthetic fixture auth inputs. The native-only runner reused the original request driver, mock, launch manifest, health and auth setup, warm-up, dispatch and completion checks, and graceful shutdown.

| Scenario | Timed requests per trial | Median req/s | Median gateway CPU ms/request | Median allocations/request |
| --- | ---: | ---: | ---: | ---: |
| `reject` | 3,000 | 2,182.4 | 0.357 | 1,264.0 |
| `models` | 3,000 | 2,302.6 | 0.373 | 1,514.6 |
| `small` | 1,500 | 374.4 | 1.746 | 5,516.5 |

Timing completed five trials per scenario, 37,500 requests total. Allocation counting completed three trials per scenario, 22,500 requests total. A separate 45,000-request profile reached `logTerminalRequest` in 17.21% of sampled user cycles, compared with 21.68% in the original report. The AArch64 atomic add used by shared ownership took 4.50% exclusive and `_M_release` took 1.43%. The profile included 19,146 samples and 37,659,702,430 period-weighted cycles. 18.33% of leaf cycles lacked symbol resolution.

The portable Clang build initially failed on sign-conversion diagnostics inside the vendored simdutf header included by `src/rt/base64.cpp`. The scratch CI driver adapter marks `third_party/` as a system-header prefix only for that consumer, preserving first-party warnings. The recovered package wrapper also needed the baseline package tool directory on its Python import path. SQLite 3.54.0 required an explicit `libsqlite3.so.0` SONAME at link time for the packager's existing ABI check. The repository build, packaging and runtime files were kept byte-identical.
