# ICU case mapping and default collation

## Work contract v1

Replace the case-mapping and collation code from plan rows 15 and 16 with the ICU 78.3 already used by the native build. Base commit `093a9e0511e9d0deae5495dc324e53ff4dccd719`, tree `cb6c6415cfd603f5aac070d424bc42b4642349e1`.

| Surface                         | Required behavior                                                                                                                                   |
| ------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------- |
| `toLowerCase` and `toUpperCase` | ICU root casing, including expansions and contextual mappings. Preserve the WTF-8 string representation and accept embedded NULs.                   |
| `localeCompare`                 | ICU root collation with canonical normalization and tertiary strength. Preserve comparison signs and test the catalog and persisted-schema callers. |
| Arbitrary bytes                 | Use the existing WTF-8 conversion boundary and its replacement for malformed byte sequences. Check lengths before passing them to ICU.              |
| Application contracts           | Preserve authentication and validation decisions, wire formats, and stored-data formats.                                                            |

Ownership covers the two string operations, their declarations, the findings and deviation notes, the component regression and fuzz fixtures, and any required linkage registration. UTF conversion, the TypeScript oracle, other runtime components, CI workflows, hosted activation and production data are separate work.

Validate with Bun 1.4.0, the existing case and collation vectors, a comparison against the original string implementation, strict warning-fatal release and ASan/UBSan Linux ARM64 tests, and a 120-second libFuzzer campaign. Every registered test links `src/rt/string.cpp`, so the sweep includes the full registry. Measure allocation calls and wall time on matched old and current library builds. Format the changed documents and registry and run `git diff --check`.

Open questions at discovery are whether ICU changes any recorded collation signs, whether the historical SDK profile supplies ICU, and whether the runner can finish both full sweeps. Record the exact dependency and source versions, observed differences, terminal test counts and any remaining qualification before handing the committed lane back to the pool.

## Implementation

`icu::CaseMap` handles ASCII directly in UTF-8. The non-ASCII path uses the existing WTF-8 to UTF-16 conversion and passes the whole string to `u_strToLower` or `u_strToUpper`. This preserves lone surrogates and lets ICU resolve final sigma and expanding mappings. The UTF-16 output uses a stack buffer and retries with ICU's reported size when it needs more space. Length checks protect the signed 32-bit ICU API boundary. Both paths use the root locale and explicit lengths.

`ucol_strcoll` compares UTF-16 with a shared read-only root collator at tertiary strength with canonical normalization enabled. ICU's weight tables, canonical decomposition and contractions replace the corresponding first-party code. The header signatures and the UTF conversion regions are unchanged. Exact identity checks such as the persisted KV name-list check still compare the original strings.

`git diff --numstat 093a9e05..ef112861` records **1,450 lines removed and 52 added in `string.cpp`**. The header has two comment lines removed and three added. CMake adds four lines to find and link ICU for the historical SDK profile. Native builds use the ICU linkage they already have. ICU 78.3 was built in runner scratch from the verified release archive. [ICU 78.3](https://github.com/unicode-org/icu/releases/tag/release-78.3)

## Equivalence and differences

Bun 1.4.0 on this runner reports ICU 78.3. `new Intl.Collator().resolvedOptions()` reports `en-US`, variant sensitivity, punctuation significant, numeric ordering disabled and no explicit case-first ordering. The unmodified `tests/rt/string_test.cpp` passed all 19 cases with the old implementation and with ICU. Its 73 case rows, 2,388 recorded collation signs and ordered repertoire retain their expectations.

| Direct old-to-new comparison                                       | Observations | Differences |
| ------------------------------------------------------------------ | -----------: | ----------: |
| Lowercase of every code point, including surrogate values          |    1,114,112 |           0 |
| Uppercase of every code point, including surrogate values          |    1,114,112 |           0 |
| Contextual lowercase across three final-sigma contexts             |      509,073 |           0 |
| Recorded collation signs, including reverse directions of `LOCALE` |        2,388 |           0 |
| Stable rank and adjacent ties in the recorded repertoire           |        5,231 |           0 |
| Comparison pairs outside the fixture's recorded repertoire         |           92 |          32 |

The live Bun replay regenerated the four existing digests `756211308`, `2828695232`, `1204333433` and `658630463` for lowercase, uppercase, contextual lowercase and collation. It also matched ICU on all 92 pairs outside the fixture's recorded repertoire, including the 32 signs that changed from the old implementation. ICU supplies weights for scripts and mathematical letters the old port sorted by code point. For example, Deseret U+10400 now sorts before the lone surrogate U+D83D, and Hangul `가` and `가` now compare equal. No override entries are needed. The new regression suite covers canonical equivalence, lone surrogates, embedded NULs, malformed byte substitution and expansion past the stack buffer.

## Measurements

Linux ARM64 Neoverse-N2, Clang 21.1.8 `-O2`, and ICU 78.3. The benchmark linked the original and current `string.cpp` as separate namespaces in one executable. After 2,000 warm-up operations, it alternated old-first and new-first across seven wall-time samples, pinned to CPU 3 with `nice -n -12` to give it priority over the concurrent compilers. The table reports medians. Separate 10,000-operation samples counted glibc `malloc`, `calloc`, `realloc` and `aligned_alloc` calls, including those through libstdc++ and ICU. Input setup and one-time ICU initialization are outside the samples.

| Workload                                       | Old ns/op | ICU ns/op | Old allocations/op | ICU allocations/op |
| ---------------------------------------------- | --------: | --------: | -----------------: | -----------------: |
| Lowercase `Content-Type`                       |     11.19 |     58.06 |                  0 |                  0 |
| Uppercase `patch`                              |     10.81 |     46.01 |                  0 |                  0 |
| Lowercase `İSTANBUL ΟΔΥΣΣΕΎΣ`                  |    365.57 |    262.38 |                  2 |                  2 |
| Uppercase 24 repetitions of `Straße `          |  3,776.34 |  2,153.53 |                  2 |                  3 |
| Compare `gpt-6.2-sol` and `gpt-6.2-sol-fast`   |    714.98 |    141.61 |                  4 |                  2 |
| Compare `é` and `e` plus U+0301                |    249.60 |    118.69 |                  4 |                  0 |
| Stable sort eight mixed-case model identifiers | 11,415.69 |  2,229.21 |                 65 |                 33 |

The short ASCII calls cost more after adopting ICU. The direct UTF-8 call eliminates the extra allocation that the initial UTF-16-only implementation needed for `Content-Type`. The long expansion allocates an extra output buffer and takes less time. Both representative comparisons and the model sort use fewer allocations.

## Runner commands

The scratch root for this run was `/tmp/cppgw-icu.ZaQDT1`. Bun 1.4.0 and GCC 14.2.0 were already present. The runner installed Clang, sanitizer runtimes, the symbolizer, Brotli headers and mold from the verified LLVM and Ubuntu package archives with `sudo apt-get install -y clang-21 libclang-rt-21-dev llvm-21 libbrotli-dev mold`. The source archives and settings were as follows.

| Prepared dependency                     | SHA-256 of official release archive                                | Build                                                                                                                                                         |
| --------------------------------------- | ------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Boost `boost_1_92_0.tar.bz2`            | `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` | `./bootstrap.sh --with-libraries=json,container` then `./b2 -j2 --with-json --with-container variant=release link=static runtime-link=shared cxxstd=20 stage` |
| ICU `icu4c-78.3-sources.tgz`            | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` | `./configure --prefix="$work/icu78" --disable-tests --disable-samples --disable-extras --disable-static` then `make -j2 && make install`                      |
| SQLite `sqlite-autoconf-3540000.tar.gz` | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` | `./configure --prefix="$work/sqlite" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'` then `make -j1 && make install`                         |

The standalone controls used the following commands from the repository root. `compare.cpp` read the original fixture arrays and compared strings, signs and stable tie groups directly. `oracle.ts` replayed the same scalar, context and repertoire loops through the JavaScript built-ins.

```bash
work=/tmp/cppgw-icu.ZaQDT1
strict=(
  -std=c++20 -O2 -Wall -Wextra -Wshadow -Wconversion -Werror
  -DBOOST_JSON_NO_LIB -DBOOST_CONTAINER_NO_LIB -DBOOST_SYSTEM_NO_LIB
  -DBOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING -DUOS_GATEWAY_NATIVE_LIBRARIES=1
  -I cpp/gateway/include -I cpp/gateway
  -isystem "$work/boost_1_92_0" -isystem "$work/icu78/include"
)
icu=(-L "$work/icu78/lib" "-Wl,--disable-new-dtags,-rpath,$work/icu78/lib" -licui18n -licuuc)
git show 093a9e05:cpp/gateway/src/rt/string.cpp > "$work/old-string.cpp"
clang++-21 "${strict[@]}" -Duos=baseline -c "$work/old-string.cpp" -o "$work/baseline-ns.o"
clang++-21 "${strict[@]}" -c cpp/gateway/src/rt/string.cpp -o "$work/new-string.o"
for probe in compare benchmark; do
  clang++-21 "${strict[@]}" "$work/$probe.cpp" "$work/baseline-ns.o" "$work/new-string.o" \
    "${icu[@]}" -o "$work/$probe-final"
done
# Each probe ran with an empty HOME and fresh TMPDIR. The native probes used a fresh network namespace.
scratch=$(mktemp -d "$work/oracle.XXXXXX")
mkdir "$scratch/h" "$scratch/t"
env -i PATH="$PATH" HOME="$scratch/h" TMPDIR="$scratch/t" LANG=C.UTF-8 TZ=UTC \
  bun --no-env-file --no-install "$work/oracle.ts" "$PWD/cpp/gateway/tests/rt/string_test.cpp"
```

The test sweep uses the unchanged portable CI driver's `write_ninja`, `links.resolve`, `find_boundary` and `run_test` functions through a scratch controller. Each Ninja wave compiles its unresolved translation units once and links the test with its resolved application objects, all 44 runtime objects and the configured simdjson and simdutf archives. It runs each binary in a credential-free loopback-only network namespace with a private tmpfs, `HOME=/tmp/h` and `TMPDIR=/tmp/t`. Release uses `-O2`. ASan/UBSan uses `-O1 -gline-tables-only -fsanitize=address,undefined -fno-omit-frame-pointer`, `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1` and `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`. Both use the strict warnings above and the mold linker. The command and incremental receipts are in runner scratch.

```bash
env -i PATH=/usr/local/bin:/usr/bin:/bin /usr/bin/python3 -B "$work/sweep.py" "$PWD" release 2
env -i PATH=/usr/local/bin:/usr/bin:/bin /usr/bin/python3 -B "$work/sweep.py" "$PWD" asan 2
env -i PATH=/usr/local/bin:/usr/bin:/bin /usr/bin/python3 -B "$work/sweep-resume.py" "$PWD" asan 4
```

Once release finished, the sanitizer controller retained the completed waves and resumed the outstanding waves with four jobs. It checked the source digest, compiler version, compile and link flags, library paths, and selected-test count against its initial run before reusing any result.

The initial sweep linked ICU and SQLite from a directory supplied through `LD_LIBRARY_PATH`. The KV crash fixture starts a child with an explicit environment, and that child exited 127 because the loader could not find ICU. The final sweep links a `DT_RPATH` to the same, verified runtime libraries under `/var/tmp` so the child can start with its intended clean environment. The final run passes that fixture in both profiles.

The fuzz target `string_icu_library` uses three seeds and is registered in `fuzz/targets.json`. The smoke builds the actual harness and string translation unit, with both sanitizers and libFuzzer coverage.

```bash
clang++-21 "${strict[@]}" -O1 -gline-tables-only -fno-omit-frame-pointer \
  -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
  cpp/gateway/src/rt/string.cpp cpp/gateway/fuzz/string_icu_library.cpp \
  "${icu[@]}" -o "$work/fuzz-final/uos_fuzz_string_icu_library"
sudo -n unshare --net -- setpriv --reuid="$(id -u)" --regid="$(id -g)" --clear-groups \
  env -i PATH=/usr/bin:/bin HOME="$work/fuzz-final/h" TMPDIR="$work/fuzz-final/t" LANG=C.UTF-8 TZ=UTC \
  ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer-21 \
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  "$work/fuzz-final/uos_fuzz_string_icu_library" -max_len=16384 -timeout=3 -rss_limit_mb=768 \
  -malloc_limit_mb=128 -seed=17112026 -max_total_time=120 -print_final_stats=1 \
  -artifact_prefix="$work/fuzz-final/artifacts/" "$work/fuzz-final/corpus"
```

## Verification

Source and test identity `ef1128617bc38571dad784a2361a0b129e7e0cc9`, tree `cf5793e00c97a102756c51b34e081e2a6588d38d`. Both profiles compiled all 369 production translation units and passed all 405 application binaries and 4,571 application cases as part of the complete registry. Each receipt has input digest `cde5c54835e8431ccf1128f47e49cef3fea46700a1e0b4d5f3e0a93e9c35fc42` before and after execution. Subsequent commits update the findings and deviation notes.

| Profile             | Passing binaries / selected | Passing cases / run | Failures |
| ------------------- | --------------------------: | ------------------: | -------: |
| Clang 21 release    |                   442 / 442 |       5,027 / 5,027 |        0 |
| Clang 21 ASan/UBSan |                   442 / 442 |       5,027 / 5,027 |        0 |

`sweep-release/receipt.json` has SHA-256 `9b9ed9ede53f5ee932f6c9034309d2934a997e5ea9a089f5ffbf912da8cc47d2`. `sweep-asan/receipt.json` has SHA-256 `84e770ad3e67c759c8521af58c029e8266f56aa801d29eb36d476d6e587d22c6`, with 312 input-matched results retained from the two-job run and 130 completed in the four-job continuation. Both terminal commands exited 0.

The final libFuzzer run completed 3,036,331 inputs in 121 seconds, reached 154 instrumented edges and 622 features, and exited with zero findings or artifacts. Peak RSS was 445 MiB. The SHA-256 of `fuzz-final/smoke.log` is `9d2bae7a493862a589f084bf6f02629ff03f1f78e6050afd5e3f756e4996c2b6`.

The SDK CMake registration found ICU 78.3, generated a 442-test registry and built the string object using the runner's compiler. An independent GCC 14.2 strict compile of `src/rt/string.cpp` also passed. All three legacy string cases passed with `LANG` and `LC_ALL` set to `tr_TR.UTF-8`.

```bash
env UOS_GATEWAY_BUILD_EVIDENCE=runner-disposable cmake -S cpp/gateway -B "$work/cmake-sdk-probe" -G Ninja \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++-21 -DUOS_GATEWAY_IS_SDK=ON -DICU_ROOT="$work/icu78" -DUOS_GATEWAY_TESTS=ON
cmake --build "$work/cmake-sdk-probe" --target uos_obj_08617ca7441f4e6a -j1
g++-14 "${strict[@]}" -c cpp/gateway/src/rt/string.cpp -o "$work/new-string-gcc14.o"
python3 -B cpp/gateway/tools/links.py --check
git diff --check 093a9e05..HEAD
```

The changed Markdown and fuzz registry passed `bun --no-env-file run format:check cpp/gateway/docs/findings/cppgw-std-icu-20261117.md cpp/gateway/docs/DEVIATIONS-FROM-BUN.md cpp/gateway/fuzz/targets.json` in scratch with frozen root and lint dependencies.

The historical SDK sysroot and macOS were unavailable on this runner. The old `tools/unit.sh` SDK wrapper still lists only Boost and OpenSSL, and `docs/rt/text.md` still describes the old implicit order. Both are outside this lane's ownership.

The working branch is `agent-job` for collection into `claude/cpp-gateway-port`. `git push --dry-run origin HEAD:refs/heads/cppgw-std-icu-20261117` failed because GitHub write credentials were unavailable.
