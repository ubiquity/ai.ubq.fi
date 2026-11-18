# QuickJS-ng libregexp qualification

## Continuation contract v5

The incoming `ab5c6b8815c0d286e324267f025e2a2ad5b9a0bd` tree
`699073d8a39533704addbde1d8c0998cc3a2a980` already replaces the C++ compiler,
matcher, and Unicode tables with QuickJS-ng 0.18.0. This continuation qualifies
that implementation and repairs any remaining gateway regressions within the
regex lane. The inherited implementation, earlier receipts, and prior failure
analysis are recorded in [the previous handoff](cppgw-std-regex-r1.md).

| Surface                             | Required behavior and check                                                                                                                       |
| ----------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| Regex API                           | Strict C++20 port flags, original vector coverage, classified QuickJS results, UTF-16 and byte offsets, captures, and stateful methods.           |
| Image and authentication validation | Preserve the gateway's accepted payloads and rejection decisions, including large inline images.                                                  |
| SSE, redaction, and compaction      | Preserve segmentation, capture spans, replacement, and large-input completion.                                                                    |
| KV interchange                      | Preserve valid source and flags through round trips and reject malformed expressions. QuickJS supplies the regex syntax diagnostic.               |
| Resource limits                     | Use the existing deadline, poll, allocation, and compiler-stack limits. Verify with regression cases, ASan/UBSan, and the 120-second fuzz target. |

The owned paths are the regex runtime and public header, its vendor build
registration, regex-only tests and fixtures, fuzz target and corpus, and the
findings and deviations. Application modules, other tests, shared build drivers,
TypeScript, workflows, and deployment are outside the continuation. The portable
driver currently accepts one C++ translation unit per vendor, while QuickJS has two
C translation units. Use a scratch compiler launcher and a separately compiled
Unicode object to run the unchanged driver. Keep production and runner outputs
outside the checkout and run test children in the driver's OS fixture boundary.

Run `bash cpp/gateway/tools/ci-build.sh --config release rt/regex_test.cpp` before
the four release shards, then run the ASan/UBSan configuration. A cold build may
take tens of minutes per configuration. Track Ninja object counts and per-test
results on the running build, keep failure logs, and use focused isolated reruns
to diagnose discrepancies. Run a two-minute fuzz campaign and compare the parent
and candidate on the same release toolchain and fixtures, including allocator
calls. The handoff is ready when those results, any remaining scope blockers, and
the exact source identities are committed to the job branch.

## Runner and preflight

The runner is Ubuntu 24.04.5, Linux AArch64, four CPUs, with Clang 21.1.8
from the LLVM apt repository and Bun 1.4.0. Boost.JSON and Boost.Container were
built as static libraries with `./bootstrap.sh --with-libraries=json,container`
and `./b2 -j2 toolset=gcc cxxflags=-fPIC link=static variant=release threading=multi
--with-json --with-container stage` from the official Boost 1.92.0 source
archive (SHA-256 `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c`).
The suite used ICU 78.3 and SQLite 3.54.0 built from their official archives.
`SELECT "qt";` succeeded on the staged shared SQLite library built with
`CFLAGS='-O2 -DSQLITE_DQS=3'`. The other runtime dependencies came from Ubuntu
packages (OpenSSL 3.0.13, zlib 1.3, Brotli 1.1.0, and Zstd 1.5.5).

The QuickJS-ng archive verified against
`28bcd89aa3683fd2a946537e5ce3f11cc46b5cbf3383c7561aaef3df5ff9f1dd`.
`sha256sum -c SHA256SUMS` passed all eight entries, and each checked-in file
matched the bytes extracted from the archive. The portable driver compiled
369/369 release production translation units under the fatal port warnings.
The preflight regex binary linked but ran zero cases because the fixture's
private `/tmp` hid `libicuuc.so.78`. The rerun stages the ICU and SQLite
shared libraries under a separate `/var/tmp` directory and uses it as the
binary's runtime library search path. The driver keeps its private fixture
filesystem and loopback-only network boundary.

## Focused release qualification

The focused command `bash cpp/gateway/tools/ci-build.sh --config release rt/regex_test.cpp`
passed 1/1 binaries and 20/20 cases, with a clean 369/369 production object phase
in 577.378 seconds. It ran inside the driver's private Linux network, mount,
and PID namespaces via the runner's `sudo` fallback. The input digest stayed
`6f477813df431cf600bd698037a67d23044438b62e790868619c73ad86527b53`
throughout the run. The receipt identifies commit
`d39285b30f7e9be1959d062a5e3c51eb7d3632bc` and tree
`2129071d979caaa78a404916dc00988b9c95116a`. That commit and the preflight
report commit have the same executable inputs.

The separate CMake vendor target also built both upstream C11 translation units
and archived them under `uos_vendor_quickjs-libregexp`. The portable CI driver
still passes only the first vendor source to its build graph. The local check
uses Clang 21 in C11 mode for `libregexp.c` and passes the separately compiled
`libunicode.c` object at link time. Both sources use the selected optimization
and sanitizer settings. No production or shared driver changes are needed to
perform this local qualification.

## Full release sweep

All four portable release shards completed on the strict Clang 21 build.
The driver ran `bash cpp/gateway/tools/ci-build.sh --config release --shard N/4`
for each `N` from 1 through 4 with the same compiler launcher and link inputs
as the focused run.

| Shard | Binaries passed | Cases passed | Build failures | Timeouts |
| ----- | --------------: | -----------: | -------------: | -------: |
| 1/4   |         111/111 |  1,290/1,290 |              0 |        0 |
| 2/4   |         109/110 |  1,027/1,029 |              0 |        0 |
| 3/4   |         110/110 |  1,376/1,376 |              0 |        0 |
| 4/4   |         109/110 |  1,332/1,333 |              0 |        0 |
| Total |         439/441 |  5,025/5,028 |              0 |        0 |

The four receipts identify commit `b63fe35a798cbaf50fd0c7ea1f1ee281689af649`,
tree `4dd8f5eb03d74dde0de4ffcab22e6bc750cb1153` and input digest
`6f477813df431cf600bd698037a67d23044438b62e790868619c73ad86527b53`.
The digest matched before and after each shard. All 369 production objects
built. The regex suite passed 20/20, images passed 37/37, and the Jev
compaction suite passed 23/23. The URL tests that failed in the earlier
ICU 74.2 run passed with ICU 78.3.

The two failing binaries are outside the owned paths.

- `tests/kv_conversion_typed_value_differential_test.cpp` passes 5/7 cases.
  The round-trip case expects `/a/v` to throw because the former C++ engine
  lacked `v`. The malformed graph case has six regex inputs that still throw
  `SyntaxError`, with new diagnostics for invalid flags or an unclosed group.
- `tests/kv_conversion_profile_json_differential_test.cpp` passes 4/5 cases.
  The three failing comparisons still reject malformed input with
  `SyntaxError`. The new diagnostics are `unexpected end` for `/[/` and
  `invalid regular expression flags` for `/a/zz` and `noslash`.

A Bun 1.4.0 probe with `--no-env-file --no-install` confirmed that `/a/v`
compiles and projects source `a` and flags `v`. It also confirmed the old
diagnostics for the malformed patterns and flags. The two test files retain
their original assertions under the job's path scope.

A scratch binary linked the original `typed_value.cpp` and `profile_json.cpp`
objects and passed 4/4 controls in each configuration. Both typed and native
graph codecs round-tripped the recorded `/a/v` graph along with the other two
regexes in that vector, preserving the encoded bytes. Both rejected all six
malformed graph inputs with `SyntaxError`. The profile JSON codec preserved four
valid wire values including `/[a--b]/v` and rejected all three malformed inputs
with `SyntaxError`. The controls used the same Linux fixture boundary as the
suite.

## ASan and UBSan sweep

The same portable driver ran `bash cpp/gateway/tools/ci-build.sh --config asan rt/regex_test.cpp`
and `bash cpp/gateway/tools/ci-build.sh --config asan --shard N/4` for `N` from
1 through 4. It compiled all 369 production translation units in both
sanitizers at `-O1 -gline-tables-only -fno-omit-frame-pointer` with the port
warnings. The driver enabled leak detection, aborted on ASan findings, and
halted on UBSan findings. The focused run passed 20/20 cases. The suite has
the same two KV assertion failures as release and no sanitizer findings.

| Shard | Binaries passed | Cases passed | Build failures | Timeouts | Elapsed seconds |
| ----- | --------------: | -----------: | -------------: | -------: | --------------: |
| 1/4   |         111/111 |  1,290/1,290 |              0 |        0 |         400.460 |
| 2/4   |         109/110 |  1,027/1,029 |              0 |        0 |         367.108 |
| 3/4   |         110/110 |  1,376/1,376 |              0 |        0 |         469.387 |
| 4/4   |         109/110 |  1,332/1,333 |              0 |        0 |         444.478 |
| Total |         439/441 |  5,025/5,028 |              0 |        0 |       1,681.433 |

Every test binary links the regex runtime. The four shard receipts cover
441 distinct binaries and bind the same commit, tree, and unchanged input
digest as the release sweep. The image and compaction binaries again passed
37/37 and 23/23 cases.

## Adapter and source size

Against `09f9840dd1df3498d83f6f32b1987beaddbaec64`, the six owned production
regex files have 5,668 lines deleted and 251 added. They went from 6,318 to
901 physical lines, a net reduction of 5,417 lines. Counts exclude vendor,
registration, tests, and documentation.

Libregexp owns parsing, execution, and Unicode data. The adapter keeps the
C++ API, source and flag projection, UTF-16 and byte offsets, named captures,
and state and progress for exec, match-all, replace, and split. The image
validator's anchored pattern compiles to bounded repeats with a lookahead
that commits to the matched payload span. This keeps existing large uploads
within the library's backtracking limits while preserving validation and the
original projected source. The execution budget also covers large compaction
scans. [Deviations and resource limits](../DEVIATIONS-FROM-BUN.md)

## Fuzz smoke

The inherited `regex_library` harness selects one public operation per input
and catches ordinary `rt::JsError` exceptions. Its seven seeds cover captures,
replacement, splitting, Unicode sets and offsets, embedded NUL, invalid bytes,
and a zero-length global match. The Clang 21 libFuzzer build exercised both
vendor translation units under ASan and UBSan. The 120-second smoke campaign
ran for 123 seconds including shutdown, executed 901,617 inputs, retained
2,271 corpus entries, and exited zero with no crash, timeout, or OOM artifacts.

```sh
python3 -B cpp/gateway/fuzz/ci.py regex_library --compiler clang++ --jobs 1 \
  --seconds 120 --out /tmp/cppgw-regex-c2.OHm0Ix/fuzz-final
```

## Before and after measurements

The baseline uses the six first-party regex files from
`09f9840dd1df3498d83f6f32b1987beaddbaec64` (tree
`ffb9ee064ae32ad56e46b4fc2a9915d9d3f0f1a1`). The candidate uses the release
objects qualified above. Both use Clang 21.1.8, `-O2`, the fatal port
warnings, the same Boost libraries, and the same supervisor redaction, string,
number, and JSON objects. Neither timing binary has allocator instrumentation.
The allocation binaries separately wrap the C allocation calls and scalar,
array, and aligned C++ `operator new` calls from the linked objects. The counts
include nonzero `realloc` requests and exclude allocation calls made solely
inside dynamically linked libraries.

The run used the 18 existing regex fixtures and four compile-only fixtures
made from the bearer, SSE, Unicode-property, and credential-key patterns. Each
workload used three warmup batches and seven timed batches, with 1 to 2,000
operations per batch as set by the manifest. Both engines ran serially on CPU 2
in a private network namespace with fresh scratch directories and a cleared
environment. The driver alternated engine order and compared the resulting
text, captures, offsets, `lastIndex`, or compile result, as applicable. All 22
outputs matched. Times are medians in nanoseconds per operation.
[Fixture generator](../../bench/regex/make_fixtures.py)

| Workload                              | Input bytes |   Old ns/op | QuickJS ns/op | Old alloc/op | QuickJS alloc/op |
| ------------------------------------- | ----------: | ----------: | ------------: | -----------: | ---------------: |
| Redact words                          |     210,000 |  23,189,561 |   126,720,209 |            8 |              233 |
| Redact dotted text                    |     200,000 | 443,207,159 |   528,181,481 |            8 |               23 |
| Redact dashed text                    |     210,000 | 283,761,726 |   383,690,611 |            8 |               23 |
| Redact assignments                    |     210,000 |  88,963,015 |   204,923,454 |      385,220 |          385,246 |
| Redact brackets                       |     225,000 |  86,964,810 |   174,042,773 |      287,763 |          375,268 |
| Redact quoted text                    |     240,000 |  85,523,856 |   214,358,841 |      260,257 |          340,262 |
| Redact transcript                     |       6,320 |   2,161,496 |     4,744,065 |        1,080 |            1,819 |
| Redact Unicode text                   |       4,096 |   1,486,456 |     3,311,101 |        1,550 |            2,334 |
| Credential-key search without matches |     210,000 |   1,931,937 |     6,857,907 |            0 |               25 |
| Credential-key search with matches    |       6,304 |     219,622 |       294,657 |           70 |              152 |
| Bearer header capture                 |         263 |         612 |         2,483 |            4 |               12 |
| SSE line splitting                    |      14,848 |      51,030 |       302,066 |          524 |              524 |
| Unicode-property search               |       5,120 |     293,422 |       332,493 |        3,603 |            3,606 |
| Global whitespace replacement         |       9,216 |     105,430 |       145,341 |           22 |               26 |
| Invalid Base64 test                   |      24,577 |     205,639 |       690,766 |            7 |               17 |
| Unicode sticky match                  |          13 |         243 |           362 |            3 |                3 |
| Provider model test                   |         244 |         438 |         2,097 |            0 |                9 |
| Title search without a match          |      24,583 |     246,005 |       569,061 |            0 |               20 |
| Compile bearer header                 |           0 |      13,909 |         1,778 |          102 |               52 |
| Compile SSE lines                     |           0 |         760 |           558 |           24 |               11 |
| Compile Unicode property              |           0 |      14,445 |        30,255 |           46 |              112 |
| Compile credential key                |           0 |      27,142 |         3,172 |          212 |               66 |

QuickJS reduced compile time for three patterns. Unicode-property compilation
took 2.09 times as long. The sampled execution workloads took 1.13 to 5.92
times as long with QuickJS. The 14,848-byte SSE fixture took 302 µs per
operation and made the same 524 allocation calls in both engines.

The local benchmark executable accepts `MANIFEST FIXTURE SAMPLES WARMUPS ITERATIONS`.
For example, the timed SSE command inside the namespace was `taskset -c 2
new-time fixtures.json sse-lines 7 3 5` with absolute executable and manifest
paths. The equivalent allocation command used `new-alloc`. The paired run
took 41.551 seconds. The fixture manifest SHA-256 is
`ccf33eab4cdacffc32e56e777838c38e74532a0b285f24ae5182468333113479`.
The output with all samples is
`/tmp/cppgw-regex-c2.OHm0Ix/bench-run.9mK1a1/summary.json` (SHA-256
`dc4ca5e2c256bfa838f8c9729dd4f536283fe2c9e15c190e74c7d61eb5debc16`).

## Handoff

The focused regex gate passed 20/20 in both configurations. The release and
sanitizer sweeps each passed 439/441 binaries and 5,025/5,028 cases. The two KV
test files still need expectation updates outside this job's ownership. The
separate controls passed the underlying wire behavior in both configurations.

The portable driver's shared Ninja registration still needs C compilation and
multi-source vendor support. A direct `clang++-21 -std=c++20 -O2 -c
cpp/gateway/third_party/quickjs-libregexp/libregexp.c` probe exited 1 with
invalid C++ conversions from `void*`. Its archive contains only `libregexp.c.o`
and leaves Unicode symbols unresolved. CMake's archive contains `libregexp.c.o`
and `libunicode.c.o`. For the portable sweeps, the scratch compiler launcher
compiled the first file in `-std=gnu11` mode, and `UOS_CI_LDFLAGS` supplied the
second object built in the same configuration. The shared portable driver and
KV test expectations are both outside the owned paths.
