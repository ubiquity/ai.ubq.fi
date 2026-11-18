# C++ gateway cross-line integration and Landlock fallback

## Work contract v1

Integrate stage 1 (`e90858809cf9cd733912be44077e97f949681c18`) into stage 2a
(`30e53aa7a773687ca4717181b24ec13eca2d6028`), then integrate the Landlock
branch (`85e40b69e65f0b6e22f442e7bb5a5a02671513e7`). Both are ordinary
two-parent merges. The starting checkout is `agent-job`, at the supplied
stage 2a tip.

| Surface                                            | Expected result                                                                                                                                                       |
| -------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| JSON, Unicode and body reads                       | Stage 2a simdjson-only parsing, simdutf conversion, ICU casing and collation, shared input limits and large-body optimizations                                        |
| Date, console, filesystem, environment and Headers | Stage 1 library helpers wired to stage 2a's shared runtime and bounds                                                                                                 |
| HTTP and WebSocket                                 | Stage 1 llhttp inbound parsing with stage 2a transport and outbound Beast WebSocket contracts. Preserve bounded reads, admission, delivery and accounting.            |
| Confinement                                        | Explicit opt-in to the incoming reduced ABI policy. Match the incoming default policy and seccomp source bytes.                                                       |
| Build and packaging                                | Preserve both lines' dependency closure across portable CI, CMake, standalone tools and package recipes. Compare canonical vendor bytes against the accepted parents. |

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

Stage 1 merged in `a05c608816661132f2e56962c8e4e7f5fbf02611`. Git found six
unmerged paths. Paths in the table are relative to `cpp/gateway/`.

| Path                          | Resolution                                                                                                                                                                                                                                                            |
| ----------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `cmake/ci/driver.py`          | Combined C11 compilation, multi-object archives, compiler identity and receipt fields with explicit SQLite 3.54.0 linkage, the isolated SQLite version probe, and Clang vendor header handling. Tracked each library's actual header with the shared vendor registry. |
| `docs/CI.md`                  | Combined C/C++ toolchain selection and version/data requirements with the SQLite prerequisite, installed workflow, four-shard commands, and isolated fixture and fuzz instructions.                                                                                   |
| `docs/DEVIATIONS-FROM-BUN.md` | Kept the JSON, UTF, WebSocket, helper and server HTTP sections under a single title and kept one ICU section.                                                                                                                                                         |
| `fuzz/targets.json`           | Kept Base64 and UTF harnesses alongside the date, Headers, explicit env, POSIX path, console and llhttp server harnesses and their closures.                                                                                                                          |
| `tools/ci_sanitize.py`        | Kept the retirement of the separate runner. Carried over its intent, matching C and C++ toolchains for C11 sanitizer instrumentation, through the unified driver and workflow.                                                                                        |
| `tools/macos-build.sh`        | Kept the stage 2a dependency discovery, system headers, executable and test closures, and packaging layout. Incorporated the C11 vendor compiler and multi-object archives with explicit vendor header dependencies.                                                  |

Git merged `string.cpp` with one ICU implementation, stage 2a simdutf
conversion and stage 1's scoped ICU header warning handling. The simdjson-only
`json.cpp` and stage 2a limits and large-body path survived unchanged.
Stage 1's console, date, environment, filesystem, Headers and HTTP source
files merged byte-for-byte from its tip. The console and filesystem use
the shared text codec. The HTTP server retains its smaller 16,408-byte
and 198-field request-head bounds. CMake and the vendor registry retained
the simdjson and simdutf consumers and added the llhttp C archive and
consumers. The existing Links declarations cover the standalone parser test.
The unified workflow now selects the matching C compiler alongside Clang++.

The helper and HTTP fixtures keep stage 1 expectations for strict wire
rejections, bounded Header errors, ISO/IMF date validation and retention
of raw Claude usage reset text. The newer JSON, UTF, WebSocket and
request-path fixtures from stage 2a remain in the merged inventory. The merge
commit records why the strict expectations follow the selected libraries.
The ISO/IMF parser rejects legacy numeric date strings for Codex refresh
validation. The Claude schema vectors retain the original reset text and
expect an absent parsed deadline for those strings. The HTTP vectors retain
the inputs and assert the applicable strict framing or header-size response.
Every test binary from either input branch remains in the Release inventory.

## Stage X merge resolution

Stage X merged in `88d62fe3cfe6839f7c06f1bdc7c69daee0c00a09`. Git found one
conflict in `docs/DEVIATIONS-FROM-BUN.md`. The resolution preserves the Linux
confinement section and the merged library sections, with the incoming
integration guidance at the end. `entrypoints/gateway.cpp`, `runtime/startup.cpp`,
`include/uos/runtime/permissions_linux.hpp`, `src/runtime/permissions_linux.cpp`,
and both Linux permissions fixtures match the fallback branch byte for byte.
This includes the default ABI 8 policy and seccomp filter. The opt-in
`UOS_GATEWAY_ALLOW_REDUCED_CONFINEMENT=1` enables the incoming policy on
ABI 3–7. The Raspberry Pi and build guides describe the opt-in and the
seccomp thread synchronization requirement.

Stage 2a's vendor tree matches exactly outside `third_party/llhttp`, and
`third_party/llhttp` matches stage 1 exactly. The vendor registry verified
all three libraries' source and license hashes.

## Initial Release shard and build path fixes

On merge tree `f2c9123f8a30a7762451a4d56735a4cf13d6a20f`, Release shard
1/4 compiled all 369 production sources. All 112 tests and 1,311 cases
passed. This cold run took 939.506 seconds, with 920.005 seconds in the
object phase and 18.888 seconds linking and running tests. No source or
fixture repair was required by the shard.

The build review found two llhttp integration gaps outside the portable
test driver. The portable Docker CMake context needed the parser test file
declared as a vendor consumer. The direct fuzz CI driver misclassified
versioned `clang++` as a C compiler. The Docker path now includes the
declared consumer and sets the ARMv8-A C toolchain flags. The fuzz driver
compiles vendor C files in C11 mode with the matching Clang or explicit
`CC`. The CMake extra-target registry provides the standalone library
harnesses with explicit link closures. The Raspberry Pi guide now matches
the fallback's opt-in configuration.

The CMake build also found missing HTTP body, early SSE, request task, upgrade
and WebSocket objects in the existing `uos_fuzz_http_request` closure. Both
HTTP harnesses now link those objects. The portable build profile records the
statically linked llhttp 9.4.4. A separate standalone fuzz attempt mixed
ICU 78 headers with the system ICU 74 libraries. The rerun uses the same
staged headers and libraries through Clang's search paths.

## Linux ARM64 release and sanitizer results

The final build-input commit was `20cad4cf8b0ab8bb0fe0b74b40dfc27c5126feb4`,
tree `073aabe4c1034a4b1638df6d69ec38919870d627`. All five driver receipts
have input SHA-256
`4487834eff106fc599066c9688b3f5231e9a6df888d521efe898af1aac6d3aa0`, equal
before and after each run. Release covered the 445 sorted tests exactly
once across the four shards. All test binaries linked and ran. Each
configuration built or reused 369 production objects, including 44 runtime
objects, plus the three vendor archives.

| Configuration | Binaries passed | Cases passed | Object phase (s) | Link and run (s) | Total wall (s) |
| ------------- | --------------: | -----------: | ---------------: | ---------------: | -------------: |
| Release 1/4   |             112 |        1,311 |            0.008 |           23.607 |         24.138 |
| Release 2/4   |             111 |        1,125 |            0.008 |           13.703 |         14.230 |
| Release 3/4   |             111 |        1,455 |            0.008 |           24.081 |         24.613 |
| Release 4/4   |             111 |        1,163 |            0.009 |           41.602 |         42.251 |
| Release total |             445 |        5,054 |                  |                  |        105.232 |
| Focused ASan  |             113 |        1,280 |            0.007 |          170.892 |        171.571 |

There were zero failures, build failures, or timeouts in these runs. The
sanitizer profile enabled AddressSanitizer, UndefinedBehaviorSanitizer and
LeakSanitizer. Selection included every test whose `tools/links.py` closure
contains `src/runtime/http.cpp`, plus the runtime helper and codec tests,
application date-sensitive fixtures, request and delivery fixtures, and
startup and permissions tests listed below. The two permissions binaries
passed five cases each, the standalone llhttp binary passed seven, and
startup passed 18 in both configurations.

The final timings are incremental. Before that check, the interrupted
sanitizer build left 285 completed Ninja edges. Compiling the remaining 219
edges took 804.911 seconds. The resumed run linked and ran the 113 binaries
in 88.705 seconds and passed 1,280 cases, taking 894.167 seconds overall.

| File under the runner scratch directory | Receipt SHA-256                                                    |
| --------------------------------------- | ------------------------------------------------------------------ |
| `receipts/final/release-1.json`         | `5b7036e43087a2609d4f7f4d0a850e43bca759b3ced2f7c17a8b2ae1928ccb8c` |
| `receipts/final/release-2.json`         | `5b603018b5a6e76bef9a0d140488df91dfd4ccecc14e78e4f0aba39c3dc7cd57` |
| `receipts/final/release-3.json`         | `d81dcaf115f2215cf5e0691b9f181b886a8e917f9105b7e052f0451f3b000872` |
| `receipts/final/release-4.json`         | `8b6500a06f03ee6d7e18c06d0bd52905424a844aee3f16e6a125277fd85646a0` |
| `receipts/final/asan.json`              | `32373edcf1ec6d4a4d7999fcf8b1fdcab0c330631ea52dad52cee6cda1499349` |

The runner was Ubuntu 24.04.5, Linux `7.0.0-1017-azure`, aarch64, with four
CPUs and 15 GiB RAM. Clang and Clang++ were 21.1.8, Ninja 1.13.2 and mold
2.30.0. Boost.JSON and Boost.Container 1.92.0 were built as static archives
from the official Boost source. ICU 78.3 and SQLite 3.54.0 were staged from
upstream sources. SQLite was configured with `SQLITE_DQS=3`, and the driver
verified the runtime version inside the fixture boundary. The remaining
libraries were OpenSSL 3.0.13, zlib 1.3, Brotli 1.1.0 and Zstd 1.5.5.

### Release and sanitizer commands

The external scratch directory was `/var/tmp/cppgw-integrate-x1.rLL59iFs`.
Its `ci-env.sh` runs each command with `env -i`, an empty home and a fresh
temporary directory. The driver creates independent network, mount and PID
namespaces through `sudo -n` and gives each binary a fresh home, temporary
directory and working directory. Network fixtures use loopback mocks.

```bash
scratch=/var/tmp/cppgw-integrate-x1.rLL59iFs
for shard in 1 2 3 4; do
  bash "$scratch/ci-env.sh" bash cpp/gateway/tools/ci-build.sh --config release --shard "$shard/4"
done
bash "$scratch/ci-env.sh" python3 -B "$scratch/asan-focus.py"
python3 -B "$scratch/audit-final.py" "$scratch" 20cad4cf
```

The wrapper sets the following dependency paths alongside `CC=clang-21`,
`CXX=clang++-21`, `UOS_CI_JOBS=4`, and `UOS_CI_BUILD_DIR=$scratch/ci`.

```bash
UOS_CI_SQLITE_PREFIX="$scratch/sqlite"
PKG_CONFIG_PATH="$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig"
UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0 -isystem $scratch/icu78/include"
UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib"
```

The selector called the same driver's `main()` with `--config asan`. Its
explicit additions to the HTTP dependency closure were the following tests.

```text
claude_code_models_differential claude_code_usage_schema claude_code_usage
codex_banked_reset_provider_differential codex_external_auth_differential codex_native_auth
kv_conversion_profile_json_differential legacy/runtime_http_llhttp lithos_wiring images
permissions_linux_reduced permissions_linux request_delivery_lifecycle request_differential
request_policy_differential runtime_native_startup
rt/base64 rt/console rt/date rt/env rt/fs rt/headers rt/http rt/json rt/string
rt/text_codec rt/url_idna rt/websocket
```

The seven portable driver controls passed in Debug and in ASan in 13.981
and 13.601 seconds, respectively. The registry validator resolved 445
test closures, and CMake generated all 445 CTests with zero skipped.

```bash
bash "$scratch/ci-env.sh" python3 -B cpp/gateway/cmake/ci/test_driver.py
bash "$scratch/ci-env.sh" env UOS_CI_SELFTEST_CONFIG=asan python3 -B cpp/gateway/cmake/ci/test_driver.py
python3 -B cpp/gateway/tools/links.py --check
python3 -B cpp/gateway/tools/vendor_sources.py cmake
git diff --check 30e53aa7..HEAD
```

## Portable packaging

The portable Docker build produced an 83,123,797-byte ARM64 archive with
SHA-256 `5933dd26713c5dd25e0366abea2836b8329894a07c56f235a6b7a35a886cfa6b`
at `20cad4cf`. Its sealed build profile includes llhttp 9.4.4. The Debian 12
and 13 proofs passed with two CPUs at 256 and 512 MiB. Each run completed
1,161 loopback mock chats and read 1,161 accounted requests after reopen.
Cgroup peaks ranged from 38,162,432 to 39,006,208 bytes with zero OOM events.

```bash
bash "$scratch/ci-env.sh" env PORTABLE_JOBS=4 bash cpp/gateway/packaging/portable/build.sh "$scratch/gateway-arm64.tar.gz"
bash "$scratch/ci-env.sh" bash cpp/gateway/packaging/portable/verify.sh "$scratch/gateway-arm64.tar.gz" "$scratch/portable-proof"
```

## Handoff status

The Release, sanitizer and portable package runs above finished. Standalone
fuzz verification is pending. The retry showed that Clang appends
`LIBRARY_PATH` entries after its system paths. The standalone runner needs
an explicit first-priority linker search path for staged ICU. The macOS
script passed shell and embedded Python syntax checks. Apple Silicon
execution requires a Darwin host, and a physical Pi was outside this
runner's verification.

The GitHub push preflight for
`agent-jobs/cppgw-integrate-x1-20261118-44302040764-1` exited 128 because
HTTPS authentication was unavailable. Fetched `origin/development` was
`2ac5aae69b0f0bac77cf1c6f329995cf1187616c`; the port branch is outside that
history. `agent-job` holds the two merges for the maintainer's integration.

```bash
git fetch origin development
GIT_TERMINAL_PROMPT=0 git -c core.askPass=/bin/false push --dry-run origin \
  HEAD:refs/heads/agent-jobs/cppgw-integrate-x1-20261118-44302040764-1
```
