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
