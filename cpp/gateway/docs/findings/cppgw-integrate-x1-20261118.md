# C++ gateway cross-line integration and Landlock fallback

## Work contract v1

Integrate stage 1 (`e90858809cf9cd733912be44077e97f949681c18`) into stage 2a
(`30e53aa7a773687ca4717181b24ec13eca2d6028`), then integrate the Landlock
branch (`85e40b69e65f0b6e22f442e7bb5a5a02671513e7`). Both are ordinary
two-parent merges. The starting checkout is `agent-job`, at the supplied
stage 2a tip.

| Surface | Expected result |
| --- | --- |
| JSON, Unicode and body reads | Stage 2a simdjson-only parsing, simdutf conversion, ICU casing and collation, shared input limits and large-body optimizations |
| Date, console, filesystem, environment and Headers | Stage 1 library helpers wired to stage 2a's shared runtime and bounds |
| HTTP and WebSocket | Stage 1 llhttp inbound parsing with stage 2a transport and outbound Beast WebSocket contracts. Preserve bounded reads, admission, delivery and accounting. |
| Confinement | Explicit opt-in to the incoming reduced ABI policy. Match the incoming default policy and seccomp source bytes. |
| Build and packaging | Preserve both lines' dependency closure across portable CI, CMake, standalone tools and package recipes. Compare canonical vendor bytes against the accepted parents. |

Changes are scoped to integration under `cpp/gateway/` and any necessary C++
gateway workflow wiring. First-party application contracts include wire
formats, status codes, stored data, authentication, validation and accounting.
TypeScript, other `cpp/` paths, vendor source edits, unrelated feature branches
and deployment are outside this integration.

Acceptance is four passing Linux ARM64 Release shards from
`bash cpp/gateway/tools/ci-build.sh --config release --shard N/4`, focused
`--config asan` coverage for the conflicts, and a clean stage branch with both
merges and exact receipts. Dependencies and build output stay under runner
scratch. Test processes have a fresh home and temporary directory, a
credential-free environment and the driver's real loopback-only fixture
boundary. The exact conflict inventory, compilation and fixture results are
open at discovery. Prepare dependencies while merging; build, reconcile
shard 1, then finish the remaining shards and focused sanitizer selection.

## Stage 1 merge resolutions

Git found six unmerged paths in the stage 1 merge.

| Path | Resolution |
| --- | --- |
| `cmake/ci/driver.py` | Combined C11 compilation, multi-object archives, compiler identity and receipt fields with explicit SQLite 3.54.0 linkage, the isolated SQLite version probe, and Clang vendor header handling. Tracked each library's actual header with the shared vendor registry. |
| `docs/CI.md` | Combined C/C++ toolchain selection and version/data requirements with the SQLite prerequisite, installed workflow, four-shard commands, and isolated fixture and fuzz instructions. |
| `docs/DEVIATIONS-FROM-BUN.md` | Kept the JSON, UTF, WebSocket, helper and server HTTP sections under a single title and kept one ICU section. |
| `fuzz/targets.json` | Kept Base64 and UTF harnesses alongside the date, Headers, explicit env, POSIX path, console and llhttp server harnesses and their closures. |
| `tools/ci_sanitize.py` | Kept the retirement of the separate runner. Carried over its intent, matching C and C++ toolchains for C11 sanitizer instrumentation, through the unified driver and workflow. |
| `tools/macos-build.sh` | Kept the stage 2a dependency discovery, system headers, executable and test closures, and packaging layout. Incorporated the C11 vendor compiler and multi-object archives with explicit vendor header dependencies. |

Git merged `string.cpp` with one ICU implementation, stage 2a simdutf
conversion and stage 1's scoped ICU header warning handling. The simdjson-only
`json.cpp` and stage 2a limits and large-body path survived unchanged.
Stage 1's console, date, environment, filesystem, Headers and HTTP source
files merged byte-for-byte from its tip. The console and filesystem use
the shared text codec. The headers parser retains its smaller 16,408-byte
and 198-field request-head bounds. CMake and the vendor registry retained
the simdjson and simdutf consumers and added the llhttp C archive and
consumers. The existing Links declarations cover the standalone parser test.

The helper and HTTP fixtures keep stage 1 expectations for strict wire
rejections, bounded Header errors, ISO/IMF date validation and retention
of raw Claude usage reset text. The newer JSON, UTF, WebSocket and
request-path fixtures from stage 2a remain in the merged inventory.
