# C++ gateway sanitizer and robustness CI

## Work contract v1

Extend the Ubuntu 26.04 ARM64 gateway checks to run the same test inventory in Release and with ASan, UBSan and LeakSanitizer. Discover every robustness target from `fuzz/targets.json`. Pull requests to `claude/cpp-gateway-port` and pushes to agent result branches run five-minute campaigns. Pushes to the port branch run 40-minute campaigns.

Keep the existing Release triggers, checks, fixture isolation, read-only workflow permissions, per-ref cancellation and evidence uploads. Limit changes to the owned C++ workflow, CI driver, runner and documentation paths. The task explicitly calls for removal of the retired `tests/ci-sanitize.txt`. Application code, test fixtures, the robustness registry and deployment are outside this change.

Qualify the workflow with `actionlint`, a cold full-inventory ASan shard and two five-minute robustness campaigns on the disposable x86_64 Linux runner. Use the driver's extra compiler flags to supply the missing `HEX` constant for local qualification. Record inventory coverage, exact commands, timings, exits and any upstream defects. Retain namespace isolation and run with fresh home and temporary directories and a credential-free environment. Four shards for both configurations are the measurement candidate within the 240-minute job limit.

The parent checkout starts at `1769741ee6d408f214c9c9dd4d8bcc7f5180a904`, tree `60acf0c92c9dbc99c34cbacb3f65478ec1192472`. The known `src/rt/crypto.cpp` compile blocker and the existing x86_64-specific fixture failures are outside the owned paths. Checkout and hosted delivery status will be recorded after the local gates finish.

## Continuation and measurement plan

The continuation starts at `5705ff938c90303b291a80f817cb29a10ce97e6f`, tree `9fae04d838c685dc52eda38aaaf17b7864b67923`. It retains the earlier commit that makes `summary.py` render either suite configuration. Ubuntu Clang 21.1.8 accepted the inherited `src/rt/crypto.cpp` in a strict syntax check with the local `HEX` definition. SQLite 3.54.0 was built from the SHA-256-checked source using the same options as CI. The cold ASan shard started on the inherited tree with four compiler workers and the genuine Linux fixture boundary via passwordless sudo.

The cold run will produce one driver receipt and per-target outcomes. The working estimate is 30–60 minutes, with Ninja progress, object counts and incremental test results in scratch. Run the JSON and regex campaigns on the same disposable host, with four workers and a budget of 300 seconds each after compilation. Keep their build and campaign timings separate from the suite and record any overlap. A failed setup or build must surface its diagnostics. A failing gateway test or harness finding will be reported to its owning lane without changing source or assertions.

The unified workflow will select the complete inventory in both configurations and fan out from the robustness registry. Agent pushes, port PRs, pool merges and manual dispatch will use 300 seconds; port branch pushes will use 2,400 seconds. The existing fork-exit leak and regex timeout exceptions remain visible in campaign evidence. A libFuzzer exit without a completed campaign or classified finding must fail the job.

The initial full build applied `-DHEX` to every translation unit. The fuzz build showed that it also rewrites the valid `HEX` declaration in `src/rt/console.cpp`. `src/jev_compaction/hybrid.cpp` has another local `HEX` declaration. Both initial runner processes were stopped, and the retry uses an external `clang++` wrapper that adds the definition when compiling `src/rt/crypto.cpp` alone. The ordinary commands in CI use unmodified Clang.

## Implemented checks

- The single ARM64 workflow runs the Release and sanitizer suite matrices and discovers one robustness job per registry entry. Both four-way splits contain the same 441 binaries, partitioned 111/110/110/110 with no duplicates. The registry step selected 11 harnesses.
- Actions use `contents: read`, check out with `persist-credentials: false`, cancel older runs for a ref and retain a summary and other available evidence for each job. The fuzz jobs run inside a private Linux network namespace with privileges dropped, an empty home, a fresh temporary directory and an allowlisted environment.
- The previous sanitizer subset runner, its selection list and both superseded workflows were removed. libFuzzer now stops on unexpected crashes, timeouts or out-of-memory errors. Successful campaigns must reach their requested duration, and the summary distinguishes expected regex timeouts and ARM64 fork-parent leaks from other artifacts.

`actionlint` 1.7.11 passed on the unified workflow with an external config for the `ubuntu-26.04-arm` label. All seven ASan driver controls passed in 28.226 seconds with the actual namespace boundary. The workflow's registry and summary commands passed local Bash controls. Twelve synthetic classification controls covered completed and interrupted campaigns, regex timeout policy and the ARM64 fork-exit exception. Python syntax, `bash -n`, `shellcheck` and `git diff --check` passed.

## Qualification commands

The successful setup installed Clang 21.1.8, LLVM's symbolizer, Ninja, mold, util-linux and the native library headers. The SQLite build followed the CI workflow with `make -j4`. The `pkg-config --modversion sqlite3` check returned `3.54.0`. The scoped retry began on `a29d7b41d8dbf5ab20c48e2297dd9a1a38db4ffa`, tree `a41d35e886b73cf4030129955cfc47298244f4a7`, with no C++ source edits. The local-only compiler wrapper was placed before the system compiler on `PATH`.

```sh
scratch=/tmp/cppgw-ci-sanitizers.orvDxOy4
mkdir -p "$scratch/compiler"
cat > "$scratch/compiler/clang++" <<'SH'
#!/bin/sh
for argument do
  case "$argument" in
    */src/rt/crypto.cpp) exec /usr/bin/clang++ "$@" '-DHEX="0123456789abcdef"' ;;
  esac
done
exec /usr/bin/clang++ "$@"
SH
chmod +x "$scratch/compiler/clang++"
```

The Bash steps used `-eo pipefail`. The cold shard and the two sequential campaigns had separate outputs and overlapped on the same four-core runner.

```sh
export UOS_CI_BUILD_DIR="$scratch/asan-scoped-build"
CONFIG=asan
SHARD=1
mkdir -p "$scratch/asan-scoped-home" "$scratch/asan-scoped-tmp" "$UOS_CI_BUILD_DIR"
env -i PATH="$scratch/compiler:/usr/local/bin:/usr/bin:/bin" LANG=C.UTF-8 HOME="$scratch/asan-scoped-home" \
  TMPDIR="$scratch/asan-scoped-tmp" CXX=clang++ UOS_CI_JOBS=4 UOS_CI_BUILD_DIR="$UOS_CI_BUILD_DIR" \
  bash cpp/gateway/tools/ci-build.sh --config "$CONFIG" --shard "$SHARD/4" 2>&1 | tee "$UOS_CI_BUILD_DIR/driver.log"

```

```sh
FUZZ_SECONDS=300
for FUZZ_TARGET in json regex; do
  fuzz_out="$scratch/fuzz-scoped-$FUZZ_TARGET"
  mkdir -p "$fuzz_out/home" "$fuzz_out/tmp"
  sudo -n unshare --net -- setpriv --reuid="$(id -u)" --regid="$(id -g)" --clear-groups --no-new-privs \
    env -i PATH="$scratch/compiler:/usr/local/bin:/usr/bin:/bin" LANG=C.UTF-8 HOME="$fuzz_out/home" TMPDIR="$fuzz_out/tmp" \
    python3 -B cpp/gateway/fuzz/ci.py "$FUZZ_TARGET" --seconds "$FUZZ_SECONDS" --jobs 4 --out "$fuzz_out" \
    2>&1 | tee "$fuzz_out/build.log"
done
```

## Local results

Both four-worker harness campaigns reached the 300-second budget and exited 0 on x86_64. The builds overlapped the sanitizer suite build.

| Harness | Build and link | Campaign wall | libFuzzer duration | Last execution count | Seed / final corpus files | Expected slow inputs | Unexpected findings |
| ------- | -------------: | ------------: | -----------------: | -------------------: | ------------------------: | -------------------: | ------------------: |
| `json`  |      469.825 s |     309.935 s |              309 s |            4,390,783 |                   9 / 997 |                    0 |                   0 |
| `regex` |      440.423 s |     317.481 s |              317 s |            2,742,951 |                 6 / 2,157 |                    7 |                   0 |

The seven slow inputs were retained as artifacts. The runs reported zero fork-parent exit leaks. The ARM64 exception was exercised by synthetic classification controls. The workflow summary step rendered both real receipts and the artifact counts matched. The campaign receipts and logs are in `$scratch/fuzz-scoped-json` and `$scratch/fuzz-scoped-regex`.

## Final continuation

The checkout resumed at `c75cd79e2b7d8b0a21e8f68534f8516d549da07f`, tree `d96fb32987663d5a130147d97c00a82bf8dbd3fe`. The sanitizer process had stopped before running tests. Its last Ninja line recorded step 443 of 501 at 3,207.012 seconds. The resumed command below reuses those objects and the existing compiler wrapper, with fresh home and temporary directories. Both parts used the same driver, C++ inputs and Clang flags. The original logs were preserved in `$scratch/asan-resume`.

```sh
set -o pipefail
export UOS_CI_BUILD_DIR="$scratch/asan-scoped-build"
out="$scratch/asan-resume"
CONFIG=asan
SHARD=1
mkdir -p "$out/home" "$out/tmp"
env -i PATH="$scratch/compiler:/usr/local/bin:/usr/bin:/bin" LANG=C.UTF-8 HOME="$out/home" TMPDIR="$out/tmp" \
  CXX=clang++ UOS_CI_JOBS=4 UOS_CI_BUILD_DIR="$UOS_CI_BUILD_DIR" \
  bash cpp/gateway/tools/ci-build.sh --config "$CONFIG" --shard "$SHARD/4" 2>&1 | tee "$UOS_CI_BUILD_DIR/driver.log"
```

`actionlint` 1.7.11 passed again with the external ARM runner-label config. The union of the four `--list` outputs matched the unsharded list for each configuration. The seven ASan driver controls passed again in 14.757 seconds, and all twelve campaign classification controls passed. `bun --no-env-file run format:check` passed for the active workflow and both changed documents in the locked scratch formatter checkout.

```sh
"$scratch/actionlint/actionlint" -config-file "$scratch/actionlint/config.yaml" .github/workflows/cpp-gateway-ci.yml
CXX=clang++ UOS_CI_SELFTEST_CONFIG=asan python3 -B cpp/gateway/cmake/ci/test_driver.py
python3 -B cpp/gateway/cmake/ci/summary.py "$UOS_CI_BUILD_DIR" --config asan
git diff --check 1769741e..HEAD
```

## ASan shard 1 receipt

On four x86_64 cores with Clang 21.1.8, the resumed command completed all 58 remaining Ninja steps. Compilation finished with all 369 production objects and both vendor archives. All 111 selected tests linked and ran with ASan, UBSan and `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1` under the real Linux namespace boundary. The job summary reported 107 passing and four failing binaries, with 1,286 passing and five failing cases. There were zero build failures, timeouts or sanitizer diagnostics. The driver exited 1 for the failed assertions.

The last recorded Ninja step before interruption was at 3,207.012 seconds. The resumed object build took 435.977 seconds and linking and testing took 48.775 seconds, for a 485.111-second resumed invocation including setup and receipt creation. The recorded work across both segments totals 3,692.123 seconds (61 minutes 32 seconds), excluding the interruption and dependency installation. Some of the first segment overlapped the robustness jobs. Four shards leave ample room under the 240-minute job limit on this measurement. ARM64 shard timings still need hosted receipts.

The driver wrote its terminal receipt to `$scratch/asan-scoped-build/clang++-ef45513c90b3/asan/receipt.json` (SHA-256 `93732a810244b38311a6b43f6aa77391559e4a249b35e68eab1643488fed72c9`). It binds commit `c75cd79e2b7d8b0a21e8f68534f8516d549da07f`, tree `d96fb32987663d5a130147d97c00a82bf8dbd3fe`, and unchanged driver input digest `ba34aa4f8d4f05e064ed711cd944f0beef96aaada4f7f164501ad20b9d1c27d0`. The workflow summary command rendered the terminal receipt and the four failing binaries.

## Findings outside the CI paths

- `tests/anthropic_replay_test.cpp` failed one assertion on restored payload key order.
- `tests/deepseek_stream_differential_test.cpp` failed two malformed SSE assertions that expect the Bun parse error text.
- `tests/jev_compaction_differential_test.cpp` failed one recorded `noulAnswer` case with `Invalid JSON`.
- `tests/native_addon_differential_test.cpp` failed one recorded artifact-verification case on the JSON error text.

The same four binaries and five cases failed in the Release shard 1 diagnostic. The two ARM64-only fixtures are in shards 3 and 4. The shared `src/rt/crypto.cpp` build blocker still requires the runtime owner’s fix for an ordinary CI build. [Release diagnostics](cppgw-ci-workflow-20261118.md#runtime-diagnostic)

## Handoff

The Release and ASan matrices and all 11 harness jobs are configured. The two local campaigns met their five-minute budgets. Matching ARM64 runs, the 40-minute campaigns and the full set of sanitizer shard results remain pending hosted validation.

The dry run and actual push both exited 128 with `could not read Username for 'https://github.com': terminal prompts disabled`. The published agent branch remains at `5705ff938c90303b291a80f817cb29a10ce97e6f`. The result commits are on the clean `agent-job` checkout for authenticated handoff.

```sh
GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false timeout 25s git push origin \
  HEAD:refs/heads/agent-jobs/cppgw-ci-sanitizers-20261118-44214118327-1
```
