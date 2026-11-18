# QuickJS-ng libregexp replacement

## Work contract v1

Replace the first-party compiler, matcher and Unicode tables in replacement-plan row 9 with
QuickJS-ng 0.18.0. Keep the public C++ regex API and JavaScript string operations. This unit
covers the owned regex files, vendor files and registration, test expectations, fuzz target,
and findings. Application modules, TypeScript, other libraries, CI workflows, and deployment
are outside this unit.

| Caller or operation                                  | Required behavior                                                                                                         |
| ---------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------- |
| Request, authentication and configuration validators | Fixed patterns keep their acceptance decisions. Invalid syntax and resource exhaustion fail closed.                       |
| SSE and provider framing                             | Preserve delimiter spans, byte and UTF-16 offsets, split, and replacement.                                                |
| Supervisor redaction and compaction                  | Preserve captures, named groups, global scanning and empty-match progress. Surface incomplete matching as an error.       |
| Stored regex and migration profile inputs            | Preserve compilation and source/flags projections for supported patterns. QuickJS determines syntax and Unicode behavior. |
| C++ callers                                          | Preserve const execution, mutable lastIndex, regex copies, indices, and replacement callbacks.                            |

Acceptance uses Clang 21 on Linux ARM64 with the strict port flags, frozen Boost 1.92.0 archives,
credential-empty test children, release and ASan/UBSan Links closures, and a 120-second fuzz run.
Compare the original and new implementations with the same optimized binary inputs and allocation
counters. The unit ends when the changed code is committed and the results and any remaining
verification limits are recorded. The supervisor integrates the job branch into the C++ port.

## Continuation and baseline

The checkout started on `agent-job` at `09f9840dd1df3498d83f6f32b1987beaddbaec64`
(tree `ffb9ee064ae32ad56e46b4fc2a9915d9d3f0f1a1`). The branch history and
`docs/findings/` contained no regex work from an earlier job. The baseline regex binary
built with Clang 21.1.8 and passed 16/16 cases. The starting production regex surface was
6,318 physical lines across six files.

Downloaded the QuickJS-ng tag archive outside the checkout. The archive hash matched the task
pin and the eight vendored files passed `sha256sum --check SHA256SUMS`. The standalone C source
closure consists of `libregexp.c` and `libunicode.c`, verified by compiling each with
`clang-21 -O2 -c` and inspecting the resulting dependencies with `nm -u`.

The baseline control and final sweeps use separate build directories. Full release and sanitizer
builds each compile every object once with four jobs. A stage has progress through Ninja object
counts and the per-binary test logs. A compile failure is recorded before continuing independent
targets. Baseline and final measurement inputs and commands will be recorded below.

## Continuation work contract v2

The incoming commit `c7766e898a63b3c67e4b8a0978746f725785857d` (tree
`373edd6a91543795644b3a46572f1cd56feb6f8e`) already replaces the matcher and compiler,
registers the vendor, and adds the fuzz target. Continue from that adapter. The caller
matrix above still applies. The current policy makes QuickJS the syntax and matching
reference. The C++ API and the gateway's validation, framing, redaction, and data
contracts remain required.

| Changed surface                                                            | Verification                                                                                                                           |
| -------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------- |
| `src/rt/regex*`, `include/uos/rt/regex.hpp`, vendor and build registration | Strict CMake builds and all 441 Links closures in optimized and ASan/UBSan profiles. Every closure links the regex runtime.            |
| `tests/rt/regex_test.cpp` and regex fixtures under `tests/legacy/`         | Keep the Bun-recorded vectors and apply only classified expectation changes. Exercise resource exhaustion and Unicode-sets operations. |
| `fuzz/regex_library.cpp`, its registration, and seed corpus                | Instrument the adapter and both vendor translation units and run libFuzzer for 120 seconds.                                            |
| This report and `docs/DEVIATIONS-FROM-BUN.md`                              | Document syntax, matching and resource changes, verify source boundaries and record timings and allocation counts.                     |

Only the owned paths and the named vendor registries are in scope. Application code,
TypeScript, deployment, shared test drivers and other libraries are outside the task.
The remaining questions are the exact library deviations, resource-limit behavior in
both profiles, and whether the portable builders can use the C-source registration.
Record terminal build and test totals, benchmark both engines on the same runner and
commit the qualification with the C++ port's job-branch handoff.

### Incoming build

On Ubuntu 24.04 ARM64, Clang 21.1.8 and staged Boost 1.92.0, the incoming regex target
compiled and linked under the strict port flags. The first CMake attempt needed Clang's
`--system-header-prefix=third_party/` flag for the existing simdutf header, as used by
the portable sanitizer driver. The retry built all 44 runtime translation units and
the two QuickJS C translation units. The regex binary reported 10 passing and 6 failing
cases out of 16 against the original Bun vectors. The 171 production-pattern rows
(63,131 checks) and 570 test-pattern rows (6,421 checks) passed. The failures were in
the semantic battery, syntax, random syntax, Unicode properties, constructor limits and
backtracking memory. The checkout's vendor manifest and the bytes extracted from the
pinned archive both verified.

## Continuation on Linux ARM64

The next incoming commit, `90d2076041ddb8610c8d4d2f438acddbb5c34974` (tree
`ddf39d01820a4d420781cc8f251384d73f9f694b`), adds the QuickJS expectation overlay
and resource, Unicode-set and embedded-NUL cases. A fresh strict Clang 21.1.8
release build of the incoming CMake Links closure passed 19/19 cases. All
171 production-pattern rows and 570 test-pattern rows still pass their original
Bun expectations, covering 63,131 and 6,421 checks respectively. The battery,
syntax and random groups passed 19,242, 1,735 and 27,513 checks with the
classified changes. Unicode property tests passed 602/602 and case folding
passed all 83 blocks. The overlay binds exceptions to the original fixture
hashes and preserves all source vector documents.

The caller audit found 101 application source files using `rt::Regex`.
Request, authentication and configuration validators use fixed syntax.
The Harmony glob builder escapes caller-provided literal text and adds its
own wildcard syntax. KV conversion compiles stored expressions and projects
their source and flags. It does not use those expressions to authorize access.
The changed syntax outcomes and resource limits are listed in
[Deviations from Bun](../DEVIATIONS-FROM-BUN.md).

CMake builds both vendor C sources for the gateway and its Links targets.
For the local sweep, a scratch controller runs those binaries against the
generated Links registry using the repository's `cmake/ci/isolate.py` Linux
network, mount and PID boundary. The checkout's portable Ninja driver assumes
one C++ source per vendor. The older `tools/ci_sanitize.py` discovers C
translation units but drops them while assembling the Links inputs. Both are
outside this assignment's owned paths.

## Continuation work contract v3

Qualify the inherited QuickJS adapter on the existing Linux ARM64 runner. The
application behavior matrix above still applies. In particular, large inline images
must retain the documented size allowance and compaction must finish its existing
large-transcript fixture. Captures, offsets, flags and the public API stay compatible.
The earlier engine stays out of production. The owned paths remain those listed in
the assignment, with any additional path requiring explicit authorization.

The pre-fix release Links sweep covered 441 binaries and reported 437 passing and
4 failing, with 5,019 of 5,027 cases passing. The ASan/UBSan sweep covered the same
441 binaries and reported 435 passing and 6 failing, with 5,017 of 5,027 cases
passing. Both runs found the large-image failures, two KV suites asserting retired
regex diagnostics or missing `v` flag support, and URL IDNA failures. ASan/UBSan
also found a one-second execution deadline in the large compaction fixture and a
different parser limit reached by nested captures with larger instrumented frames.
The current fuzz harness passed 711,754 inputs in 121 seconds with ASan/UBSan after
switching the private network namespace to the runner's `sudo` fallback.

Run the changed regex test and affected application controls first, then link the
final runtime into both full CMake Links inventories. Use the pre-swap engine as an
independent control for the failures and compare the same optimized operations and
allocation counters. Retain a final two-minute smoke result for the updated runtime.
The handoff ends with committed source and a report that names any failing gates and
the scope or technical blocker for each one.

### Resource-limit follow-up

The execution deadline is 2.5 seconds per operation. The previous one-second limit
interrupted the large compaction fixture under ASan/UBSan. The poll and allocation
ceilings still apply. The nested-capture test requires an ordinary JS-style error
and checks the diagnostic for the limit reached, since instrumented frames use
more stack than optimized frames.

The optimized pre-swap control passed all 37 image cases and all 12 cases across
the two KV differential binaries. It reproduced both URL IDNA failures on the
runner's ICU 74.2. The image failure is a VM memory regression on the existing
whole-payload character-class repeat. The KV differences are support for the
recorded `v` expression and new syntax diagnostics.

## Continuation work contract v4

The inherited release build at `549fa65dea484d71642998baecc40a35d3ac9b9f`
(tree `2dc2c66f037d50946b18b393991b08e074240b94`) was current in Ninja.
The focused isolated rerun passed all 19 regex cases and reproduced the three
image and three KV case failures. The existing image expression must match the
documented upload size with unchanged validation and capture spans. Its compiled
form can use fixed-size blocks and an atomic suffix, both executed by libregexp,
without touching the image caller or vendor source. The earlier scratch probes
showed why both parts are needed: chunking alone still exhausts the execution
budget on malformed long payloads.

The matrix in v1 still applies. The remaining owned source changes are confined
to the regex adapter and its tests. Application and shared test-driver changes
are outside scope. Run focused image, compaction and regex tests, then both
CMake Links sweeps and the 120-second sanitizer fuzz campaign. Use the existing
same-runner comparison harness for timing and allocation counts. Finish when the
source and evidence are committed and any remaining out-of-scope failures are
identified by test and cause.

The optimized and ASan/UBSan focused runs each passed 80/80 cases across the
regex (20), image (37), and compaction (23) binaries. The added regex case checks
a large Base64 capture and its spans, then verifies that a malformed suffix
returns no match. The adapter preserves the original source and public flags.
