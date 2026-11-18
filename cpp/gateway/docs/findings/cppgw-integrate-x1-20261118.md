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

Changes are scoped to integration under `cpp/gateway/` and the compiler
selection in `.github/workflows/cpp-gateway-ci.yml`. Preserve wire formats,
status codes, stored data, authentication, validation and accounting. Retain
the exact upstream vendor bytes.

Acceptance is four passing Linux ARM64 Release shards from
`bash cpp/gateway/tools/ci-build.sh --config release --shard N/4`, focused
`--config asan` coverage for the conflicts, and a clean stage branch with both
merges and exact receipts. Dependencies and build output stay under runner
scratch. Test processes have a fresh home and temporary directory, a
credential-free environment and the driver's real loopback-only fixture
boundary. The conflict inventory, compilation and fixture results were open at
discovery. Prepare dependencies while merging. Build and reconcile shard 1
before running the remaining shards and the focused sanitizer selection.

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
The Codex refresh vectors keep the original inputs and reject legacy dates
outside the ISO/IMF grammar, including `"1"`, `"Nov 16 2026"` and
`" 2026-11-16 "`. The Claude schema vectors retain the raw reset text
`"0"` and expect an absent parsed deadline. The HTTP vectors retain their
inputs and expect a 400 for invalid framing or field syntax and a 431 for
exceeding the request-head bound. Every test binary from either input branch
remains in the Release inventory. [Stage 1 fixture decisions](cppgw-integrate-s1-20261118.md#build-and-fixture-reconciliation)

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
all three libraries' source and license hashes. The unfiltered whitespace
check reports 147 lines in generated upstream `llhttp.c`. The first-party
diff passes its whitespace check.

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
staged headers and libraries. Clang appended `LIBRARY_PATH` after the
system directories, so `902d71bc` passes `LDFLAGS` to the standalone
fuzz link command before the library names.

## Linux ARM64 release and sanitizer results

The final build-input commit was `902d71bc648041199b8f867e562175992b5f8c38`,
tree `f1c8f227b847fb5b2c1f28f05801801911806fe9`. All five driver receipts
have input SHA-256
`4487834eff106fc599066c9688b3f5231e9a6df888d521efe898af1aac6d3aa0`, equal
before and after each run. Release covered the 445 sorted tests exactly
once across the four shards. All test binaries linked and ran. Each
configuration built or reused 369 production objects, including 44 runtime
objects, plus the three vendor archives.

| Configuration | Binaries passed | Cases passed | Object phase (s) | Link and run (s) | Total wall (s) |
| ------------- | --------------: | -----------: | ---------------: | ---------------: | -------------: |
| Release 1/4   |             112 |        1,311 |            0.095 |           34.878 |         35.745 |
| Release 2/4   |             111 |        1,125 |            0.011 |           29.459 |         30.860 |
| Release 3/4   |             111 |        1,455 |            0.017 |           36.834 |         37.608 |
| Release 4/4   |             111 |        1,163 |            0.017 |           43.180 |         44.227 |
| Release total |             445 |        5,054 |                  |                  |        148.440 |
| Focused ASan  |             113 |        1,280 |            0.009 |          143.085 |        143.765 |

There were zero failures, build failures, or timeouts in these runs. The
sanitizer profile enabled AddressSanitizer, UndefinedBehaviorSanitizer and
LeakSanitizer. Selection included every test whose `tools/links.py` closure
contains `src/runtime/http.cpp`, plus the runtime helper and codec tests,
application date-sensitive fixtures, request and delivery fixtures, and
startup and permissions tests listed below. The two permissions binaries
passed five cases each, the standalone llhttp binary passed seven, and
startup passed 18 in both configurations. The reduced fixture applied real
rulesets capped at ABIs 3–8 on the ABI 8 host. It verified that an ABI 3
child and its later worker thread both got `EACCES` on out-of-grant writes
and that the TCP port rules applied at ABI 4 and newer.

The final timings are incremental. Before that check, the interrupted
sanitizer build left 285 completed Ninja edges. Compiling the remaining 219
edges took 804.911 seconds. The resumed run linked and ran the 113 binaries
in 88.705 seconds and passed 1,280 cases, taking 894.167 seconds overall.

| File under the runner scratch directory | Receipt SHA-256                                                    |
| --------------------------------------- | ------------------------------------------------------------------ |
| `receipts/handoff/release-1.json`       | `313a69027edc450f54f57aea73577d4a71008ea2736f19ea18a2a0b3bd41c3c3` |
| `receipts/handoff/release-2.json`       | `3217b1559935ec46e9dec0adf29c33d4c33d362c6ed3931bf1e2f5057a8c35aa` |
| `receipts/handoff/release-3.json`       | `81eeab993674df2da4199d5e897a97d830e9283481af328fa47649eeb2c9130c` |
| `receipts/handoff/release-4.json`       | `e52715dd0985c050b460d3b3fb7971ff3ddac84011bb6775850e3b9879ffba30` |
| `receipts/handoff/asan.json`            | `607bd7e23d2bf718aa7a3bb67df8ae1d64b57b6e3e327a11e0577197ef5bfaf8` |

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
python3 -B "$scratch/audit-handoff.py" "$scratch" 902d71bc
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
Actionlint passed for the merged workflow.

```bash
bash "$scratch/ci-env.sh" python3 -B cpp/gateway/cmake/ci/test_driver.py
bash "$scratch/ci-env.sh" env UOS_CI_SELFTEST_CONFIG=asan python3 -B cpp/gateway/cmake/ci/test_driver.py
python3 -B cpp/gateway/tools/links.py --check
python3 -B cpp/gateway/tools/vendor_sources.py cmake
"$scratch/bin/actionlint" -config-file "$scratch/actionlint.yaml" .github/workflows/cpp-gateway-ci.yml
git diff --check 30e53aa7..HEAD -- . ':!cpp/gateway/third_party'
```

## Request-path benchmark build wiring

The benchmark generator produced a Ninja graph with 369 production sources
and all three vendor libraries. Each of the gateway and two offline CLI targets
links `libuos_vendor_llhttp.a`. GCC 14 compiled the three C11 objects and
G++ 14 compiled `runtime/http.cpp` with fatal warnings. A relocatable link
resolved the HTTP object's llhttp entry points from the generated archive.
The full benchmark and its throughput measurements remain unrun on the
integrated tree.

```bash
bench=$(cat "$scratch/bench-closure.dir")
# The scratch tools/ directory contains a wrapper that adds -n to Ninja.
bash "$scratch/ci-env.sh" env PATH="$bench/tools:/usr/local/bin:/usr/bin:/bin" \
  python3 -I cpp/gateway/bench/request_path/build.py --source "$PWD" --out "$bench/build" \
  --boost "$scratch/boost_1_92_0" --cxx g++-14 --cc gcc-14 --jobs 4
bash "$scratch/ci-env.sh" ninja -C "$bench/build" -j 4 libuos_vendor_llhttp.a obj/src/runtime/http.cpp.o
bash "$scratch/ci-env.sh" g++-14 -r "$bench/build/obj/src/runtime/http.cpp.o" \
  "$bench/build/libuos_vendor_llhttp.a" -o "$bench/http-llhttp.o"
nm --defined-only "$bench/http-llhttp.o" | rg ' T llhttp_(execute|init|reset)$'
rg --count '^build uos_\w+: link .*libuos_vendor_llhttp\.a' "$bench/build/build.ninja"
```

## Fuzz build paths

The standalone runner linked against staged ICU 78 and completed both
HTTP smoke campaigns with four workers. The classifier recorded one
known ARM64 libFuzzer fork-parent exit leak per campaign. Each produced
an empty artifact after its workers exited successfully. Both campaigns
reported zero input findings and the driver exited zero.

| Target                | Compiling and linking (s) | Campaign (s) | Reported fuzzing (s) | Corpus files |
| --------------------- | ------------------------: | -----------: | -------------------: | -----------: |
| `http_request`        |                   187.145 |       11.332 |                   11 |          331 |
| `http_server_library` |                   139.881 |       10.628 |                   10 |          474 |

The standalone commands used the following settings. The receipts are
`fuzz-handoff-http_request/summary.json` and
`fuzz-handoff-http_server_library/summary.json` under scratch.

```bash
for target in http_request http_server_library; do
  out=$(mktemp -d "$scratch/fuzz-$target.XXXXXXXX")
  mkdir "$out/home" "$out/tmp"
  sudo -n unshare --net -- setpriv --reuid="$(id -u)" --regid="$(id -g)" --clear-groups --no-new-privs \
    env -i PATH=/usr/lib/llvm-21/bin:/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$out/home" TMPDIR="$out/tmp" \
    CC=clang-21 CXX=clang++-21 CPLUS_INCLUDE_PATH="$scratch/boost_1_92_0:$scratch/icu78/include:$scratch/sqlite/include" \
    LDFLAGS="-L$scratch/icu78/lib -L$scratch/sqlite/lib -L$scratch/boost_1_92_0/stage/lib" \
    LD_LIBRARY_PATH="$scratch/icu78/lib:$scratch/sqlite/lib" \
    python3 -B cpp/gateway/fuzz/ci.py "$target" --seconds 10 --jobs 4 --out "$out"
done
```

The CMake sanitizer build linked 10 fuzz harnesses. All 10 completed
1,000 serial runs each with ASan, UBSan and leak detection enabled.
This exercised the helper harnesses, both HTTP link closures, and the
existing Base64, ICU and WebSocket registrations.

```bash
harnesses=(base64 console_format date_parse env_file headers_library http_request http_server_library
           posix_path string_icu_library websocket_library)
targets=()
for harness in "${harnesses[@]}"; do targets+=("uos_fuzz_$harness"); done
bash "$scratch/ci-env.sh" env UOS_GATEWAY_BUILD_EVIDENCE="$scratch/cmake-check" UOS_GATEWAY_TOOL_ROOT="$scratch" \
  cmake --build "$scratch/cmake-check" --parallel 4 --target "${targets[@]}"
for harness in "${harnesses[@]}"; do
  out=$(mktemp -d "$scratch/cmake-x1-$harness.XXXXXXXX")
  mkdir "$out/home" "$out/tmp" "$out/corpus"
  cp -- cpp/gateway/fuzz/corpus/"$harness"/* "$out/corpus/"
  sudo -n unshare --net -- setpriv --reuid="$(id -u)" --regid="$(id -g)" --clear-groups --no-new-privs \
    env -i PATH=/usr/lib/llvm-21/bin:/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$out/home" TMPDIR="$out/tmp" \
    LD_LIBRARY_PATH="$scratch/icu78/lib:$scratch/sqlite/lib" ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "$scratch/cmake-check/uos_fuzz_$harness" \
    -runs=1000 -max_len=4096 -timeout=10 -rss_limit_mb=1024 -artifact_prefix="$out/" "$out/corpus"
done
```

## Portable packaging

The portable Docker build produced an 83,123,798-byte ARM64 archive with
SHA-256 `a36592e8cf4bb8938be5b492a07947cea5b8855bfb0abffc914d5afff5dcc0c0`
at `902d71bc`. Its sealed build profile includes llhttp 9.4.4. Debian 12
and 13 each passed the two-CPU checks at 256 and 512 MiB on the ABI 8
Linux host. Each run returned 401 for an unauthenticated chat, completed
1,161 loopback mock chats and read 1,161 accounted requests after reopen.
Cgroup peaks ranged from 38,014,976 to 38,690,816 bytes with zero OOM events.

```bash
bash "$scratch/ci-env.sh" env PORTABLE_JOBS=4 bash cpp/gateway/packaging/portable/build.sh "$scratch/gateway-arm64-handoff.tar.gz"
bash "$scratch/ci-env.sh" bash cpp/gateway/packaging/portable/verify.sh \
  "$scratch/gateway-arm64-handoff.tar.gz" "$scratch/portable-handoff-proof"
```

## Handoff status

The macOS script passed shell and embedded Python syntax checks. Native
Apple Silicon execution, the owner-installed native and comparison SDK
profiles, and physical Raspberry Pi startup still need their target
environments. The reduced rulesets were exercised by ABI caps on the
Linux 7.0 runner.

The portable mock proof continued to report `unknown` for the release
identity. The operator kit tracks the separate live identity gate.
[Stage 2a findings](cppgw-integrate-s2a-20261118.md#remaining-work-and-findings)

The GitHub push preflight for
`agent-jobs/cppgw-integrate-x1-20261118-44302040764-1` exited 128 because
HTTPS authentication was unavailable. The fetch returned
`origin/development` at `2ac5aae69b0f0bac77cf1c6f329995cf1187616c`. The
integration tip is outside that history. `agent-job` holds the two merges
for the maintainer's integration. The Git bundle in runner scratch preserves
that branch using the stage 2a history as its prerequisite. The final report
commit changes only this Markdown file. Its isolated `format:check` and
`git diff --check` passed, and the driver input digest matches the five
receipts above.

```bash
git fetch --no-tags origin refs/heads/development:refs/remotes/origin/development
GIT_TERMINAL_PROMPT=0 git -c core.askPass=/bin/false push --dry-run origin \
  HEAD:refs/heads/agent-jobs/cppgw-integrate-x1-20261118-44302040764-1
git bundle create "$scratch/cppgw-integrate-x1-20261118.bundle" agent-job ^30e53aa7
git bundle verify "$scratch/cppgw-integrate-x1-20261118.bundle"
```

**Summary.** Stage 1 and the opt-in Landlock fallback are merged in `a05c6088` and `88d62fe3`.
Linux ARM64 Release passed all four shards, totaling 445 binaries and 5,054 cases.
Focused ASan, UBSan and LeakSanitizer passed 113 binaries and 1,280 cases.
The default Landlock policy and seccomp filter match stage X byte for byte.
GitHub HTTPS authentication blocked publication. The committed integration is on `agent-job`.
