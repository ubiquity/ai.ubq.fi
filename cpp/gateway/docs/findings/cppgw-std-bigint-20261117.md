# Boost.Multiprecision BigInt replacement

## Contract v1

Replace the limb implementation of `uos::rt::BigInt` with the pinned, header-only Boost 1.92.0 `cpp_int` on C++ port base `093a9e0511e9d0deae5495dc324e53ff4dccd719` (tree `cb6c6415cfd603f5aac070d424bc42b4642349e1`). Preserve the C++ API, canonical KV key and value bytes, integer ordering, low-64-bit casts and the 1,048,576-bit size limit. Number formatting and `Math` are outside the BigInt replacement. This unit has no provider or deployment changes.

| Surface                                                             | Acceptance                                                                                                                      |
| ------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| Arithmetic, comparison, signed bit operations and double conversion | Boost owns the operations. Existing arithmetic and conversion vectors are checked against the replacement.                      |
| Text and fixed-width casts                                          | Keep the application numeric formats and total input validation. Document each changed Bun-only expectation.                    |
| Storage, authority and remaining consumers                          | Use the existing test link registry with strict release and ASan/UBSan builds. Every existing binary links `src/rt/number.cpp`. |
| Robustness and cost                                                 | Run the registered BigInt libFuzzer entry point for 120 seconds and measure paired old/new release workloads.                   |

Owned production changes are confined to the BigInt regions in `src/rt/number.cpp` and `include/uos/rt/number.hpp`. Report, deviation notes, the BigInt regression and its link entry, and the BigInt fuzz target and seeds are in scope. Boost already ships through the native profile. The starting registry contains 441 binaries. The affected test paths are selected with `python3 -B cpp/gateway/tools/links.py --list-tests`. Verification uses `cpp/gateway/tools/ci-build.sh --config release` and `--config asan`, a bounded fuzz smoke, changed-document formatting and `git diff --check`. Gates and benchmarks use external scratch, with a clean environment and Linux fixture isolation for test execution.

The Bun 1.4.0 parser accepts `"-"`, `"+"` and `"0x "` as zero. The existing recorded vectors for those leniencies are in `tests/rt/number_test.cpp`, outside the assigned write paths. Authorization for a narrow expectation update is pending. Library boundary behavior, full-suite outcome and performance are still being measured.

The stop condition is a buildable committed replacement, recorded deviations, reproducible verification, cost and source-line counts, and an exact list of any unmet gates for the C++ port integrator.

## Runner preparation

Ubuntu 24.04.5 Linux ARM64 with four CPUs, GCC 14.2.0, Clang 21.1.8 and Bun 1.4.0. Boost.JSON and Boost.Container static archives were built in `/tmp` from the official Boost 1.92.0 release archive. Its SHA-256 matched the upstream release metadata. [Archive metadata](https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2.json)

```text
5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c  boost_1_92_0.tar.bz2
```

## Continuation and contract v2

The resumed checkout started clean at `e6c2a002d954318e10b52b411f871f3d9d48dbcd` (tree `6f637163a39cf068e4d5a3626c5261b7758ff240`). The inherited commits contain the arithmetic replacement, a three-case BigInt regression and the registered fuzz target. The registry now contains 442 targets. The runner restored the verified Boost archive and installed Clang 21.1.8. The old and new implementations use the same compiler for paired measurements. The full test driver uses SQLite 3.54.0 with `SQLITE_DQS=3` per the portable CI instructions. [Portable CI requirements](../CI.md)

The inherited source compiled with the strict port flags at `-O2`. The new BigInt test passed 3/3 cases and the Number test passed 20/21. The only failure was the recorded BigInt parser case. Its three zero expectations for incomplete signed or prefixed tokens are outside the explicit write scope. Bun 1.4.0 independently confirmed all three zero results and the same prefix-only behavior with octal and binary. Its full-width unsigned cast also differs from the gateway as recorded in the deviations. [BigInt deviations](../DEVIATIONS-FROM-BUN.md#bigint)

The suite and fuzz campaigns use empty home and temporary directories, a credential-free environment and Linux namespaces. The selected gates are the two C++ build profiles for all 442 link closures, the 120-second BigInt fuzz smoke, paired measurements, changed-document formatting and `git diff --check`. The verification runs and measurements are pending. The stop condition is the terminal result of these runs, including any differences in test fixtures or unavailable handoff prerequisites.

## Resumed qualification

The handoff contained the v2 notes and deviation update in the index. It also contained a completed libFuzzer log. The registered harness ran 901,036 inputs in 121 seconds with a peak RSS of 411 MiB and zero findings. The checked-in corpus has five seeds. No new vendor or build registration is needed because the pinned Boost headers are already part of the gateway's build.

Boost owns arithmetic, comparison, signed bitwise operations, shifts, exponentiation, conversion to and from binary64, and decimal, octal and hexadecimal arithmetic and formatting. The adapter validates complete input, normalizes decimal digits for Boost's parser, uses Boost bit import and export for the power-of-two formats that need them, and groups other output radices into native `to_chars` chunks using Boost division. It retains the public BigInt API, existing error classes, bounds on allocation, fixed-width wrapping and signed conversion from the low 64 bits. These preserve the integer representations used by KV, typed values, authority and the FNV-1a hash.

Relative to `093a9e0511e9d0deae5495dc324e53ff4dccd719`, the production diff deletes 448 lines and adds 146 across `src/rt/number.cpp` and `include/uos/rt/number.hpp`, a net reduction of 302 lines. The new implementation replaces the limb vector, carry and borrow operations, arbitrary-precision multiply and divide, two's-complement limb translation and binary64 rounding algorithm.

### Paired measurements

Both benchmark binaries used Clang 21.1.8 `-O2` with the strict port flags on the same Linux ARM64 runner. Each linked its own `number.cpp` and used the corresponding `number.hpp`. The baseline files came from `git show 093a9e05:<path>`. Each reported value is the median of seven runs using `taskset -c 3` and thread CPU time. Runs alternated old-first and new-first, warmed up each operation with 100 iterations and counted calls to global C++ allocation operators during the timed loop. Inputs and result consumption were identical between binaries.

| Operation                                | Iterations per sample | Old ns/op | Boost ns/op | Old allocations/op | Boost allocations/op |
| ---------------------------------------- | --------------------: | --------: | ----------: | -----------------: | -------------------: |
| Increment counter starting at 10,000,000 |             1,000,000 |     23.20 |       15.08 |              1.000 |                0.000 |
| FNV-1a byte step with 64-bit wrapping    |             1,000,000 |    113.48 |       91.46 |              5.996 |                0.000 |
| Parse signed decimal `-123456789`        |               500,000 |    138.26 |       60.79 |              6.000 |                0.000 |
| Parse 77 decimal nines, 256 bits         |               100,000 |    728.04 |      237.51 |             12.000 |                2.000 |
| Parse 1,233 decimal nines, 4,096 bits    |                 2,000 | 69,518.67 |    4,180.94 |             20.000 |                4.000 |
| Multiply 4,097- and 2,048-bit operands   |                 5,000 |  8,416.72 |    2,728.94 |              1.000 |                1.000 |
| Divide 4,097- by 2,048-bit operands      |                 5,000 |  5,655.06 |    3,464.41 |              4.000 |                3.000 |
| Format 4,097 bits in decimal             |                 2,000 | 43,528.02 |   34,513.08 |              9.000 |               70.000 |
| Format 4,097 bits in base 36             |                 2,000 | 39,148.08 |   36,048.08 |              8.000 |                9.000 |
| Convert 256 bits to binary64             |             1,000,000 |     27.58 |        9.73 |              1.000 |                0.000 |

The two wide operands are `(1 << 4095) + (10^1233 - 1)` and `(1 << 2047)` plus 616 decimal sevens. The 256-bit conversion uses the same 77 nines as the parser workload. `sizeof(BigInt)` is 32 bytes in each build. Boost's decimal formatting took less CPU time on the wide operand and allocated about eight times as often.

The full suites are still running. The first portable-driver invocation found that `tools/ci-build.sh` is mode 0644, so the succeeding invocation used `bash` to run the script. The Boost headers are in the extracted archive and the compiled archives are in the separate `boost/lib` prefix.

### Fixed-width oracle correction

A fresh isolated Bun 1.4.0 probe corrected the v2 observation about full-width casts. `BigInt.asUintN(1048576, -1n).toString(2).length` returns `1048576`, and `BigInt.asUintN(1048577, -1n)` throws `RangeError`. `BigInt.asIntN(1048576, 1n << 1048575n)` returns the negative of its second argument. The Boost adapter agrees in all three cases. It also fixes the old C++ signed cast's size check on a temporary mask. The deviations section describes the stricter parser cases.

The portable CI compiler applies strict warnings to `base64.cpp`, including its tree-relative simdutf header. Clang 21 reports upstream sign-conversion diagnostics there. The scratch runner uses the same `--system-header-prefix=third_party/` setting as the repository sanitizer driver for that source. The first-party warning set stays fatal.

## Contract v3

The complete link closure exposed a compilation regression in the inherited BigInt header. Boost's `<iomanip>` include introduces `std::quoted` into argument-dependent lookup in `src/rt/kv_value.cpp`. The BigInt header will use Boost's forward declaration. The adapter will manage a `cpp_int` in aligned inline storage with compile-time size and alignment checks, retaining allocation-free small integers. All production writes stay in the BigInt regions. The source change supersedes the previous build, fuzz and benchmark receipts for acceptance. The same gates and prior stop condition apply to the revised source.

A Clang 21 `--target=arm64-apple-darwin` probe confirmed 8-byte alignment for `long double` and 16-byte alignment for `__int128`. The storage explicitly aligns to 16 bytes for both deployment targets. The runner can check this ABI distinction and run the Linux suite. A native Darwin suite requires a Darwin runner.

### v3 Linux ARM64 evidence

The measured source is commit `5ab1a34de1cb47b31a990c0e3ca6ec3024440f2b`, tree `789c830f90d941ee4e4183f71ad33f91a875f0ed`. The v3 diff deletes **448 production lines** and adds **199**, for a net reduction of **249**. The inline object adds construction, destruction, copy, move and alignment checks. Boost handles the integer storage and operations inside it. The wide-storage regression covers independent copies and reuse after a move.

The phased suite uses the existing portable CI driver's source inventory, dependency resolver, compiler rules, linker and Linux namespace runner. It compiles each distinct translation unit once per profile and runs closures in batches, starting with BigInt and its storage callers. Clang 21.1.8 uses the fatal port warning set and `-O2` for release. The ASan/UBSan profile uses `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer`. The compile of the vendored simdutf include uses a system-header prefix on `base64.cpp`. SQLite 3.54.0 was built with `SQLITE_DQS=3`. The suite used system ICU 74.2.

```bash
scratch=/tmp/cppgw-bigint.cJMy6S
for profile in release asan; do
  run="$scratch/drivers/$profile"
  env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$run/final-home" TMPDIR="$run/final-tmp" \
    LANG=C.UTF-8 PKG_CONFIG_PATH="$scratch/sqlite/lib/pkgconfig" \
    python3 -B "$scratch/run-closures.py" "$PWD/cpp/gateway" "$scratch" "$profile" --jobs 2
done
python3 -B cpp/gateway/tools/links.py --check tests/legacy/bigint_library_test.cpp
git diff --check 093a9e05 HEAD
```

The two profile commands ran concurrently with two workers each. The runner helper SHA-256 is `f49db6e41fa77981a64ce309f14ffba7db6cf2f1e9d6f7242e53b7664b684948`. Each build directory's `closures.jsonl` and `logs` contain per-binary counts, exact link commands and test output. Both profiles passed the four BigInt regression cases. Both recorded Number runs passed 20 of 21 cases. The failure is the parser test with three Bun-only expectations. A temporary copy of that test with just those three expected results changed to rejection passed all 21 cases in each profile.

At the first v3 checkpoint, release recorded 113 passing and 2 failing binaries with 1,137 passing cases out of 1,140. ASan/UBSan recorded 33 passing and 1 failing binary with 361 passing cases out of 362. In addition to the Number failure, the release IDNA binary failed its two ICU differential cases. The independent URL qualification uses ICU 78.3 and records a passing control with that version. [URL IDNA qualification](cppgw-oss-ada-url-20261117.md)

The final registered BigInt fuzz entry was compiled with the port warning flags, `-O1 -gline-tables-only -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined`, and the two translation units `cpp/gateway/fuzz/bigint_library.cpp` and `cpp/gateway/src/rt/number.cpp`. The runner used `bash "$scratch/fuzz-final.sh"` to build and run with fresh namespaces and clear credentials. The executable received these options.

```sh
/tmp/b/bigint_library -seed=17112026 -max_len=4096 -max_total_time=120 \
  -timeout=5 -rss_limit_mb=2048 -malloc_limit_mb=128 -print_final_stats=1 \
  -artifact_prefix=/tmp/b/artifacts/ /tmp/b/corpus
```

LibFuzzer exited 0 after **753,947 inputs in 121 seconds** from the five checked-in seeds, with no findings and a peak RSS of 489 MiB. The binary SHA-256 is `fd01665e0dbecc167ee2477a91d808f9c7f9cd5fc657ba23c1fac410fa8d33ee`. The campaign log SHA-256 is `59c4db69a23c91c7f7e18c01e4ef2b92a5c32e72940956aa8757910af4ccd97f`.

### v3 paired measurements

The final source and the baseline reran the seven paired samples described above. Both Clang 21 binaries used the strict flags and `-O2`. The scratch benchmark driver SHA-256 is `a141c63e44190ca1dda9a8965ec12b16c52443188185e40115baacb3aa70ed7c`. The measurements supersede the earlier table.

| Operation                              | Old ns/op | Boost ns/op | Old allocations/op | Boost allocations/op |
| -------------------------------------- | --------: | ----------: | -----------------: | -------------------: |
| Increment counter                      |     23.28 |       15.93 |              1.000 |                0.000 |
| FNV-1a 64-bit byte step                |    113.12 |       92.68 |              5.996 |                0.000 |
| Parse signed decimal                   |    138.51 |       64.25 |              6.000 |                0.000 |
| Parse 256-bit decimal                  |    730.98 |      248.66 |             12.000 |                2.000 |
| Parse 4,096-bit decimal                | 70,018.23 |    4,104.77 |             20.000 |                4.000 |
| Multiply 4,097- and 2,048-bit operands |  8,461.29 |    2,696.85 |              1.000 |                1.000 |
| Divide 4,097- by 2,048-bit operands    |  5,675.61 |    3,476.05 |              4.000 |                3.000 |
| Format 4,097 bits in decimal           | 43,679.56 |   34,610.39 |              9.000 |               70.000 |
| Format 4,097 bits in base 36           | 39,125.05 |   36,411.05 |              8.000 |                9.000 |
| Convert 256 bits to binary64           |     26.46 |        9.67 |              1.000 |                0.000 |

The type remains 32 bytes. The larger number of allocations for wide decimal formatting reflects Boost's `str()` implementation.

### Independent controls and handoff

GCC 14.2.0 compiled the final BigInt source and its regression with the strict port flags at `-O2`, and the test passed 4/4 cases. The scratch Number control using the final release and sanitizer objects passed 21/21 cases in each profile inside the CI driver's namespaces. It changes exactly the three expected values for `"-"`, `"+"` and `"0x "` from zero to rejection. The checked-in Number vector file is outside this job's write scope.

The URL baseline control uses the unchanged `src/rt/{url,string}.cpp` and `tests/rt/url_idna_test.cpp`. It failed both cases against system ICU 74.2. The same sources passed both cases against ICU 78.3 in release and ASan/UBSan. The upstream ICU archive verified against the release's `SHASUM512.txt`. The control is reproducible on this runner with `bash "$scratch/icu-idna-control.sh"`. [ICU 78.3 source release](https://github.com/unicode-org/icu/releases/tag/release-78.3)

The v3 benchmark compile commands use the following inputs. Each of the seven runs invokes `env -i HOME="$scratch/bench/final/home" TMPDIR="$scratch/bench/final/tmp" PATH=/usr/bin:/bin LANG=C.UTF-8 taskset -c 3 "$scratch/bench/final/<old-or-new>"`.

```bash
strict=(-O2 -std=c++20 -Wall -Wextra -Wshadow -Wconversion -Werror
  -DBOOST_JSON_NO_LIB -DBOOST_CONTAINER_NO_LIB -DBOOST_SYSTEM_NO_LIB
  -DBOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING -DUOS_GATEWAY_NATIVE_LIBRARIES=1
  -isystem "$scratch/boost_1_92_0" -I cpp/gateway/include -I cpp/gateway)
clang++-21 -I "$scratch/old/include" "${strict[@]}" "$scratch/old/number.cpp" \
  "$scratch/bench/bigint-bench.cpp" -o "$scratch/bench/old"
clang++-21 "${strict[@]}" cpp/gateway/src/rt/number.cpp \
  "$scratch/bench/bigint-bench.cpp" -o "$scratch/bench/final/new"
git diff --numstat 093a9e05 HEAD -- cpp/gateway/src/rt/number.cpp cpp/gateway/include/uos/rt/number.hpp
```

At 01:48 UTC, release had completed 355 of 442 closures with 353 passing binaries and 3,841 passing cases out of 3,844. ASan/UBSan had completed 220 of 442 with 218 passing binaries and 1,914 passing cases out of 1,917. Each reported two failing binaries, Number and IDNA. Every completed application binary passed, including the FNV-1a reliability hash in both profiles.

A fetch refreshed `origin/development` to `6bd78fc5e3b0783809691c6bd45d25ac45e9696e` and the port branch to `45118332b287b7ba9534bb66ab1518f1990a9d9e`. The development tree has no `cpp/gateway` entry. `git merge-tree --write-tree --name-only origin/claude/cpp-gateway-port HEAD` found one add/add conflict in the shared `DEVIATIONS-FROM-BUN.md`, which the two branches created independently. The fuzz registry merged in the preview.

`GIT_TERMINAL_PROMPT=0 git push --dry-run --porcelain origin HEAD:refs/heads/pool-partial/cppgw-std-bigint-20261117-44188151110` failed with GitHub requiring authentication. The same dry run with GitHub CLI's credential helper failed. The BigInt commits are in the local task branch pending repository handoff.

### Release terminal receipt

The release sweep compiled and ran all **442 binaries**. On the runner's system ICU 74.2 it recorded **440 passing binaries, 2 failing binaries and 5,025 passing cases out of 5,028**. All **405 application binaries and 4,571 application cases** passed. No builds or test runs timed out. The receipt SHA-256 is `4c131370ccc469a05bb00b15a2154ef253c38c9de85f846409a5f3e6a4f126f6`. The input digest was `173c881ddb9a3698a4387278b11f2c247ddcf3cb161a0cf038f1087d0be05416` at start and finish.

The exact IDNA link closures in release and ASan/UBSan were relinked with ICU 78.3 after recompiling their `url.cpp` object against those headers. Each passed 2/2 cases with the BigInt object and all other objects from the main sweep. The command was `python3 -B "$scratch/icu-link-closures.py" "$PWD/cpp/gateway" "$scratch" "$scratch/icu-full-closures"` in the isolated environment. This accounts for both IDNA cases in each raw suite.

A subsequent full source build used `python3 -B "$scratch/link-production.py" "$PWD/cpp/gateway" "$scratch"`. It compiled all 369 production objects, archived `libuos_gateway.a` and linked `uos_gateway`, `uos_kv_convert` and `uos_kv_proof` using the strict release flags and the vendored archives. The startup and entrypoint units compiled for those links. The binaries are link evidence and were kept in scratch.

The sanitizer sweep kept its completed entries and switched from two to four workers after release finished. The command is `python3 -B "$scratch/resume-closures.py" "$PWD/cpp/gateway" "$scratch" asan --jobs 4 --resume-after 5ab1a34de1cb47b31a990c0e3ca6ec3024440f2b` with the same environment and compiler flags. The helper verifies the scoped files against the tested revision before resuming. Its SHA-256 is `c99e0d83547a2307c66e741068c556b2a9392d7c24f8f259dd3e36450e7117b5`.

## Fresh Linux ARM64 continuation

The clean handoff at `4886bd900278fa88bb36769499af8e89c4a9d69e` (tree `fcad75012ddd37adccdb86c61b3668bb9c6a5c00`) differs from the v3 revision only in this report. The previous runner's scratch objects were unavailable. The full release, fuzz and benchmark receipts still apply to the inherited implementation. The remaining verification is the full sanitizer link closure. The Number vector remains a shared test outside the assigned write scope.

The runner restored Clang 21.1.8, the verified Boost 1.92.0 source archive, SQLite 3.54.0 with `SQLITE_DQS=3`, and ICU 78.3. It rebuilt static Boost.JSON and Boost.Container with `./b2 -j2 --with-json --with-container toolset=gcc variant=release link=static threading=multi --stagedir="$scratch/boost" --build-dir="$scratch/boost-build" stage`. The focused inherited-tree build compiled `src/rt/number.cpp` and both test files with the strict port flags and `-O2`, then linked the tests against Boost.JSON. The BigInt regression passed 4/4 cases. The mixed Number binary passed 20/21, with the documented incomplete-token parser expectation failing. Both binaries ran inside the portable CI driver's fixture namespaces.

```bash
scratch=/tmp/cppgw-bigint-c1.WnJb02
env -i PATH=/usr/bin:/bin LANG=C.UTF-8 \
  bash "$scratch/focus-release.sh" "$scratch" "$PWD/cpp/gateway"
env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$scratch/driver-home" TMPDIR="$scratch/driver-tmp" \
  LANG=C.UTF-8 PKG_CONFIG_PATH="$scratch/icu-install/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig" \
  python3 -B "$scratch/sanitize-closures.py" "$PWD/cpp/gateway" "$scratch" 4
```

The sanitizer sweep builds each closure's objects with the fatal port warning set, `-O0 -gline-tables-only -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined`, and uses the repository's portable builder and namespace runner. Lower optimization and line tables shorten the cold build. The run prioritizes the BigInt, Number and IDNA binaries, then storage, authority and other users, before completing the 442 entry registry. The driver logs progress after each batch and writes a row to `asan/results.jsonl` as each binary finishes. It checks the source digest before and after the run and writes a terminal receipt. The edited surface for this continuation is this report, so the additional file gates are pinned Prettier and `git diff --check`.

### Sanitizer loader recovery

The first two KV failures came from child processes exiting 127 because their explicit environments dropped `LD_LIBRARY_PATH` and the loader could not find `libicuuc.so.78`. The SQLite differential binary loaded the system's SQLite 3.45.1 via `libsqlite3.so` and failed three cases. The pinned SQLite build has no SONAME, so the unversioned name had to resolve to the new library. The runner staged the same ICU and SQLite shared libraries under a temporary loader path outside the checkout. A probe linked with the suite's SQLite flags resolved 3.54.0 in an empty environment.

```bash
prefix=/opt/$(basename "$scratch")/lib
loader_config=/etc/ld.so.conf.d/01-$(basename "$scratch").conf
sudo install -d -m 0755 "$prefix"
sudo install -m 0644 "$scratch"/icu-install/lib/libicu{uc,i18n,data}.so.78.3 \
  "$scratch/sqlite/lib/libsqlite3.so.3.54.0" "$prefix"
sudo ln -s libsqlite3.so.3.54.0 "$prefix/libsqlite3.so"
printf '%s\n' "$prefix" | sudo tee "$loader_config" >/dev/null
sudo ldconfig
ln -s libsqlite3.so.3.54.0 "$scratch/asan/bin/lib/libsqlite3.so"
env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$scratch/driver-home" TMPDIR="$scratch/driver-tmp" \
  LANG=C.UTF-8 python3 -B "$scratch/relink-loader-cohort.py" "$PWD/cpp/gateway" "$scratch"
env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$scratch/driver-home" TMPDIR="$scratch/driver-tmp" \
  LANG=C.UTF-8 python3 -B "$scratch/relink-loader-range.py" "$PWD/cpp/gateway" "$scratch" 35 67
```

Sixty-six closures were relinked and rerun using the already compiled objects. The IDNA result in the same cohort used ICU 78.3 and passed 2/2, with no SQLite calls or child processes in that test. The three affected SQLite and KV binaries passed 29/29, 17/17 and 7/7 after the recovery. At the 123-closure checkpoint, reconciling those reruns gave 122 passing binaries and 1,607 passing cases out of 1,608. The remaining failure is the documented Number parser fixture.

### Full sanitizer sweep and optimized controls

The `-O0` sweep built 829 distinct first-party translation units and both vendor archives, then attempted all 442 registered test binaries in 2,572.436 seconds. The raw result was 435 passing binaries, 6 failing binaries and 1 timeout. The harness reported 4,985 passing cases out of 4,993. JSON and Sentinel retention exited before printing case summaries, so those binaries contribute zero to the raw case totals. The source digest was `173c881ddb9a3698a4387278b11f2c247ddcf3cb161a0cf038f1087d0be05416` both at launch and at completion, matching the inherited full release receipt. The raw sanitizer receipt SHA-256 is `d35ef6f67819db76f7232ece50edf879a1fcdb5ac9fb23da0fc937521ed00d45`.

Alongside the Number and loader failures, the `-O0` run recorded three results in other components. The supervisor redaction binary passed 39/41 cases, missing two 2-second wall-clock assertions. The deep JSON equality test hit an AddressSanitizer stack overflow inside `matchesAt` in `src/rt/json.cpp:1008` and `:1013`. The Sentinel retention metadata binary was CPU-active when the driver killed it at 300 seconds. The same strict ASan/UBSan closure build with `-O1` passed the JSON binary's 25/25 and the redaction binary's 41/41 cases. The retention closure is running at `-O1` with a 600-second driver limit.

```bash
controls=(tests/codex_supervisor_secret_test.cpp tests/rt/json_test.cpp tests/sentinel_replay_retention_metadata_test.cpp)
env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$scratch/driver-home" TMPDIR="$scratch/driver-tmp" \
  LANG=C.UTF-8 python3 -B "$scratch/optimized-asan-control.py" "$PWD/cpp/gateway" "$scratch" build "${controls[@]}"
for test in tests/rt/json_test.cpp tests/codex_supervisor_secret_test.cpp; do
  env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$scratch/driver-home" TMPDIR="$scratch/driver-tmp" \
    LANG=C.UTF-8 python3 -B "$scratch/optimized-asan-control.py" "$PWD/cpp/gateway" "$scratch" run "$test"
done
env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$scratch/driver-home" TMPDIR="$scratch/driver-tmp" \
  LANG=C.UTF-8 UOS_BIGINT_CONTROL_TIMEOUT=600 \
  python3 -B "$scratch/optimized-asan-control.py" "$PWD/cpp/gateway" "$scratch" run tests/sentinel_replay_retention_metadata_test.cpp
```

The optimized build reused its compiled objects as closures were added. The redaction run began after the full sweep released the runner CPUs. The receipt retains the original JSON stack overflow and the passing optimized rerun.

### Terminal verification and handoff

The optimized Sentinel retention metadata closure passed 10/10 cases in 64.988 seconds. Reconciling the loader and optimization controls with `python3 -B "$scratch/summarize-sanitizer.py" "$scratch"` yields **441/442 passing binaries and 5,027/5,028 passing cases**. Every one of the **405 application binaries and 4,571 application cases** passed. The only sanitizer diagnostic across the runs was the recorded `-O0` JSON stack overflow. The reconciled receipt SHA-256 is `ed4af61e07391f429def798ca5744215d465dc6264b2fee9bdd5588b4d72abb4`. The inherited release sweep also yields **441/442 binaries and 5,027/5,028 cases** when its ICU 78.3 control replaces the two ICU 74.2 IDNA failures. Both profiles passed all four dedicated BigInt cases.

The remaining failure in both profiles is the shared Number test's three stale expectations at `tests/rt/number_test.cpp:2083–2086`. The assigned write paths exclude that file. The prior scratch control passed all 21 cases with just those three values updated to expect rejection. The replacement's digit requirement and the two classes of parser deviation are documented. [BigInt deviations](../DEVIATIONS-FROM-BUN.md#bigint)

Boost now owns the integer operations and core conversions. The adapter keeps whole-token validation, allocation bounds, error translation, fixed-width wrapping and radix output compatibility. The production diff removes 448 lines and adds 199. The v3 fuzz smoke ran 753,947 inputs in 121 seconds with no findings. The paired 4,096-bit decimal parse fell from 70,018.23 to 4,104.77 ns/op and from 20 to 4 allocations/op. Wide decimal formatting improved in CPU time and increased from 9 to 70 allocations/op.

The task commits are on the clean `agent-job` branch. `GIT_TERMINAL_PROMPT=0 git push --dry-run --porcelain origin HEAD:refs/heads/agent-jobs/cppgw-std-bigint-r1-44202080331-1` failed because GitHub write authentication was unavailable. The refreshed `origin/development` at `6bd78fc5e3b0783809691c6bd45d25ac45e9696e` has no C++ gateway tree and lacks the BigInt implementation tip in its ancestry. The port branch at `45118332b287b7ba9534bb66ab1518f1990a9d9e` has the previously recorded add/add conflict in `DEVIATIONS-FROM-BUN.md`. The shared Number fixture and repository integration remain open.
