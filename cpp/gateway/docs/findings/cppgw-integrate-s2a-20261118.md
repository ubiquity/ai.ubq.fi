# C++ gateway stage 2a integration

## Work contract v1

Integrate the seven accepted branch tips in the requested order on the suite repair at
`33ea9b20d1f02b96bb86a24c0530641ccd4fd525`. Preserve wire formats, status codes, storage,
authentication, validation, resource limits, accounting, and real fixture confinement.
Keep the standard-library behavior accepted by each branch. Use Boost.Beast for the outbound
WebSocket operations it replaces, ICU for casing and collation, and simdutf for UTF conversion.
Terminal logging must preserve the existing record formats and request settlement.

The integration owns `cpp/gateway/` and the C++ gateway workflows. TypeScript, other C++ projects,
vendored bytes, production services, live data, and credentials are outside this stage. Tests use
local mocks, a sparse environment, and private temporary state. The macOS, Raspberry Pi, and
operator branches supply build and handoff artifacts. Physical host qualification is a separate step.

Acceptance requires all four portable release shards on Linux ARM64, focused ASan/UBSan coverage
of the merged components, CMake and vendor-registry checks, and a driver/CI receipt for the pinned
SQLite headers and loaded library. The merge graph must retain the seven incoming tips. The report
records the exact commands, tree, counts and unresolved items for the maintainer's next integration.

Discovery confirmed shared ICU ancestry on the logging, macOS, Pi, operator and audit lines. The
runner was Ubuntu 24.04.5 LTS on ARM64, with four Neoverse-N2 CPUs and 15 GiB of usable memory.
Its distribution SQLite and ICU are older than the suite fixtures expect. The integration uses
the versions qualified below. The maintainer owns the merge with the next integration stage.

## Merge order

| Order | Change                                        | Incoming commit                            | Merge commit                               |
| ----: | --------------------------------------------- | ------------------------------------------ | ------------------------------------------ |
|     1 | Unified release, sanitizer, and robustness CI | `156a3b8e635aa02d2ffca8d9d7331cea5096e208` | `c1471aa448c16101837a1e22b6c6a76834ffa50d` |
|     2 | Terminal log records and shared ICU ancestry  | `436369dbdf7ab6ae2d65e4637ca2c0b5ffebc4a8` | `2dd580031aa975a6a87825d711233a3170577d07` |
|     3 | Boost.Beast outbound WebSocket                | `e030bdd61e1f8081cd64a6ceac8e9b3c76b64f09` | `cf5538f7311e1cab476afe58ad7403538c1340b3` |
|     4 | macOS requalification and ICU strings         | `33f2cd3356e6a27a4daa111473ab1aff8633f63b` | `850360b7939de2838f44b02138b5071850df3648` |
|     5 | Raspberry Pi packaging                        | `da56e2277e0b0ef6ee0da3b3f504dfea16069bd2` | `c5c67a830cdeed1d81dec4d1d5094eeb05b227eb` |
|     6 | Side-by-side operator kit                     | `bb167116d55d91259e7eb62b7dabdb55863309e0` | `0a4cf2e757fb8c1f5847410e2e4f5ace0631eb5c` |
|     7 | Runtime audit and briefs                      | `d0c8d551bbb6334300c930bda29eb4f8e4e93632` | `e4c370babf313be9ae331f15a3de78f624c64b53` |

All merges used `git merge --no-ff` with the fetched tip as the second parent. Git reported three conflicts.

- Merge 2 added a second `docs/DEVIATIONS-FROM-BUN.md`. The integrated document keeps the
  repaired JSON and encoding notes and adds the ICU case mapping and collation examples.
- Merge 3 added a third version of the deviations document. It now includes the Beast UTF-8
  validation rule and close-code example alongside the previous sections.
- Merge 4 conflicted in the opening history and dependency table in `docs/MACOS.md`. The table
  describes the newer requalification and the November 17 counts, zlib version and floating-point
  libc++ probe remain attached to the earlier receipt.

Git combined the two `src/rt/string.cpp` changes automatically during merge 2. The ICU branch
replaced the generated case/collation tables while the repaired head's simdutf UTF scans and
transcoders stayed in place. Both fuzz targets joined the existing registry. The CMake SDK ICU
binding, vendor consumer map, test closures and unified CI documentation also merged cleanly.
The incoming ICU ancestry was already present when its macOS, Pi, operator and audit descendants
were merged. The base and final `third_party/` trees are byte-identical.

## Build follow-ups

- `3e762a71` removes SQLite from the portable suite's `pkg-config` lookup. The workflow and
  driver use `/usr/local/include/sqlite3.h`, `/usr/local/lib/libsqlite3.so` and the library
  directory's ELF RPATH. The version probe uses the suite's compile/link flags and makes the
  `sqlite3_libversion()` call inside the ordinary fixture namespace. Its compile-time pin and
  runtime check both require 3.54.0. The probe logs the loaded version before the suite starts.
- `0d1a070b` marks tree-relative vendor includes as system headers in the common Clang CMake
  settings. The Pi hook covered only `base64.cpp`; the shared setting also covers the simdutf
  consumers in `string.cpp` and `text_codec.cpp`. Both new robustness harnesses have native
  CMake targets, and the same pinned vendor registry supplies the archives.

## Test decisions

All inherited test binaries and assertions passed as written. The unified CI branch retires the
sanitizer selection list and runs the same complete registry as Release. The ICU and Beast branches
add two binaries with eight cases, bringing the repaired baseline from 441 binaries and 5,031 cases
to 443 and 5,039. The new WebSocket cases cover valid handshakes, rejection of malformed tokens and
UTF-8 (1007), and the route-defined close on an over-limit continuation header. The existing client
coverage still exercises custom headers, TCP/TLS and Unix sockets. [Runtime deviations](../DEVIATIONS-FROM-BUN.md)

`tests/rt/sqlite_test.cpp` passed all 29 cases under Release and ASan. The native fixtures still
require decimal text `2251799813685248.0` for the version-sensitive REAL boundary and `SQLITE_NOTADB`
on a simple query against the garbage file. A comparison relinked the same Release objects with
`/usr/lib/aarch64-linux-gnu/libsqlite3.so.0` and put that directory first in ELF RPATH. Under the
same fixture boundary, the version probe printed `3.45.1` and rejected it. The test binary then
reproduced 26 passes and three failures in the open, binding and version cases. The comparison
logs are in `sqlite-distro-control.cACIM5qf/` under the runner scratch directory.

## Linux ARM64 verification

Clang 21.1.8 came from apt.llvm.org. Boost 1.92.0 JSON and Container were built as static libraries
in scratch. SQLite 3.54.0 used the archive and `-DSQLITE_DQS=3` recipe from the workflow. ICU 78.3
was built from upstream and installed under `/usr/local`. The runner used OpenSSL 3.0.13, zlib 1.3,
Brotli 1.1.0 and Zstd 1.5.5. These are the prepared source archive digests.

| Archive                          | SHA-256                                                            |
| -------------------------------- | ------------------------------------------------------------------ |
| Boost 1.92.0 source              | `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` |
| `sqlite-autoconf-3540000.tar.gz` | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` |
| ICU 78.3 source                  | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` |

The acceptance runs used commit `0d1a070b6d3c4aaa2fb049dc8e1673e4c5e88951`, tree
`2fce48e5ddfea283c8c2ead1883fc3e1bc932261`, and driver input digest
`3c1b11aa6e43856eebde8934baaf203aa878372ed0df5317ddaa6c67f24e9dd2`. Each receipt records the same
input digest before and after. Each shard found 369/369 production objects and logged
`sqlite3_libversion(): 3.54.0`. All 443 test paths appear exactly once across the four shards.
The confirmed Release runs reused compiled objects and freshly linked and ran the test binaries.

| Check                        | Binaries passed | Binaries failed | Cases passed | Cases failed |    Wall time |
| ---------------------------- | --------------: | --------------: | -----------: | -----------: | -----------: |
| Release 1/4                  |             111 |               0 |        1,328 |            0 |     17.735 s |
| Release 2/4                  |             111 |               0 |        1,295 |            0 |     35.922 s |
| Release 3/4                  |             111 |               0 |        1,339 |            0 |     21.425 s |
| Release 4/4                  |             110 |               0 |        1,077 |            0 |     10.978 s |
| **Release total**            |         **443** |           **0** |    **5,039** |        **0** | **86.060 s** |
| Focused ASan, UBSan and LSan |              67 |               0 |          805 |            0 |  2,720.441 s |

The sanitizer build compiled its objects in 2,624.017 s. Its selection includes the 50 tests whose
resolved links reach either logging change, plus the relevant runtime and transport fixtures.
The seven driver controls also passed with the real fixture namespaces. All suite binaries ran with
loopback-only networking, a private `/tmp`, an empty home and a credential-free environment.

```bash
scratch=/tmp/cppgw-s2a.uiGciqTD
for shard in 1 2 3 4; do
  out="$scratch/release-confirmed"
  mkdir -p "$out/home-$shard" "$out/tmp-$shard"
  env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 TZ=UTC \
    HOME="$out/home-$shard" TMPDIR="$out/tmp-$shard" \
    CXX=clang++-21 UOS_CI_JOBS=4 UOS_CI_BUILD_DIR="$scratch/release" UOS_CI_SQLITE_PREFIX=/usr/local \
    UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0" UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -licudata" \
    PKG_CONFIG_PATH=/usr/local/lib/pkgconfig \
    bash cpp/gateway/tools/ci-build.sh --config release --shard "$shard/4"
done
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 TZ=UTC HOME="$scratch/asan-home" TMPDIR="$scratch/asan-tmp" \
  CXX=clang++-21 UOS_CI_JOBS=4 UOS_CI_BUILD_DIR="$scratch/asan" UOS_CI_SQLITE_PREFIX=/usr/local \
  UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0" UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -licudata" \
  PKG_CONFIG_PATH=/usr/local/lib/pkgconfig python3 -B "$scratch/run-asan.py"
```

The scratch selector calls `driver.main()` with `--config asan`. It filters `links.all_tests()` to
closures containing `src/handler/terminal_log.cpp` or `src/handler/serve_handler.cpp`, plus these
test paths, listed without `tests/` and `_test.cpp`.

```text
legacy/string_icu legacy/websocket_library rt/sqlite rt/kv_sqlite rt/base64 rt/string rt/text_codec
rt/url_idna rt/websocket rt/websocket_lifetime runtime_websocket runtime_http runtime_http_lifetime
live_sideband_http codex_supervisor codex_native_auth codex_external_native_auth permissions_linux
runtime_native_startup serve serve_delivery_http
```

| Receipt under the scratch directory              | SHA-256                                                            |
| ------------------------------------------------ | ------------------------------------------------------------------ |
| `release-confirmed/release-1.json`               | `6d69a42bedf5341da28f69b89e76871e08d5266a019b4cedfed572881a860b61` |
| `release-confirmed/release-2.json`               | `a462948b5df6b28ab49289d6463eb26d1d0c192cc50b0944165ab5005824a24c` |
| `release-confirmed/release-3.json`               | `28b6f3a30fa6e6553581293bcd01e712497ba94e322e5f9267c2974d944f7f39` |
| `release-confirmed/release-4.json`               | `f6e816bea570327ec082e2ad88732a96e887faf6d0a9f5775f55ebc4de0137ac` |
| `asan/clang++-21-2b8d836a9ec8/asan/receipt.json` | `eade74bbd5049852f08af35e4ff4719e832669caf70f3321dc571e34d20a3b43` |

## Build, package and operator checks

- `python3 -B cpp/gateway/tools/links.py --check` resolves all 443 tests. The `cmake` mode of
  `tools/vendor_sources.py` emits both archive targets. Its `unit` mode selects simdjson for
  `src/rt/json.cpp` and simdutf for Base64, string and text codec consumers. Both modes verify the
  vendored file hashes before producing the bindings.
- CMake configured all 443 CTests and linked the two new harnesses. Each CMake-built
  binary completed 10,000 libFuzzer runs. The unified CI runner also completed four-worker,
  30-second campaigns for `string_icu_library` and `websocket_library`. Actual fork durations
  were 32 s and 33 s, with zero findings. Each logged one empty artifact for the known ARM64
  libFuzzer fork-parent exit leak, which the existing classifier records separately.
- The portable ARM64 build produced an 83,044,176-byte archive with SHA-256
  `118652d165355f769bcff5bb7ba4dd012eee730831b91f1c5a4c67c7fdb7daca`. The package proof
  started and exercised the same binary on Debian 12 and 13 with 512 MiB and 256 MiB caps.
  Each of the four runs completed 1,161 local mock requests and read back 1,161 accounted requests
  after reopen. Measured cgroup peaks ranged from 37,695,488 to 38,760,448 bytes with zero OOM events.
- The operator replay matched three of three `GET /health`, `GET /openapi.json` and `HEAD /openapi.json`
  pairs against separate loopback mocks.

```bash
python3 -B cpp/gateway/tools/links.py --check
python3 -B cpp/gateway/tools/vendor_sources.py cmake
python3 -B cpp/gateway/tools/vendor_sources.py unit src/rt/json.cpp src/rt/base64.cpp src/rt/string.cpp src/rt/text_codec.cpp
cmake --build "$scratch/cmake-fuzz/build" --parallel 4 --target uos_fuzz_string_icu_library uos_fuzz_websocket_library
bash cpp/gateway/packaging/portable/build.sh "$scratch/portable/gateway-arm64.tar.gz"
bash cpp/gateway/packaging/portable/verify.sh "$scratch/portable/gateway-arm64.tar.gz" "$scratch/portable/proof"
git diff --check 33ea9b20..HEAD
```

The CMake test and fuzz configuration used Clang 21.1.8, `-DUOS_GATEWAY_TESTS=ON`,
`-DUOS_GATEWAY_FUZZ=ON`, and `-DUOS_GATEWAY_SANITIZER=address,undefined`. The packaging proof
ran both runtime images with Docker's network disabled. The portable suite, CMake and packaging
receipts identify the tested code tree above. The handoff's subsequent edits are documentation.

Workflow syntax passed `actionlint` with the supported ARM64 runner label. Bash syntax checks
passed for the portable suite, macOS, replay, and portable packaging entry points. The seven
namespace driver controls passed in 23.566 s under `UOS_CI_SELFTEST_CONFIG=asan`.

## First-party runtime lines

Tracked physical lines include `.cpp` and private `.hpp` files, comments and blank lines. The count
uses `git ls-tree -r --name-only` for each revision and `wc -l` on each `git show "$revision:$path"`.

| Path          | Repaired base | Stage 2a |  Delta |
| ------------- | ------------: | -------: | -----: |
| `src/rt`      |        30,622 |   29,135 | −1,487 |
| `src/runtime` |         2,966 |    2,966 |      0 |
| Total         |        33,588 |   32,101 | −1,487 |

## Remaining work and findings

- Exact-tree macOS execution, physical Raspberry Pi measurements and hosted CI receipts remain
  pending. This ARM64 runner provides Linux, Debian container and focused sanitizer evidence.
- The portable package's mock proof reports `unknown` for the gateway release identity. The
  release owner must bind the real SHA and deployment ID before operational acceptance.
  [Operator handoff](cppgw-ops-side-by-side-20261118.md#findings-and-handoff)
- The WebSocket adapter enforces the configured route budgets. The shared 16 MiB generic default
  and 60-second post-upgrade idle deadline remain follow-up work. [Runtime limits](../RUNTIME.md#input-limits)
- `GIT_TERMINAL_PROMPT=0 git push --dry-run origin HEAD:refs/heads/agent-jobs/cppgw-integrate-s2a-20261118`
  reached GitHub and returned 128 because the runner has no GitHub credentials. The local
  integration tip preserves all seven merge commits for maintainer collection.

## Summary

All seven merges are in order. On Linux ARM64 the four Release shards passed 443 binaries and
5,039 cases. Focused ASan, UBSan and LSan passed 67 binaries and 805 cases. The driver loaded
SQLite 3.54.0, and the first-party runtime shrank from 33,588 to 32,101 lines. CMake, Debian ARM64
packaging and mock operator replay passed. Hosted CI and hardware qualification remain pending.
