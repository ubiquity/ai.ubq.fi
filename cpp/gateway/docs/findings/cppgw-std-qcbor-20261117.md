# Passkey CBOR replacement

## Work contract v1

Replace the translated tiny-cbor value model and codec with QCBOR 1.6.1. Keep WebAuthn verification decisions, credential storage and the original signed authenticator bytes at their existing boundaries. This change owns the CBOR region and necessary model call sites in `src/auth/passkeys.cpp`, the QCBOR vendor directory and its build registration, and the scoped CBOR tests, fuzz target and findings. ASN.1, X.509, TypeScript, other gateway modules, workflows and deployment are outside the change.

| Input                                                  | Required behavior                                                                                                                                                      |
| ------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Attestation object and stored COSE key                 | Decode exactly one canonical item and reject trailing data.                                                                                                            |
| COSE key and extensions embedded in authenticator data | Decode the first canonical item, advance by consumed wire bytes, and enforce the existing final boundary.                                                              |
| Untrusted CBOR                                         | Enforce minimal widths, definite lengths, CTAP2 key order, unique integer or text keys, valid UTF-8, the supported WebAuthn types, eight container levels and one MiB. |
| Existing Firefox 117 EdDSA attestation                 | Retain the exact known map-count correction before decoding and preserve the signed input bytes.                                                                       |

WebAuthn Level 3 requires its CBOR encoders to emit CTAP2 canonical encoding. CTAP2's ordering for integer and text keys is equivalent to ordering their canonical wire bytes lexicographically. Discovery found TPM differential fixtures with unsorted statement maps and malformed-input expectations that reflect the old codec. The existing fixture paths are outside the owned test directory. [WebAuthn Level 3](https://www.w3.org/TR/webauthn-3/#sctn-conforming-all-classes) [FIDO CTAP 2.2](https://fidoalliance.org/specs/fido-v2.2-ps-20250714/fido-client-to-authenticator-protocol-v2.2-ps-20250714.html#ctap2-canonical-cbor-encoding-form)

Acceptance uses the strict C++20 flags, app tests selected by `tools/links.py`, ASan/UBSan for the same closure, a 120-second libFuzzer smoke and a before/after timing and allocation comparison. Stop after scoped verification, the time-bounded broader sweep, and branch handoff. Exact commands, counts, measurements and open items follow as they become available.

## Initial evidence

- Base commit `55afcc8b13b2ddd10747355745da19e5264f7b76`, tree `5514adb01452ecaa20719fef73b6bf16c7e84b1a`.
- ARM64 Ubuntu 24.04 runner with GCC 14.2 and pinned Bun 1.4.0. `sudo -n unshare -n true` succeeded.
- The QCBOR codeload archive matched SHA-256 `286badcef7227533fcd5840386177ff20b1a53ca29f1544092b1fc36d020ade8`.
- The original registry has 441 binaries. `tools/links.py` resolves 58 existing test binaries with `src/auth/passkeys.cpp` in their closure.

## Continuation checkpoint

The inherited implementation at `3e496bc56494183e4ab2cab490864f9fd9bcf260` compiled and linked the two focused CMake binaries on Linux ARM64 with GCC 14.2.0, Boost 1.92.0 and the port's strict `-O2 -g0` flags. The vendored `inc/`, `src/` and `LICENSE` matched a fresh extraction of the pinned archive byte for byte. All 13 codec cases passed. The application replay ran 155 vectors in one case and reported 146 matches and nine mismatches.

| Mismatches                | Inherited fixture and strict result                                                                                                                                                 |
| ------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 2 TPM registrations       | The expected 200 responses became 400 because the synthetic `attStmt` maps begin with `ver` before `alg`. The replay needs canonical fixtures to reach the TPM verifier.            |
| 2 COSE text cases         | Registration uses `61 c3 a9` for the two-byte UTF-8 value `é`. The stored-key fixture uses a correct text length with positive key `07` after the negative keys. Both returned 400. |
| 1 authenticator extension | `a1 62 c3 a9 01` is canonical. The legacy re-encoding wrote the wrong text length before a second decode. QCBOR preserved the valid signed assertion, which returned 200.           |
| 4 diagnostic expectations | The array containing null, tagged extension, duplicate extension label and empty stored key still return 400. The warning messages now identify the strict decoder failures.        |

The existing passkeys parser fuzzer includes `src/auth/passkeys.cpp` directly. Its CMake object and link now select the same QCBOR target as the production translation unit. The generated test registry has 442 binaries, 59 with `src/auth/passkeys.cpp` in their closure. The remaining work is the release and ASan/UBSan closure, the new fuzz target's two-minute smoke, the broader release sweep, and the baseline measurement.

## Closure verification checkpoint

The inherited codec built at `28943ca1ca40dba9cf6901cbcbf8ac9d0a91aa4d` (`c9283e8ec2f48163acd92b6cb8b02b1d47b823d0`). The 59-test Links closure ran on ARM64 with GCC 14.2 at `-O2` and Clang 21 with ASan, UBSan and LeakSanitizer. Each configuration passed 58 binaries and 736 cases. The single failing binary contained the same nine WebAuthn replay mismatches listed above. The first Clang attempt stopped on headers in the existing simdutf vendor. A runner-local CMake setting treated those headers as system headers, with all first-party warning flags still enabled.

The portable `cmake/ci/driver.py` and legacy `tools/unit.mk` use a single C++ source per vendor. QCBOR needs five C objects and its include directory. The portable compile probe failed at the missing header, and `g++-14 -std=c++20 -fsyntax-only` with the include path confirmed that QCBOR's upstream C source requires the C frontend. The CMake and dedicated fuzz paths use that frontend. The portable driver and `tools/unit.mk` are outside this task's ownership.

## Canonical WebAuthn replay

The original 155-vector differential replay remains the check on the committed fixtures. A temporary replay reused the same test source and application objects with a scratch copy of the data. All five TPM statement maps were sorted while preserving every encoded value and the original signed data. The two positive and three negative TPM cases retained their recorded expectations. The malformed COSE registration and stored-key cases expect a 400, and two added canonical COSE cases verify successful registration and assertion. Four invalid cases retain their HTTP status and store state with the stricter diagnostic, and the original correctly encoded UTF-8 extension verifies with the same successful state as the other signed extension case. The temporary replay passed all 157 vectors in GCC release and Clang ASan/UBSan. Its data and logs are under `/tmp/qcbor-r1/canonical-replay`.

## Implementation and cost

QCBOR owns CBOR parsing and writing. The adapter keeps the decoded values alive for asynchronous verification, limits the wire types and sizes, checks widths and map order against the original bytes, and reports the consumed prefix. COSE re-encoding uses the already validated order and exact serialized size. The Firefox 117 repair remains limited to its known EdDSA prefix and retains the original authenticator bytes for signature checks. The production diff against `55afcc8b` removes 516 lines and adds 271 in `src/auth/passkeys.cpp`, including required call-site edits, for a net reduction of 245. The ASN.1 and X.509 codec sections compare byte for byte with the base.

| Input and operation                | Bytes | Before ns/op | After ns/op | Before allocs/op | After allocs/op |
| ---------------------------------- | ----: | -----------: | ----------: | ---------------: | --------------: |
| ES256 none attestation, decode     |   170 |       579.16 |      397.57 |                8 |               2 |
| Packed attestation, decode         |   705 |      1192.51 |      809.16 |               17 |               6 |
| ES256 COSE key, decode and encode  |    77 |       763.85 |      566.81 |               13 |               4 |
| RS256 COSE key, decode and encode  |   272 |       621.40 |      497.98 |               12 |               4 |
| UTF-8 extension, decode and encode |     5 |       159.88 |      160.16 |                2 |               1 |

Measured on this ARM64 runner using GCC 14.2 and `-O2 -g0` for both versions, including QCBOR's C objects. The inputs came from the WebAuthn replay. `bash /tmp/qcbor-r1/bench-build.sh` builds the two codec versions from their exact production source plus the same driver. `python3 /tmp/qcbor-r1/bench-run.py <output-dir>` alternates nine before/after pairs pinned to CPU 3. Each process warms up with 10,000 calls, measures 200,000 and counts `malloc`, `calloc`, `realloc` and `aligned_alloc` calls over a separate 10,000. The old encoder wrote a one-byte length for the two-byte extension key. This row measures the codec operations, with their respective output bytes.

## Fuzz smoke checkpoint

The Clang 21 libFuzzer binary from `fuzz/ci.py` exercised full decode, prefix decode and QCBOR re-encoding. A one-process rerun used the eight committed seeds with `-seed=17112026 -max_len=16384 -timeout=3 -rss_limit_mb=768 -max_total_time=120 -print_final_stats=1`. It completed 755,122 executions in 121 seconds, exit 0, with no artifacts or ASan, UBSan or LeakSanitizer reports. The process used `unshare --net`, an empty environment with a fresh home and temporary directory, and the non-root runner UID. The first fork-mode run hit libFuzzer's known ARM64 parent leak at exit. The earlier single-process run completed 872,292 executions in 121 seconds.

## Build and execution commands

The changed paths select the QCBOR manifest, the passkey Links closure, fuzz target and Markdown/JSON formatting. The runner used GCC 14.2.0 and Clang 21.1.8, Boost 1.92.0 JSON and Container archives, OpenSSL 3, and a static SQLite 3.54.0 with `SQLITE_DQS=3`. CMake applies `-std=c++20 -Wall -Wextra -Wshadow -Wconversion -Werror` and all port definitions to first-party C++. QCBOR builds five C sources as a static archive. The bounded CMake tool-view registration was exercised separately from a temporary source copy through `tools/compiler-limit.py`. All five guarded compiles returned 0 and the archive contained five objects.

```sh
q=/tmp/qcbor-r1
export UOS_GATEWAY_TOOL_ROOT="$q/cmake-sdk"
export UOS_GATEWAY_BUILD_EVIDENCE=qcbor-runner
cmake -S cpp/gateway -B "$q/cmake-release" -G Ninja \
  -DCMAKE_C_COMPILER=/usr/bin/gcc-14 -DCMAKE_CXX_COMPILER=/usr/bin/g++-14 \
  -DCMAKE_C_FLAGS='-O2 -g0' -DCMAKE_CXX_FLAGS='-O2 -g0' \
  -DCMAKE_EXE_LINKER_FLAGS="-L$q/sqlite/lib" \
  -DUOS_GATEWAY_FRAME_RECYCLING_DISABLED=ON -DUOS_GATEWAY_TESTS=ON
cmake -S cpp/gateway -B "$q/cmake-asan" -G Ninja \
  -DCMAKE_C_COMPILER=/usr/bin/clang-21 -DCMAKE_CXX_COMPILER=/usr/bin/clang++-21 \
  -DCMAKE_CXX_FLAGS='-O1 -gline-tables-only' -DCMAKE_EXE_LINKER_FLAGS="-L$q/sqlite/lib" \
  -DCMAKE_PROJECT_INCLUDE="$q/sanitizer-vendor-headers.cmake" \
  -DUOS_GATEWAY_FRAME_RECYCLING_DISABLED=ON -DUOS_GATEWAY_SANITIZER=address,undefined \
  -DUOS_GATEWAY_TESTS=ON
mapfile -t targets < <(jq -r '.tests[] | select(.sources | index("src/auth/passkeys.cpp")) | .target' \
  "$q/cmake-release/tests-registry.json")
cmake --build "$q/cmake-release" -j 2 --target "${targets[@]}"
cmake --build "$q/cmake-asan" -j 3 --target "${targets[@]}"
python3 "$q/run-tests.py" --build "$q/cmake-release" --mode affected --jobs 3 --log "$q/resume-release"
python3 "$q/run-tests.py" --build "$q/cmake-asan" --mode affected --jobs 2 --asan --log "$q/resume-asan"
cmake --build "$q/cmake-release" -j 4 --target uos_gateway uos_kv_convert uos_kv_proof uos_gateway_tests
python3 "$q/run-tests.py" --build "$q/cmake-release" --mode all --jobs 4 --log "$q/resume-all"
```

The CMake include contains a deferred `set_source_files_properties` call that gives only `src/rt/base64.cpp` Clang's `--system-header-prefix=third_party/` option. The runner wrapper selects the generated Links registry, gives each test a private mount, network and PID namespace via `sudo -n unshare`, and invokes `cmake/ci/isolate.py` with a credential-free environment. The namespace helper verifies loopback works and external routing is denied before executing the binary with a fresh tmpfs home and temporary directory. `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:detect_stack_use_after_return=1` and `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` make sanitizer findings fatal. `python3 -B cpp/gateway/tools/links.py --check` resolved 442 test entries. `sha256sum -c SHA256SUMS` verified 16 upstream files and all three vendor paths (`inc/`, `src/`, `LICENSE`) matched the pinned extraction. The on-disk binaries, replay helpers and receipts are outside the checkout under `/tmp/qcbor-r1`.

## Second continuation checkpoint

The fresh ARM64 runner started at commit `0b727faf827c0c79607921348e84c2fdf9aa20da`, tree `8f0dff322191473524b6d096ea710738acf7303c`. GCC 14.2 and Clang 21.1.8 rebuilt and linked both focused binaries, using Boost 1.92.0 and SQLite 3.54.0 from official source archives. The inherited QCBOR source, fuzz harness and benchmark implementation have the same bytes as the previous receipts. The scoped format check passed for all five changed Markdown and JSON files using the frozen Bun dependencies in scratch. The vendor's `inc/qcbor/UsefulBuf.h` contains an upstream blank line at EOF, which `git diff --check 55afcc8b..HEAD` reports.

| Fresh focused gate    | Binaries passed | Harness cases passed | Recorded WebAuthn vectors matched |
| --------------------- | --------------: | -------------------: | --------------------------------: |
| GCC 14.2 release      |             1/2 |                13/14 |                           146/155 |
| Clang 21 ASan / UBSan |             1/2 |                13/14 |                           146/155 |

All 13 codec cases passed in each profile. The sole failing harness case is the differential replay, which reproduced the nine mismatches listed above. The three negative TPM tests currently have matching recorded outcomes. Their statement maps also place `ver` before `alg`, so sorting all five TPM maps restores coverage of the downstream TPM decisions. The existing vector file, `tests/passkeys_webauthn_differential_vectors.inc`, requires separate scope approval for a committed update. The broader release build is in progress.

## Work contract v2

Bind QCBOR's C compiler in the existing qualified Clang profiles. A fresh `fuzz-clang` tool view selected `/usr/bin/cc` even though its C++ wrapper used Clang 21. The QCBOR compile then failed on the unsupported GCC option `-fsanitize=fuzzer-no-link`. The change owns the C language selection for QCBOR in `CMakeLists.txt` and this report. The passkey codec, fixtures and vendor sources retain their current input hashes for this build correction. The tool view should use its pinned Clang C frontend when no C compiler was supplied. Explicit `CC` or `CMAKE_C_COMPILER` selections and the existing native GCC and SDK selection paths stay authoritative. Acceptance is a fresh Clang tool-view configure and a five-object `uos_vendor_qcbor` build through `compiler-limit.py`, plus the direct release and sanitizer focused commands. The actual qualified host SDK is unavailable on this runner. Stop once the reproduced failure clears and the direct build commands and focused results reconcile.

The corrected fresh tool view built all five QCBOR objects with Clang 21.1.8 and `-fsanitize=fuzzer-no-link,address,undefined` through `tools/compiler-limit.py`. All five compile receipts returned 0 and the archive had five members. The commands below use the runner's Clang 21 C++ wrapper and pinned C symlink under `profile-view` and `cmake-sdk/bin`, respectively. The pre-fix control ran the same commands with `profile-before` paths and stopped on two GCC C compiles.

```sh
q=/tmp/qcbor-c1
export UOS_GATEWAY_TOOL_ROOT="$q/cmake-sdk"
env -u CC UOS_GATEWAY_TOOL_VIEW="$q/profile-view" UOS_GATEWAY_BUILD_EVIDENCE="$q/profile-after-evidence" \
  cmake -S cpp/gateway -B "$q/profile-after" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/cpp/gateway/cmake/native-toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUOS_GATEWAY_FUZZ=ON -DUOS_GATEWAY_SANITIZER=address,undefined
env -u CC UOS_GATEWAY_TOOL_VIEW="$q/profile-view" UOS_GATEWAY_BUILD_EVIDENCE="$q/profile-after-evidence" \
  cmake --build "$q/profile-after" -j 2 --target uos_vendor_qcbor
ar t "$q/profile-after/libuos_vendor_qcbor.a"
```

The direct Clang ASan/UBSan configure retained all compiler commands after sorting by input. The focused rerun passed 13 codec cases and reproduced the same nine fixture differences without sanitizer diagnostics.

The scratch fixture patch at `/tmp/qcbor-c1/passkeys-canonical-fixtures.patch` (SHA-256 `fc9e9dc8c522c4ec56e6fb170381b185ba673f42e0a754755d7ea0abf9e474d6`) updates 12 vectors and adds two canonical COSE controls. It preserves all signed TPM data and every unaffected fixture. Rebuilding just the replay translation unit against that scratch data passed all 157 vectors in GCC release and Clang ASan/UBSan. The repository fixture path is outside the assigned scope, so this receipt applies to the scratch data.

## ARM64 release sweep

The final CMake release configuration used the same compiler commands as the inherited direct GCC 14 configuration after sorting both compilation databases. It built `uos_gateway`, `uos_kv_convert`, `uos_kv_proof` and 230 test binaries. Of the 230 binaries, 229 passed with 2,773 of 2,774 harness cases. The differential replay was the sole failure. The initial aggregate build reached 559 of 1,221 build steps before the 18-minute runner budget. The follow-up batch linked every test whose compile objects were ready, then the complement resumed only the remaining binaries.

```sh
q=/tmp/qcbor-c1
export UOS_GATEWAY_TOOL_ROOT="$q/cmake-sdk" UOS_GATEWAY_BUILD_EVIDENCE=qcbor-c1-runner
cmake -S cpp/gateway -B "$q/release" -G Ninja \
  -DCMAKE_C_COMPILER=/usr/bin/gcc-14 -DCMAKE_CXX_COMPILER=/usr/bin/g++-14 \
  -DCMAKE_C_FLAGS='-O2 -g0' -DCMAKE_CXX_FLAGS='-O2 -g0' \
  -DCMAKE_EXE_LINKER_FLAGS="-L$q/sqlite/lib" \
  -DUOS_GATEWAY_FRAME_RECYCLING_DISABLED=ON -DUOS_GATEWAY_TESTS=ON
cmake -S cpp/gateway -B "$q/asan" -G Ninja \
  -DCMAKE_C_COMPILER=/usr/bin/clang-21 -DCMAKE_CXX_COMPILER=/usr/bin/clang++-21 \
  -DCMAKE_C_FLAGS='-O1 -gline-tables-only' -DCMAKE_CXX_FLAGS='-O1 -gline-tables-only' \
  -DCMAKE_EXE_LINKER_FLAGS="-L$q/sqlite/lib" -DCMAKE_PROJECT_INCLUDE="$q/sanitizer-vendor-headers.cmake" \
  -DUOS_GATEWAY_FRAME_RECYCLING_DISABLED=ON -DUOS_GATEWAY_SANITIZER=address,undefined -DUOS_GATEWAY_TESTS=ON
xargs -r cmake --build "$q/asan" -j 2 --target < "$q/asan-focus-targets"
python3 -B "$q/run-tests.py" "$PWD/cpp/gateway" "$q/asan" focused "$q/run-asan-postfix" --asan
timeout --signal=INT --kill-after=20s 18m cmake --build "$q/release" -j 4 \
  --target uos_gateway uos_kv_convert uos_kv_proof uos_gateway_tests -- -k 0
bash "$q/finish-release.sh" 124
```

The release build used four compile slots in its scratch Ninja file. The Python runner used `cmake/ci/isolate.py` for each executable with a fresh private tmpfs home and temporary directory, loopback-only fixture networking and a credential-free environment. `run-release-final/results.tsv` records case counts per binary. The CMake source fix is commit `110ed4761ca568dca2218a22b13fd1127b3a2d7d`, tree `3cf1aaa1c71044fc961ee9ca05cffe04b45a6f95`.

### Terminal results

The complement linked and ran the remaining 212 binaries. Both batches together covered every entry in the 442-test Links registry, with distinct results per binary. The release gate retained `-std=c++20 -Wall -Wextra -Wshadow -Wconversion -Werror` and the pinned Boost definitions. The three production executables and all 442 test binaries compiled and linked.

| Gate                           | Binaries passed | Harness cases passed |
| ------------------------------ | --------------: | -------------------: |
| Full GCC release sweep         |         440/442 |          5,034/5,037 |
| GCC passkeys Links closure     |           58/59 |              736/737 |
| Clang ASan/UBSan Links closure |           58/59 |              736/737 |

The sanitizer closure carries forward the earlier production-source receipt. The direct Clang command set remained byte-identical after the CMake fix, and the fresh focused sanitizer run reproduced the same result. The codec contributed all 13 passing cases to both closures. The committed WebAuthn replay matched 146 of 155 vectors, with one failed harness case. The scratch fixture patch passed 157 of 157 vectors in each profile. The other release failure was `tests/rt/url_idna_test.cpp`, with two failed cases. Its unchanged source closure links ICU 74.2 on this runner, while the qualified port uses 78.3. Its closure excludes passkeys and QCBOR.

```sh
q=/tmp/qcbor-c1
export UOS_GATEWAY_TOOL_ROOT="$q/cmake-sdk" UOS_GATEWAY_BUILD_EVIDENCE=qcbor-c1-runner
timeout --signal=INT --kill-after=20s 9m cmake --build "$q/release" -j 4 --target uos_gateway_tests -- -k 0
python3 -B "$q/run-tests.py" "$PWD/cpp/gateway" "$q/release" available "$q/run-release-complement" \
  --jobs 4 --exclude-run "$q/run-release-final/results.tsv"
```

### Handoff

The branch carries the QCBOR migration, strict codec tests, two-minute fuzz smoke evidence, benchmark results and the corrected Clang profile. Passkey production code removes 516 lines and adds 271. The adapter retains owning verifier values, canonical checks and consumed-byte accounting. It also retains the specific Firefox 117 EdDSA map-count correction.

- The differential replay fixture update is ready in scratch and requires approval for the existing fixture path. The remaining passkey mismatch is in that fixture.
- The portable `cmake/ci/driver.py` and legacy `tools/unit.mk` still require multi-source C vendor support. The existing TPM manufacturer check also preserves JavaScript's inherited property names, as the dedicated replay vector shows. These paths sit outside the owned CBOR slice.
- A merge preview against `origin/claude/cpp-gateway-port` at `45118332b287b7ba9534bb66ab1518f1990a9d9e` found an add/add conflict in `docs/DEVIATIONS-FROM-BUN.md`. Each branch contributes its own section.
- The checkout is on `agent-job`. The GitHub push dry run requires authentication, which is unavailable on the runner. The refreshed `origin/development` at `6bd78fc5e3b0783809691c6bd45d25ac45e9696e` has no `cpp/gateway/` tree and does not contain the task tip.
