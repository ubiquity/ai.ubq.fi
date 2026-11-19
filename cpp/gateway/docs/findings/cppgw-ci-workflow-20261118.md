# C++ gateway CI on 2026-11-18

## Work contract v1

Run the portable Release driver with Clang on Ubuntu 26.04 ARM64 in four round-robin shards. Pushes to `claude/cpp-gateway-port`, `pool-merge/**` and `agent-jobs/**`, and PRs targeting `claude/cpp-gateway-port`, exercise the same suite. Each shard publishes the driver receipt, incremental results, and diagnostic logs with a short summary.

The work owns `.github/workflows/cpp-gateway-ci.yml`, `cpp/gateway/tools/ci-build.sh`, `cpp/gateway/cmake/ci/*.py`, and the two CI documents. First-party warning policy, exact Links closures, fixture namespaces, and read-only workflow permissions are invariants. Build data stays outside the checkout. Deployment and changes to the gateway, its test fixtures, and other workflows are outside the task scope.

The local qualification uses Ubuntu 26.04 x86_64 with Clang 21 and SQLite 3.54.0. It covers shard 1 and at least one additional shard. The host architecture can fail the application-level ARM64 fixtures documented in [the earlier run](cppgw-ci-build-x86-20261117.md). Any other failures need their first diagnostic and a driver fix or an out-of-scope finding. Completion requires terminal shard counts, a validated workflow, and a clean checkout. The result-branch push is the entry point for hosted ARM64 evidence.

## Initial findings

- Starting commit `45118332b287b7ba9534bb66ab1518f1990a9d9e` discovered 441 test binaries. Git recorded the wrapper as `100644`, causing the hosted jobs to exit 126 before the driver ran.
- The portable driver already reads the pinned vendor registry, builds the selected simdjson and simdutf static archives without the first-party warning flags, and links them into matching closures. Clang diagnosed `simdutf.h:2029:23: error: implicit conversion changes signedness` when compiling `src/rt/text_codec.cpp` without `--system-header-prefix=third_party/`. The same strict compile passed with the prefix. Ninja explicitly tracks the vendor headers for their registered consumers because `-MMD` excludes system headers.
- The workflow originally searched for `results*.json`. The driver creates `receipt.json`, `results.tsv`, compiler logs, boundary probe logs, and each binary's link/run logs.

## Workflow changes

- The wrapper is executable in Git and the workflow invokes it with Bash. The Bash step retains the driver's exit status through `tee` while preserving console output in `driver.log`.
- Push selection uses branch filters alone so every push to an agent result branch is eligible, including pushes that change paths outside the gateway. The pull request trigger uses the C++ port as its base branch.
- The upload runs after failures and takes actual receipt, TSV and log paths under `UOS_CI_BUILD_DIR`. The summary reads the Release receipt and counts successful links, including binaries that later fail or time out, and lists every unsuccessful target. If the driver exits before writing the receipt, the summary directs readers to the available diagnostics.

## Local commands

```sh
sudo apt-get update -qq
sudo apt-get install -y -qq clang ninja-build pkg-config mold util-linux libboost-json-dev libboost-container-dev \
  libssl-dev libsqlite3-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev
clang++ --version
mkdir -p "$RUNNER_TEMP/sqlite" && cd "$RUNNER_TEMP/sqlite"
curl --fail --silent --show-error --location https://www.sqlite.org/2026/sqlite-autoconf-3540000.tar.gz -o sqlite.tar.gz
echo "134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed  sqlite.tar.gz" | sha256sum --check
tar -xzf sqlite.tar.gz && cd sqlite-autoconf-3540000
./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3' > /dev/null
make -j4 > /dev/null && sudo make install > /dev/null && sudo ldconfig
pkg-config --modversion sqlite3
```

The disposable local preparation used `/tmp/cppgw-ci-workflow-20261118.DENd3y` in place of `$RUNNER_TEMP`. The archive hash matched, `pkg-config` found SQLite 3.54.0, and `/usr/local/bin/sqlite3 :memory: 'select sqlite_version(), "double quoted string";'` returned `3.54.0|double quoted string`. The compiler is Ubuntu Clang 21.1.8.

From the checkout root:

```sh
for shard in 1 2 3 4; do
  CXX=clang++ UOS_CI_JOBS=4 bash cpp/gateway/tools/ci-build.sh --config release --shard "$shard/4"
done
CXX=clang++ UOS_CI_SELFTEST_CONFIG=release python3 -B cpp/gateway/cmake/ci/test_driver.py
bash -n cpp/gateway/tools/ci-build.sh
shellcheck cpp/gateway/tools/ci-build.sh
git diff --check 45118332..HEAD
```

The four-way inventory covers all 441 targets once. All seven Clang Release driver controls passed. Python syntax checks and `bun --no-env-file run format:check` on the changed workflow and documents passed, using the repository's frozen manifests and Bun 1.4.0 in an external scratch directory. Actionlint 1.7.11 passed with an external config recognizing the runner label `ubuntu-26.04-arm`, which its built-in list did not yet recognize.

## Production compile blocker

The first run reached the gateway objects and found this diagnostic in `cpp/gateway/src/rt/crypto.cpp`:

```text
crypto.cpp:839:14: error: use of undeclared identifier 'HEX'
crypto.cpp:840:14: error: use of undeclared identifier 'HEX'
```

The starting commit removed the lowercase hex alphabet alongside the old `toHex` function. The unguarded `randomUUID()` implementation still indexes that alphabet. Every standalone test links all runtime objects, so this one compilation failure blocks all binary links on x86_64 and ARM64. A source change requires maintainer authorization beyond the CI paths. The CI driver preserves the build failure in the final receipt and the per-object compiler log.

## Requested Release commands

Each command completed with exit 1. The driver built both vendor archives and 368 of 369 production objects for each selection. It reported the remaining `src/rt/crypto.cpp` error before linking any test binaries.

| Shard | Targets selected | Binaries built | Passed | Build failures | Cases passed | Cases failed |
| ----- | ---------------: | -------------: | -----: | -------------: | -----------: | -----------: |
| 1/4   |              111 |              0 |      0 |            111 |            0 |            0 |
| 2/4   |              110 |              0 |      0 |            110 |            0 |            0 |
| 3/4   |              110 |              0 |      0 |            110 |            0 |            0 |
| 4/4   |              110 |              0 |      0 |            110 |            0 |            0 |

The cold shard 1 object build took 742.919 seconds. Its starting commit was `4210939634d6748ca55816ea6a71e68bc2b5ebc5`, and the summary changes made in flight invalidated its input receipt. The shard 1 rerun and shards 2–4 recorded unchanged inputs on commit `fca804c22437f5ea5f85e68fad2a43ad00ed8bae`, tree `b12724acbe6ef5aa1640bf439296909c58ed172e`, and source digest `895c50dc00d17206f969f8969add4422f8784427a0bcd2e6878b9fb86ff02ccb`. Only the cold shard started with an empty object cache.

The local shard 1 rerun executed the workflow's actual Bash build step with `-eo pipefail`, then executed its summary step. The build exited 1 through `tee`. The upload path expressions matched one receipt, one TSV, and the driver and nested logs. The summary reported `0 built, 0 passed, 111 failed, 0 cases passed, 0 cases failed` and listed the 111 affected targets.

## Runtime diagnostic

A separate probe reused the generated objects, vendor archives, driver link closures, and namespace runner in a scratch tree. It compiled just `src/rt/crypto.cpp` with the receipt's exact Clang flags plus `-DHEX="0123456789abcdef"`, the value removed by the starting commit. The probe linked and ran all 441 binaries. Its results are diagnostic evidence for the existing runtime and fixtures. The committed workflow uses the ordinary compiler flags.

```sh
work=/tmp/cppgw-ci-workflow-20261118.DENd3y
python3 -B "$work/diagnostic.py" /tmp/uos-gateway-build/clang++-8ca21d960a13/release "$work/hex-diagnostic"
```

The probe's receipts, exact crypto compile command, and per-binary link and run logs are under `$work/hex-diagnostic`. The ordinary driver receipts and logs are under `/tmp/uos-gateway-build/clang++-8ca21d960a13/release`, including `history/` for rotated runs.

| Shard | Binaries built | Passed | Failed | Cases passed | Cases failed |
| ----- | -------------: | -----: | -----: | -----------: | -----------: |
| 1/4   |            111 |    107 |      4 |        1,286 |            5 |
| 2/4   |            110 |    106 |      4 |        1,028 |            4 |
| 3/4   |            110 |    105 |      5 |        1,236 |            6 |
| 4/4   |            110 |    106 |      4 |        1,264 |           20 |
| Total |            441 |    424 |     17 |        4,814 |           35 |

There were zero link failures and timeouts. The simdjson-backed JSON and simdutf-backed Base64, string, and text codec unit tests passed all 61 cases. The SQLite suite passed all 29 cases.

The documented ARM64 checks account for 23 failed cases in two binaries:

| Test                                    | Failed cases | First error                                                         |
| --------------------------------------- | -----------: | ------------------------------------------------------------------- |
| `tests/permissions_linux_test.cpp`      |            5 | `uncaught exception: Linux confinement requires Linux ARM64`        |
| `tests/runtime_native_startup_test.cpp` |           18 | `uncaught exception: Native KV development platform is unsupported` |

The other 15 failing binaries had the following first diagnostics. The four binaries ending with signal 11 had no case summary. Several assertions still expect detailed Bun parser diagnostics where the current simdjson behavior returns `Invalid JSON`. [Parser behavior](../DEVIATIONS-FROM-BUN.md)

| Shard | Test                                                     | First error or mismatch                                                                                                                                        |
| ----- | -------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1/4   | `tests/anthropic_replay_test.cpp`                        | Line 483 failed equality on stringified restored entries. `FAIL differential: restored payloads carry Bun's canonical key order`.                              |
| 1/4   | `tests/deepseek_stream_differential_test.cpp`            | Malformed SSE cause `Invalid JSON`, expected `JSON Parse error: Expected '}'`.                                                                                 |
| 1/4   | `tests/jev_compaction_differential_test.cpp`             | `uncaught exception: Invalid JSON` in `noulAnswer matches the recorded answers and errors`.                                                                    |
| 1/4   | `tests/native_addon_differential_test.cpp`               | `Invalid JSON`, expected `JSON Parse error: Property name must be a string literal`.                                                                           |
| 2/4   | `tests/claude_code_models_differential_test.cpp`         | `Invalid JSON`, expected `JSON Parse error: Expected '}'`.                                                                                                     |
| 2/4   | `tests/kernel_attestation_differential_test.cpp`         | `Unauthorized: failed to parse kernel attestation JWT parts: Invalid JSON`, expected `Unauthorized: kernel attestation JWT 'alg' MUST be 'RS256', got 'null'`. |
| 2/4   | `tests/kv_conversion_legacy_codec_differential_test.cpp` | `Invalid JSON`, expected `JSON Parse error: Unexpected identifier "not"`.                                                                                      |
| 2/4   | `tests/native_codex_429_differential_test.cpp`           | Line 54 expected recorded digest `b73ef4a13b381702`, observed `16605f5f878cd9e7`.                                                                              |
| 3/4   | `tests/compaction_summary_forwarding_test.cpp`           | `terminate called after throwing an instance of 'uos::rt::SyntaxError'`, `what(): Invalid JSON`, signal 11.                                                    |
| 3/4   | `tests/native_authority_test.cpp`                        | `terminate called after throwing an instance of 'uos::rt::SyntaxError'`, `what(): Invalid JSON`, signal 11.                                                    |
| 3/4   | `tests/native_principal_control_test.cpp`                | `terminate called after throwing an instance of 'uos::rt::SyntaxError'`, `what(): Invalid JSON`, signal 11.                                                    |
| 3/4   | `tests/passkeys_webauthn_differential_test.cpp`          | Passkey assertion warning `Invalid JSON`, expected `JSON Parse error: Unterminated string`.                                                                    |
| 4/4   | `tests/anthropic_chat_stream_differential_test.cpp`      | Malformed SSE cause `SyntaxError: Invalid JSON`, expected `SyntaxError: JSON Parse error: Expected '}'`.                                                       |
| 4/4   | `tests/harmony_differential_test.cpp`                    | `normalizeToolArguments` returned `"1e400"`, expected `"null"`.                                                                                                |
| 4/4   | `tests/native_runtime_binding_test.cpp`                  | `terminate called after throwing an instance of 'uos::rt::SyntaxError'`, `what(): Invalid JSON`, signal 11.                                                    |

## Hosted status

The target ref is `refs/heads/agent-jobs/cppgw-ci-workflow-20261118`. The Git remote was readable, but the push preflight failed with `could not read Username for 'https://github.com': terminal prompts disabled`. Hosted ARM64 evidence is pending branch delivery.
