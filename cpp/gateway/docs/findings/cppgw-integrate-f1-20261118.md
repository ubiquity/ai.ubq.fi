# C++ gateway final integration F1

## Work contract v1

Start at cross-line tip `6d28e858e7ab8b8b1036db4a4adfdade0465ed40`. Merge the fetched stage 2b, macOS release, and
README/architecture tips in that order as ordinary two-parent merges. Build and test each integrated tree before the
next merge. Record textual conflicts, semantic reconciliations, and expectation changes as they are resolved.

| Surface                  | Integration decision                                                                                                                                                                        |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| JSON and text            | One simdjson JSON parser with the stage 2b depth cap, the shared input limits, simdutf text and ICU Unicode handling.                                                                       |
| HTTP and WebSocket       | llhttp for the inbound HTTP and outbound fetch boundaries. Keep Beast's WebSocket client and handshake, and preserve close, framing and delivery decisions across supported Boost versions. |
| Runtime and storage      | Take the stage 2b helper slimming, Ada, QCBOR, OpenSSL, Boost.Multiprecision and sparse/cyclic KV changes. Preserve strict input validation, the stored codec and graph bounds.             |
| Gateway behavior         | Preserve wire, status, authentication, validation, quota and accounting contracts. Compare packaged native and Bun gateways with the same local mocks.                                      |
| Dependencies and release | Register each vendored library once in every build and packaging path. Preserve vendor bytes. Retain the default Linux confinement requirement and the explicit reduced-confinement opt-in. |

Edits are confined to `cpp/gateway/` and the gateway workflows as needed. TypeScript, other `cpp/` paths, credentials,
production service state, and live provider traffic are out of scope. Test processes use empty homes, fresh temporary
directories, a credential-free environment and local mocks through the existing OS fixture boundaries. Dependencies,
downloads and build products stay outside the checkout.

The starting evidence is 445/445 Release binaries and 5,054/5,054 cases on cross-line, and 448/448 and 5,086/5,086
on stage 2b. Stage 2b reports full paired parity with two shared validation failures. Conflicts, final inventory, the
WebSocket Boost discrepancy and the target-specific package results remain open at discovery.

Acceptance covers all four Linux ARM64 Release shards with `bash cpp/gateway/tools/ci-build.sh --config release
--shard N/4`, focused ASan/UBSan/LSan coverage, `python3 cpp/gateway/tools/links.py --check`, paired packaged
conformance, the portable package verifier, and the workflow's `kv_jsc` fuzz command for at least 600 seconds.
Capture exact Git commits and trees, vendor comparisons, case counts, timings, and production source counts. The
stop condition is a clean, verified handoff with all three ordered merges and any concrete blockers identified.

## Starting refs

| Input                   | Fetched tip                                |
| ----------------------- | ------------------------------------------ |
| Stage 2b                | `af7a0036b823ef2357c26b9b5f03f2677796b775` |
| macOS release           | `980d97fb0c90bd6937c06d4f9e3b25bc1d52a9e1` |
| README and architecture | `20c83e02258a5435b6ebc9f16c7b927d3b111407` |

The checkout was clean on `agent-job`. The host is Ubuntu 24.04.5, Linux ARM64, with four CPUs and 15 GiB RAM.
Passwordless network namespace setup and Docker 28.0.4 are available. Bun reports 1.4.0.

## Merge 1 — stage 2b

| Git conflict                  | Resolution                                                                                                                                                           |
| ----------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `cmake/ci/driver.py`          | Combine per-vendor include flags and the explicit Ninja dependencies on every registered vendor header, including QCBOR's nested includes.                           |
| `cmake/vendors.json`          | Keep a single llhttp entry for server, fetch and WebSocket response-head consumers. Add Ada and QCBOR. Keep all three simdutf consumers.                             |
| `docs/DEVIATIONS-FROM-BUN.md` | Keep the cross-line deviations and add the runtime, library and depth decisions from stage 2b. Describe the Beast and llhttp boundaries.                             |
| `fuzz/ci.py`                  | Use the same verified vendor registry as the other build paths. Keep the matching C frontend, library link flags, timed campaign receipt and finding classification. |
| `tests/rt/json_test.cpp`      | Keep cross-line's document-size and array/object depth rejection cases. Keep stage 2b's value-copy coverage and exercise both arrays and objects.                    |

The automatic WebSocket client merge retains Beast's handshake key and accept digest and uses the existing llhttp
response-head adapter where the socket transport needs HTTP parsing. The merged JSON parser remains simdjson.
`kJsonMaxDepth` now uses the central 128-container constant for recursive operations on JSON values. KV keeps
the separate 2,000-container graph bound and the sparse/cyclic reader. The CMake, SDK, macOS, benchmark, and
fuzz build paths use the selected vendor inputs once per build closure. The standalone fuzz targets for fetch,
URL and passkey CBOR derive their vendor sources from the registry. The macOS build also tracks nested vendor
headers in the object receipts for both the sources and consumers.

The three stage 2b parser rejection assertions at depth 129 expected `RangeError` with `Maximum call stack size
exceeded.` The merged parser expects the cross-line `SyntaxError` with `Invalid JSON` for all three inputs.
The stringify, comparison and coercion tests continue to require `RangeError` when given values past the limit.
The stage 2b depth-related Codex 429, BigInt, fetch and WebAuthn fixture changes are recorded in its
[findings](cppgw-integrate-s2b-20261118.md#fixture-decisions).

The initial merge attempt `38019ea1` failed in `src/kv_conversion/proof.cpp` and `src/rt/kv_value.cpp`. Boost's
transitive `<iomanip>` include let argument-dependent lookup select `std::quoted` over both local `quoted`
helpers for `std::string` arguments. The accepted merge uses distinct local names and compiled all 369 production
objects. The stage 2b replay on the prior parser promoted integer-index keys in two generic console vectors. The merged simdjson and console path
prints both full objects in insertion order. The vectors now expect `4294967294` last (formerly first)
and `1` between `valid_id` and `with space` (formerly first). Both inputs and all fields are retained.
Pre-serialized gateway log records and terminal accounting fields use their existing paths.

| Automatically merged fixture                       | Combined coverage and expected results                                                                                                                                                                                                                                                                                                                      |
| -------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `tests/images_test.cpp`                            | Keep the cross-line oversized-body rejection and the successful inline edit above the default JSON cap, plus stage 2b's raw multipart upload coverage.                                                                                                                                                                                                      |
| `tests/native_codex_429_differential_vectors.inc`  | Keep stage 2b's 40 header-only expectations for over-depth documents and the cross-line exact raw response and classification checks.                                                                                                                                                                                                                       |
| `tests/passkeys_webauthn_differential_vectors.inc` | Keep stage 2b's strict CBOR/DER regressions and the cross-line warning for `client data with base64 plus`. The previous `JSON Parse error: Unterminated string` becomes `Invalid JSON`. HTTP 400, the body, state and upstream observations still match.                                                                                                    |
| `tests/rt/http_test.cpp`                           | Keep all 346 generated HTTP scenarios and the stage 2b wire-level replacements for the retired Blob/File and FormData helpers. The cross-line `1e400` fixture changes from `null` to `SyntaxError: Failed to parse JSON`. Stream vectors 107–113 and 116 keep their inputs and change their specific JSCore syntax messages to `SyntaxError: Invalid JSON`. |

The JSON fixture retains the cross-line numeric, insertion-order, UTF-8 and malformed-document expectations and
stage 2b's depth and copy cases. Comparing tracked `*_test.cpp` paths confirms the 450-test inventory is the
union of the two parents. The other inherited fixture changes for slimmer helpers and the selected libraries are
documented in the input reports.
[Stage 1b fixture migration](cppgw-integrate-s1b-20261118.md) · [Stage 2a](cppgw-integrate-s2a-20261118.md) ·
[Stage 2b](cppgw-integrate-s2b-20261118.md#fixture-decisions)

## WebSocket close code 1014

On ARM64 `ubuntu:26.04`, Clang 21.1.8 and distro Boost `1.90.0-6ubuntu1`, a direct
`boost::beast::websocket::detail::is_valid_close_code` probe returned false for the IANA-registered 1014
(Bad Gateway). The same call returned true with staged Boost 1.92. The 1.90 receive helper also reported an
invalid close code for a 1014 frame. The gateway now handles 1014 in its shared send and receive validator,
uses Boost.Endian to read the code, and uses Beast to validate the reason's UTF-8. The original assertion
still requires the client to enter CLOSING without throwing and send exactly one masked 1014 frame.
Two new received-frame examples check a valid 1014 reason and rejection of a malformed UTF-8 reason.

The ARM64 container reproduced the hosted failure on the original `websocket_frame.cpp` from `6d28e858`, linked
against the `7b602c71` WebSocket test and runtime closure. Both profiles reported the expected `[["close-ok",2]]`
versus the actual `[["close-threw","TypeError","First argument must be a valid error code number",2],
["second-close-ok",2]]` and zero frames. The newly added receive vectors also failed as a single harness case.
Replacing only the frame object with the current source passed all 17 cases in both Release and ASan/UBSan/LSan.
Boost 1.91's upstream header also places 1014 in its reserved branch. [Boost 1.91 validation](https://github.com/boostorg/beast/blob/boost-1.91.0/include/boost/beast/websocket/detail/frame.hpp)

The isolated container run reused the portable driver's Ninja flags, source closures and fixture boundary. It supplied
the staged SQLite to that boundary. The first container attempt hit AppArmor's nested-mount restriction. The
following launch enabled the required capabilities only inside the disposable networkless container. Its nested
test namespaces still verified loopback and outbound denial.

```bash
scratch=/var/tmp/cppgw-integrate-f1.WpF47zpW
proof="$scratch/ubuntu26-websocket"
docker run --rm --platform linux/arm64 --network=none --cap-add=SYS_ADMIN --cap-add=NET_ADMIN \
  --security-opt seccomp=unconfined --security-opt apparmor=unconfined \
  --mount "type=bind,src=$proof,dst=/proof" --mount "type=bind,src=$scratch/sqlite,dst=/sqlite,readonly" cppgw-f1-ubuntu26 \
  env -i PATH=/usr/bin:/bin LANG=C.UTF-8 HOME=/proof/home TMPDIR=/proof/tmp python3 -B /proof/check.py
```

The Release and sanitizer hexadecimal SHA-256 receipt hashes are
`bf54583c7fd0e8d6b045b90ac9deca9758581e9078013146a5cbda278cac6cd3` and
`bd47764e0f9260174f9ed42df6f5f5190a503482e628b1d261c17aedd763937d`.

## Stage 2b release gate

All four release shards passed on `7b602c71` after the console fixture correction. The counts by shard were
113/1,332, 113/1,233, 112/1,439 and 112/1,100, for 450 binaries and 5,104 cases. The receipts are under
`/var/tmp/cppgw-integrate-f1.WpF47zpW/receipts/stage2b/`.

## Merge 2 — release packaging

| Git conflict                    | Resolution                                                                                                                                                                                        |
| ------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `packaging/portable/payload.py` | Keep the release branch's timestamps, `VERSION`, native build receipt and installation files. Derive all linked vendor versions from the same registry used to collect their licenses.            |
| `tools/macos-build.sh`          | Keep expanded pkg-config system include paths, matching C/C++ compilers, system treatment for all vendor headers, and explicit header dependencies for each C and C++ vendor source and consumer. |

The Docker build context retains the llhttp consumer and adds the direct QCBOR test consumer. Portable Linux
payloads include the five linked vendor licenses and notices where supplied, SQLite's public-domain notice,
Boost and ICU licenses, Rust addon notices and the Debian copyright files for the linked toolchain and system
libraries. The macOS package uses the same vendor registry for versions and includes each linked vendor license.
The Linux release instructions describe the reduced-confinement opt-in.

The focused Release gate on the packaging merge passed 2 WebSocket binaries and all 21 cases. Shell and Python
syntax checks, the vendor registry and the Links registry passed. The Docker package gate uses this merge's
committed snapshot.

## Merge 3 — README and architecture

| Git conflict | Resolution                                                                                                                                                                                        |
| ------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| None         | Update the incoming guides with the integrated release inventory, selected libraries, sparse and cyclic KV support, default and opt-in confinement, and the macOS package's `--version` boundary. |

## Source identity and size

Each merge is an ordinary two-parent commit. `7b602c71` records the console fixture correction between the first
and second merge.

| Order | Merge commit                               | Integrated tree                            |
| ----- | ------------------------------------------ | ------------------------------------------ |
| 1     | `5a8c3ec1bf32d5710f4c372f416f32b283921983` | `6a70c8d2e9ae19a53ea20c84493b60f9e29d9b7e` |
| 2     | `483a8684a2f615ddf267393e26be2864765fbe0c` | `778f3d439bd4bf44542eb7d4714737b1a88d30c8` |
| 3     | `0c9dfda6d2abfcf3d9c9eb733585fe8db0488b28` | `1c2303aa2df265ca4560d14c2935fc1a7925c7cd` |

Count physical newlines in tracked Git blobs using `git ls-tree -r --name-only <ref> -- <path>` and
`git show <ref>:<file> | wc -l`. Tests, samples, build tooling and vendors are excluded. The macOS launcher
is reported separately because it is compiled into the package.

| First-party path                      |  Cross-line |  Integrated |              Change |
| ------------------------------------- | ----------: | ----------: | ------------------: |
| `src`                                 |     148,128 |     145,097 |              −3,031 |
| `include`                             |      39,575 |      39,480 |                 −95 |
| `runtime`                             |         554 |         554 |                   0 |
| `entrypoints`                         |          69 |          69 |                   0 |
| `packaging/macos/gateway-version.cpp` |           0 |          11 |                 +11 |
| **Total**                             | **188,326** | **185,211** | **−3,115 (−1.65%)** |

Within `src`, `src/rt` fell from 28,174 to 26,101 lines. `src/auth/passkeys.cpp` fell from 5,072 to 4,197.
The `src/runtime` implementation stays at 3,069 lines. The `llhttp`, `simdjson`, `simdutf` and retained `yyjson`
trees byte-compare with cross-line. Ada and QCBOR byte-compare with stage 2b. The registry verifies the source
and license hashes for all five linked vendors.

## Integrated Linux ARM64 release

All release shards on `0c9dfda6` recorded driver input SHA-256
`97e66e6321b955b3a5e00ac9a6297b1520d045cd462186b977dc6d64c89790b6` before and after each run.
The four selections are disjoint and cover the entire sorted 450-test Links inventory. The counts use the fixture
harness summaries. These measurements reuse 369 validated production objects, including 44 runtime units.

| Release shard |    Binaries |           Cases | Object phase (s) | Link and test (s) |    Wall (s) |
| ------------- | ----------: | --------------: | ---------------: | ----------------: | ----------: |
| 1/4           |     113/113 |     1,332/1,332 |            0.010 |            51.813 |      52.790 |
| 2/4           |     113/113 |     1,233/1,233 |            0.034 |            40.645 |      42.190 |
| 3/4           |     112/112 |     1,439/1,439 |            0.017 |            37.644 |      39.261 |
| 4/4           |     112/112 |     1,100/1,100 |            0.016 |            45.945 |      47.126 |
| **Total**     | **450/450** | **5,104/5,104** |                  |                   | **181.367** |

```bash
scratch=/var/tmp/cppgw-integrate-f1.WpF47zpW
for shard in 1 2 3 4; do
  bash "$scratch/ci-gate.sh" bash cpp/gateway/tools/ci-build.sh --config release --shard "$shard/4"
done
bash "$scratch/ci-gate.sh" bash cpp/gateway/tools/ci-build.sh --config release rt/websocket
python3 cpp/gateway/tools/links.py --check
python3 -B cpp/gateway/tools/vendor_sources.py cmake
git diff --check 6d28e858..HEAD -- . ':(exclude)cpp/gateway/third_party/**'
```

The wrapper uses `env -i` with a fresh home and temp directory. It sets `CC=clang-21`, `CXX=clang++-21`,
`UOS_CI_JOBS=4`, `UOS_CI_BUILD_DIR=$scratch/ci`, `UOS_CI_SQLITE_PREFIX=$scratch/sqlite`,
`PKG_CONFIG_PATH=$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig`,
`UOS_CI_CXXFLAGS=-isystem $scratch/boost_1_92_0` and
`UOS_CI_LDFLAGS=-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib`.
The locally staged Boost 1.92.0 static archives, ICU 78.3 and SQLite 3.54.0 with `SQLITE_DQS=3` came from the
pinned upstream source archives. The underlying Ubuntu 24.04.5 runner supplied Clang 21.1.8, OpenSSL 3.0.13,
zlib 1.3, Brotli 1.1.0 and Zstd 1.5.5. The driver independently verified real namespace isolation and SQLite's
runtime version.

| Receipt under `receipts/final/` | SHA-256                                                            |
| ------------------------------- | ------------------------------------------------------------------ |
| `release-1.json`                | `bb786db1983a80480aced736748d04f818825458b0700cb8fd1ad190c2fc5343` |
| `release-2.json`                | `22ae715ab024a186de1a0e47239dc0fe35a74869a42a8b68db083fe47f8f13ef` |
| `release-3.json`                | `f94362e465da0ab1d2afbb5930cfbbd47553694ade93e059db6529e7ccab5ffd` |
| `release-4.json`                | `fbcaf55ac8352bd5b7516a979620ea170e8c76129baf7d173bbcffe93eb0d49c` |

## Integrated Linux ARM64 sanitizers

The focused AddressSanitizer, UndefinedBehaviorSanitizer and LeakSanitizer run passed **157/157 binaries and
1,933/1,933 cases**. The selection covers all 37 runtime binaries, tests whose paths contain `passkey`, `kv`,
`http` or `websocket`, and tests whose Links closure includes `src/runtime/http.cpp`. It uses the portable
driver's real fixture boundary and stock `--config asan` compiler flags. All 157 run logs were free of sanitizer
diagnostics. The WebSocket runtime fixture passed 17/17 cases, and the JSC reader and sparse-array fixtures
passed 8/8 and 2/2. QCBOR and OpenSSL standalone tests passed 13/13 and 9/9, and the WebAuthn differential
case passed all of its vectors.

```bash
scratch=/var/tmp/cppgw-integrate-f1.WpF47zpW
out="$scratch/receipts/final"
/usr/bin/time -o "$out/asan-resumed.time" -f 'wall_s=%e peak_process_kib=%M exit=%x' \
  timeout -k 30s 1500s bash "$scratch/ci-gate.sh" python3 -B "$scratch/asan-focus.py" \
  > "$out/asan-resumed.log" 2>&1
```

The earlier connection drop left 479 of 556 Ninja edges completed. The resumed compile phase took 303.291 s,
the link and test phase took 131.864 s and the timed command took 435.94 s with 1,110,064 KiB reported peak
process RSS and exit 0. The receipt binds `35d16ce92b1c577e6bdea66ea2d03b10d7690d14`, tree
`7a913e86b13b200f85575d0dfdcdc6bf05100522`, and the same before-and-after driver input SHA-256 as Release.
The SHA-256 of `receipts/final/asan-resumed.json` is
`439f5140a42e18ce6b5a57bfe195c831612db1ea95942d5acd9e782070b373d6`.

## KV robustness

The Ubuntu 26.04 ARM64 campaign on `5a8c3ec1` used the workflow's standalone target, four workers and the
following command inside its credential-free network namespace. The reader, harness and campaign selection are
identical in the integrated release.

```bash
python3 -B cpp/gateway/fuzz/ci.py kv_jsc --seconds 600 --jobs 4 --out /output
```

The build took 182.883 s. The campaign ran for 608.301 s (608 fuzzed seconds), reached 2,009 corpus entries
and recorded no OOM or other genuine finding. libFuzzer reported 403 MiB peak RSS in its fork parent, which exited
77 with the already classified empty-input ARM64 fork-exit leak. The workflow harness classified the complete run as
passed and retained the artifact. The receipt and campaign log are under `kv-workflow.XzdtO6dU/`, with SHA-256
`bb4292544eb3f7cf8de596d9b7e8ebcb144c6b3fa07ecff565ef2b2f9534bd10` and
`7e558729f1cc954a6b4e89058b4d215891cf06e2a8d62b423bd4b9df7798b1db`.

## Portable Linux ARM64 package

Docker 28.0.4 built and verified the package from the packaging merge `483a8684a2f615ddf267393e26be2864765fbe0c`,
tree `778f3d439bd4bf44542eb7d4714737b1a88d30c8`. This commit supplies the same package inputs as the guides merge.
The build took 1,536.70 s with three build jobs, and the verifier took 65.03 s. Both exited 0. The archive is
83,898,785 bytes with SHA-256 `91370e5a44df44833326bc0898fb826194ab45bd8f392b338685704305232e18`.

```bash
scratch=/var/tmp/cppgw-integrate-f1.WpF47zpW
work="$scratch/portable"
cd "$work/source"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$work/home" TMPDIR="$work/tmp" \
  DOCKER_CONFIG="$scratch/docker-config" PORTABLE_JOBS=3 \
  bash cpp/gateway/packaging/portable/build.sh "$work/uos-gateway-linux-arm64.tar.gz"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$work/home" TMPDIR="$work/tmp" \
  DOCKER_CONFIG="$scratch/docker-config" \
  bash cpp/gateway/packaging/portable/verify.sh "$work/uos-gateway-linux-arm64.tar.gz" "$work/verify"
```

The verifier sealed the release on Debian 12 and 13 and checked executable relocations and native addon
dependencies. Each networkless two-CPU proof recorded 1,161 completed authenticated Chat requests and 1,161
accounting records after reopening the database. All four proofs returned 401 for unauthenticated Chat. They
used the default Landlock ABI 8 policy, recorded two seccomp filters and reported zero cgroup OOM events.

| Runtime   | Memory cap | Peak cgroup memory | Median mock requests/s |
| --------- | ---------: | -----------------: | ---------------------: |
| Debian 12 |    512 MiB |       37,773,312 B |                 204.99 |
| Debian 12 |    256 MiB |       37,830,656 B |                 208.36 |
| Debian 13 |    512 MiB |       41,041,920 B |                 194.48 |
| Debian 13 |    256 MiB |       38,658,048 B |                 192.52 |

The archive declares the version of each of the five linked vendors and carries their notices along with the
Boost, ICU, SQLite, native addon, compiler and shared-library licenses. The build and verification logs are
`portable/build.log` and `portable/verify.log` under scratch, with SHA-256
`7989fbe166428d6a60671c1b6579be65c652c2034b7abea00908692afe17f8a0` and
`98d911dd715b73d58bdee96f948693fbb80d8aa2e431736f015eb369b6aaeee3`.

## Paired conformance

The runner reused the exact Docker archive above and checked its `VERSION` commit and tree before sealing it.
The suite and Bun source are from `0c9dfda6d2abfcf3d9c9eb733585fe8db0488b28`. The only differences from the
packaging merge are guides and findings. The prepared scratch Bun snapshot used the pinned 1.4.0 runtime,
frozen root dependencies and the matching, prebuilt native artifact inputs. The wrapper verified the native KV
binding, ran all nine comparator controls, and launched `conformance/run.py` through the standard
`bench/request_path/isolated.sh` with `timeout -k 10s 180s`. Both IPv4 and IPv6 route probes returned
`ENETUNREACH`. The gateways used fresh state, empty homes and temporary directories and the same loopback mocks.

```bash
scratch=/var/tmp/cppgw-integrate-f1.WpF47zpW
work="$scratch/conformance-final"
gate=$(mktemp -d "$work/gate.XXXXXXXX")
mkdir "$gate"/{home,tmp}
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC HOME="$gate/home" TMPDIR="$gate/tmp" \
  /usr/bin/time -o "$work/run-portable.time" -f 'wall_s=%e peak_process_kib=%M exit=%x' \
  bash "$work/run-portable.sh" "$scratch/portable/uos-gateway-linux-arm64.tar.gz" 483a8684a2f615ddf267393e26be2864765fbe0c
```

Native and Bun agreed on all 186 cases, with 162 before restart and 24 after it. The suite reported 184 passes,
zero cosmetic differences, zero differential functional differences and two shared validation failures. Both
gateways returned HTTP 200 and dispatched upstream for `chat-invalid-role` and `responses-invalid-tool-type`.
The independent assertions require HTTP 400 and no dispatch. These same defects were present in the incoming
conformance evidence. The paired phase took 2.996 s. The wrapper took 5.55 s with 144,232 KiB peak process RSS
and exited 1 for the two shared assertions. [Incoming conformance findings](cppgw-conformance-e2e-20261118.md#functional-findings)

| Evidence under `conformance-final/` | SHA-256                                                            |
| ----------------------------------- | ------------------------------------------------------------------ |
| Native executable                   | `7c806b731607672294c496cb19e594e3b2aec0c6f72e5a9645a618659b13a906` |
| Bun executable                      | `086c4121c8738a8e0f5ed730e8a461bc3973b4444e372ddb77aef9a747fa2ae9` |
| Package manifest                    | `c4ef9e8df217234b3f6aae31091ed557b6bfeef373c2540ef56800e7634bf5d5` |
| `run.6JWRPzZJ/output/results.json`  | `ce9bb46d2c7cd6da6ada75cdb5d790a08fcb9ce3c401837fc9d3ecf41d1d7355` |
| `run.6JWRPzZJ/output/table.md`      | `2bea47a8a0cf89edca4f79f7da9b007578c6d65da0bc1e3028399fec1918de8e` |

## Handoff

The Links and vendor registry checks passed. Syntax checks covered 5 shell scripts, 13 Python files, 3 JSON
inputs and the embedded Python in two build scripts. The scoped `bun --no-env-file --no-install run format:check` passed
on the changed integration guides, release workflow and this report. The first-party `git diff --check` passed.
The full diff reports one blank line at EOF in `third_party/qcbor/inc/qcbor/UsefulBuf.h`. Its bytes match the
stage 2b vendor tree. The findings commits after `0c9dfda6` change only this report, so all driver and package
inputs remain bound to their recorded source trees.

The two shared validation defects remain open. The incoming macOS release supplies the Apple Silicon package
receipt. This integration exercised Linux ARM64, including the Ubuntu 26.04 WebSocket and KV gates. Physical
Raspberry Pi and production serving acceptance remain separate. The worktree is clean on `agent-job`, and
build outputs, binaries and downloads are in runner scratch.

The refreshed `origin/development` is `a3a96bc5fe96ba383936746fae23e64daa69562f`. It has no gateway tree and
does not contain the F1 merges. Both the HTTPS push preflight and the push exited 128 because the runner has
no GitHub write authentication. The GitHub PR, ancestry-preserving merge and checkout handoff to `development`
remain pending. The verified Git bundle in runner scratch keeps the ordered integration available from the fetched
input tips.

```bash
git fetch --no-tags origin refs/heads/development:refs/remotes/origin/development
GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false git push --dry-run --porcelain origin \
  HEAD:refs/heads/agent-jobs/cppgw-integrate-f1-20261118
GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false timeout -k 2s 20s git push --porcelain origin \
  HEAD:refs/heads/agent-jobs/cppgw-integrate-f1-20261118
git bundle create "$scratch/cppgw-integrate-f1-20261118.bundle" agent-job \
  ^6d28e858 ^af7a0036 ^980d97fb ^20c83e02
git bundle verify "$scratch/cppgw-integrate-f1-20261118.bundle"
```

**Summary.** All three merges are committed on `agent-job`. Linux ARM64 passed 450 release binaries and 5,104
cases, plus 157 sanitizer binaries and 1,933 cases. The Links check and Docker package verification passed.
Native and Bun agreed on all 186 paired cases, with shared validation failures for `chat-invalid-role` and
`responses-invalid-tool-type`. Boost 1.90 rejected the registered WebSocket close code 1014. The fix preserves
the one-close-frame assertion. The 608-second KV fuzz campaign produced no OOM or other genuine findings.
First-party production source fell from 188,326 to 185,211 lines. GitHub write authentication blocked publication
and the `development` handoff.
