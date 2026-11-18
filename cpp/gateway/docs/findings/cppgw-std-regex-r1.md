# QuickJS-ng libregexp replacement

## Work contract v1

Replace the first-party compiler, matcher and Unicode tables in replacement-plan row 9 with
QuickJS-ng 0.18.0. Keep the public C++ regex API and JavaScript string operations. This unit
covers the owned regex files, vendor files and registration, test expectations, fuzz target,
and findings. Application modules, TypeScript, other libraries, CI workflows, and deployment
are outside this unit.

| Caller or operation | Required behavior |
| --- | --- |
| Request, authentication and configuration validators | Fixed patterns keep their acceptance decisions. Invalid syntax and resource exhaustion fail closed. |
| SSE and provider framing | Preserve delimiter spans, byte and UTF-16 offsets, split, and replacement. |
| Supervisor redaction and compaction | Preserve captures, named groups, global scanning and empty-match progress. Surface incomplete matching as an error. |
| Stored regex and migration profile inputs | Preserve compilation and source/flags projections for supported patterns. QuickJS determines syntax and Unicode behavior. |
| C++ callers | Preserve const execution, mutable lastIndex, regex copies, indices, and replacement callbacks. |

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

| Changed surface | Verification |
| --- | --- |
| `src/rt/regex*`, `include/uos/rt/regex.hpp`, vendor and build registration | Strict CMake builds and all 441 Links closures in optimized and ASan/UBSan profiles. Every closure links the regex runtime. |
| `tests/rt/regex_test.cpp` and regex fixtures under `tests/legacy/` | Keep the Bun-recorded vectors and apply only classified expectation changes. Exercise resource exhaustion and Unicode-sets operations. |
| `fuzz/regex_library.cpp`, its registration, and seed corpus | Instrument the adapter and both vendor translation units and run libFuzzer for 120 seconds. |
| This report and `docs/DEVIATIONS-FROM-BUN.md` | Document syntax, matching and resource changes, verify source boundaries and record timings and allocation counts. |

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
