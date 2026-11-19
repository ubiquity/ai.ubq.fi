# Shrink C++ gateway numeric helpers

## Contract v1

Retire `parseFloat`, `numberToString(double, int)` and `toFixed` after migrating their remaining callers to bounded types. Preserve Codex session UUID bytes, decimal JSON and persisted key bytes, numeric coercion and validation, and BigInt. The provider matrix has one touched path. Codex dispatch derives the same session identity from the same SHA-256 material. Provider selection, authentication, accounting, TypeScript, dependencies and deployment are outside this change.

Production edits are confined to the three numeric API regions and their private dead code in `src/rt/number.cpp` and `include/uos/rt/number.hpp`, and to the UUID nibble operation in `src/codex/dispatch.cpp`. The directly affected numeric fixtures, the deviation notes and this report are the remaining write paths.

The starting commit is `9b4cc9aa25ddaa8a229fdc480d949931fbcaceca`, tree `2fdf141d7ba938ec52a1cdf8e2d52c87a1812eae`. Its registry contains 442 binaries. The BigInt handoff records 441 passing binaries and 5,027 passing cases out of 5,028 in both profiles after its ICU and sanitizer controls. The inherited failure is the Number fixture's BigInt expectations for incomplete signed and prefixed tokens. Starting production line counts are 758 in `src/rt/number.cpp`, 246 in `include/uos/rt/number.hpp` and 883 in `src/codex/dispatch.cpp`. [BigInt handoff](cppgw-std-bigint-20261117.md#terminal-verification-and-handoff)

The baseline command is `bash cpp/gateway/tools/ci-build.sh --config release`. The final gates use the same driver in release and `asan` modes. The runtime object in every link list makes all 442 registered tests in scope. The Number, JSON, KV, Codex dispatch and API key policy receipts are tracked individually. Tests run on Linux ARM64 with the driver's credential-free namespace setup. Changed documents use pinned Prettier and the patch uses `git diff --check`.

Runner-specific unknowns are the baseline failure inventory and cold build times. Budget 45 minutes for the baseline and 75 minutes for the sanitizer compile and sweep. Track compiled objects and completed binaries in the driver's logs, use incremental objects for retries and retain prior failure evidence. Stop after both final profiles match the baseline and the committed scope, counts, timings and any handoff blockers are recorded.

## Implementation

Codex dispatch decodes digest byte 16, keeps the low two bits and sets the high two bits to `10`. The existing differential vectors still assert exact session bytes. Two additional inputs cover variant digits `8` and `a` in the same case, alongside the existing `9` and `b` inputs.

Removed the unused float prefix API, `Number` radix and fixed-decimal formatters, and private fractional radix arithmetic. Kept the whole-string decimal scan used by `stringToNumber`, the decimal formatter, and the BigInt radix constants. The bounded test fingerprints, a synthetic Codex hash and SQLite code point fixtures now format unsigned integers with `std::to_chars` or select a single hex digit directly.

The three migrated Number cases exercise 132 integral radix vectors through BigInt, 252 distinct former radix and fixed inputs through JSON serialization, and all 38 former prefix inputs through whole-string coercion. Bun 1.4.0 supplied the new `JSON.stringify` and `Number` expected values in a fresh network namespace with a sparse environment and empty home. The existing Number, Math, safe-integer and BigInt cases retain their assertions. The registry still contains 442 test binaries.

The production diff adds 4 lines and removes 123. The three changed production files go from 1,887 to 1,768 lines. The 739 first-party files under `cpp/gateway/src` and `cpp/gateway/include` go from 190,675 to 190,556 physical lines, a reduction of **119**. On the same Clang 21 `-O2` flags, `size` reports 96,324 to 88,097 bytes in the Number object's text column. `nm -C --defined-only` confirms the three retired exports are absent and the decimal formatter, `parseInt` and `stringToNumber` remain.

## Runner and commands

The runner is Ubuntu 24.04.5 on Linux ARM64 with four CPUs. It uses Clang 21.1.8 from `apt.llvm.org`, mold 2.30.0, Boost 1.92.0, OpenSSL 3.0.13, SQLite 3.54.0, zlib 1.3, ICU 78.3, Brotli 1.1.0 and Zstd 1.5.5. Boost.JSON and Boost.Container were built statically in scratch with the following command after verifying the published archive SHA-256 `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c`.

```bash
./bootstrap.sh --with-libraries=json,container
./b2 -j2 --with-json --with-container toolset=gcc variant=release link=static threading=multi \
  --stagedir="$scratch/boost" --build-dir="$scratch/boost-build" stage
```

SQLite used the SHA-256 and `CFLAGS='-O2 -DSQLITE_DQS=3'` configuration from the CI guide with `--prefix="$scratch/sqlite"`. ICU used the 78.3 source release verified against its `SHASUM512.txt` and `./configure --prefix="$scratch/icu-install" --disable-tests --disable-samples`. Both libraries were available through a temporary `/opt` loader directory registered with `ldconfig`, including SQLite's unversioned library name. ICU 78.3 also addresses the starting branch's documented failures with the runner's ICU 74.2. [Portable CI guide](../CI.md) [ICU release](https://github.com/unicode-org/icu/releases/tag/release-78.3)

The first baseline compile reported Clang `-Wsign-conversion` errors inside vendored `simdutf.h`, included by `src/rt/base64.cpp`. A scratch Ninja wrapper adds `--system-header-prefix=third_party/simdutf/` only to that compile rule. The resumed driver reuses the completed objects and keeps its fatal first-party warnings. The checked-in driver and vendor sources retain their bytes. Scratch wrapper SHA-256 is `214582b486e7caf94dd46a01869bffe3dadbffb6264a3e96f6410af307d1d7b3`.

```bash
scratch=/tmp/cppgw-slim-numbers.S20xEJ
git worktree add --detach "$scratch/qualification" 9b4cc9aa25ddaa8a229fdc480d949931fbcaceca
bash "$scratch/run-suite.sh" "$scratch/qualification" release
nice -n 5 bash "$scratch/run-suite.sh" "$PWD" asan
git -C "$scratch/qualification" switch --detach 0dfd9055865e66e7a2a39787227b5dd2f0843af3
bash "$scratch/run-suite.sh" "$scratch/qualification" release
```

`run-suite.sh` enters the selected checkout and runs the driver with `env -i`, `CXX=clang++-21`, `LANG=C.UTF-8`, `TZ=UTC`, four workers and a 600-second test limit. `PKG_CONFIG_PATH` points to the pinned SQLite and ICU installs. `UOS_CI_CXXFLAGS` adds the Boost include root through `-isystem`, and `UOS_CI_LDFLAGS` adds its static archive directory. Sanitizer flags also include `-gline-tables-only -fno-sanitize-recover=undefined` alongside the driver's `-O1` ASan and UBSan profile. The driver invokes the executable in a fresh Linux network, mount and PID namespace with verified loopback, a private tmpfs, empty `HOME` and fresh `TMPDIR` for each binary.

The direct changed-unit syntax check used `clang++-21 -std=c++20 -O2 -Wall -Wextra -Wshadow -Wconversion -Werror -fsyntax-only` with the portable driver definitions and Boost headers. `python3 -B cpp/gateway/tools/links.py --check`, changed-document Prettier from the frozen lint lock and `git diff --check` passed.

## Verification

The implementation commit is `0dfd9055865e66e7a2a39787227b5dd2f0843af3`, tree `8ee5aae1b4b42cb841597284a19fd9d238f2792e`. The isolated release Number control ran both the starting and patched test using `python3 -B "$scratch/number-control.py" "$scratch" "$PWD"` with the same runtime link closure. Each passed 20/21 cases. Each failed at `BigInt(string) follows JSC StringToBigInt` on the inherited incomplete-token expectation. All three migrated Number cases passed.

The early release control reused unchanged baseline objects and compiled the changed sources on the implementation tree. Codex dispatch passed 13/13 including all four UUID variant digits, JSON passed 25/25, KV passed 53/53, SQLite passed 29/29 and API key policy passed 1/1. The driver run is `python3 -B "$scratch/focus-release.py" "$scratch" "$PWD" tests/codex_dispatch_differential_test.cpp tests/rt/sqlite_test.cpp tests/rt/json_test.cpp tests/rt/kv_test.cpp tests/api_key_policy_differential_test.cpp` with the same isolated settings as the Number control.

The first full baseline release compiled all 369 production objects and ran 442 tests. It reported 414 passing binaries and 4,897 passing cases out of 5,028 because the private `/tmp` hid the detached checkout from fixtures that read checked-in files via `__FILE__`. For example, all 18 native startup cases could not open the tracked `tools/native-kv/inputs.json`. A scratch namespace helper bind-mounts the detached checkout read-only at the same path within the private tmpfs. The network, process and fresh home checks remain in force. No checked-in file is changed by the workaround.

The qualified baseline rerun is `env -i PATH="$scratch/tool-bin:/usr/local/bin:/usr/bin:/bin" HOME="$scratch/home" TMPDIR="$scratch/tmp" LANG=C.UTF-8 TZ=UTC python3 -B "$scratch/recover-baseline.py" "$scratch"`. It verifies the exact original input SHA-256 `173c881ddb9a3698a4387278b11f2c247ddcf3cb161a0cf038f1087d0be05416`, reuses original objects by timestamp bound to the first receipt, rebuilds objects from the interrupted candidate build, and links and runs all 442 fixtures with the portable driver's test functions. Recovery script SHA-256 is `6fd3c5bff6316739e27ea9a7fbf8aa3934a86473df8f0636314850f3e6269706`. The queued release command uses the same read-only mount. The sanitizer command runs from the repository-root checkout, which is outside `/tmp`.

The corrected baseline reused 770 objects, rebuilt 62 in 161.809 seconds, and linked and tested in 87.482 seconds. It passed **441/442 binaries and 5,027/5,028 cases** with zero build failures and timeouts. The inherited BigInt parser assertion is the sole failure. Its receipt SHA-256 is `6d66c1ea36ddb2fc8e5578840fcbb1e11c99c01ae8d24c5cff0fb79cf976d0a3`.

The candidate release run completed the 369-object production build, recorded **441/442 passing binaries and 5,027/5,028 passing cases**, and matched the corrected baseline on every binary's case counts and status. Its incremental object phase took 2,262.056 seconds and its link and test phase 91.248 seconds. The receipt SHA-256 is `62107ab2eb269c25a3483a691001c5ec700429a133ffd4654f30b012a5e25e52`.

The candidate ASan and UBSan run completed the same production build and passed **441/442 binaries and 5,027/5,028 cases**, with zero sanitizer diagnostics, build failures or timeouts. Its cold object phase took 4,986.812 seconds and its link and test phase 180.283 seconds. The receipt SHA-256 is `73007690e498a006fd07ea575dfd0c02830d2a6be2cc624080cf1f124e54a190`.

`python3 -B "$scratch/compare-receipts.py" "$scratch"` verified the recorded commit and tree for each run and checked every binary's status and case counts. All 442 rows agree across the baseline, candidate release and candidate sanitizer runs. The only failure is the inherited BigInt assertion in `tests/rt/number_test.cpp`. Both candidate runs recorded the same input SHA-256 `ef523eabef62f11b3793ea321430e87f6cd3cdfa4216be51a2e9f9dd113eca79` at start and finish. The subsequent commits update only this findings report.

| Fixture                                | Release cases passed | ASan and UBSan cases passed |
| -------------------------------------- | -------------------: | --------------------------: |
| `rt/number_test.cpp`                   |                20/21 |                       20/21 |
| `rt/json_test.cpp`                     |                25/25 |                       25/25 |
| `rt/kv_test.cpp`                       |                53/53 |                       53/53 |
| `codex_dispatch_differential_test.cpp` |                13/13 |                       13/13 |
| `api_key_policy_differential_test.cpp` |                  1/1 |                         1/1 |
| `rt/sqlite_test.cpp`                   |                29/29 |                       29/29 |

## Handoff

`docs/rt/values.md` still lists the retired C++ APIs and needs the shared runtime documentation update after integration.

The implementation and verified receipts are committed on `agent-job`. `GIT_TERMINAL_PROMPT=0 git push --dry-run --porcelain origin HEAD:refs/heads/agent-jobs/cppgw-slim-numbers-20261118` failed because GitHub write authentication is unavailable. Branch publication and PR integration remain pending. The fetched `origin/development` at `2ac5aae69b0f0bac77cf1c6f329995cf1187616c` lacks the implementation in its ancestry and has no `cpp/gateway` tree.
