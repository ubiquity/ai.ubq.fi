# Codex module ASan/UBSan bring-up, 2026-11-17

## Work contract v1

Objective: build and run every top-level `tests/{codex_,banked_reset,cluster_}*_test.cpp`
first without sanitizers, then with Clang AddressSanitizer and UndefinedBehaviorSanitizer.
Preserve the TypeScript/Bun 1.4.0 oracle, assertion values, observable ordering, and
the documented coroutine rules. Fix root causes only in the assigned Codex/model
sources, their headers, shard tests/vector files, and `tests/helpers/codex_*`.
Other owned outputs are this report and, only for a documented model limitation,
`tests/lsan-codex.supp`.

Non-goals: other modules/runtime changes, host-specific build-tool repairs,
TypeScript or workflow changes, deployment, live credentials, production data,
and external provider requests. Outside-owned defects are reported with exact
commands and first diagnostics. No delegated agents are used.

Behavior matrix: all selected tests run unchanged in the plain profile and the
same tests run with ASan/UBSan and leak detection; supported providers remain
mocked/local fixtures. Every binary receives a fresh empty HOME, TMPDIR and cwd,
an allowlisted credential-free environment, and a 600-second process-group timeout.
All runtime translation units link into every binary; module Links closures select
the remaining objects. Each source compiles once per profile, with four workers.

Acceptance surface: warning-fatal C++20 compile/link, plain shard execution,
sanitized shard execution, regression cases for newly exposed paths, changed
document formatting, and `git diff --check`. No unrelated TypeScript aggregate
is selected. Before the final receipt all source writers stop; record commit/tree,
consumed-input hashes, commands, and actual counts. Material changes to this
contract invalidate affected evidence and increment its version.

Unknowns at discovery: distribution dependency availability, closure/link errors,
host-specific fixture assumptions, sanitizer failures, and publication permissions.
Stop when all selected binaries have both receipts and observed failures have
either owned fixes with reruns or exact outside-ownership reports. A blocked
publication remains explicitly incomplete for the enclosing job's publisher.

Long-operation budget: four compile workers, expected 10–30 minutes per cold
profile, incremental objects and full logs under `/tmp/cppgw-asan-codex-20261117`.
Observe live progress at most once a minute and diagnose failed commands before
retry. Runtime is expected to be seconds per binary; preserve all failure logs,
kill the process group after 600 seconds, and count every selected test.

## Discovery checkpoint

- Starting commit: `141cb4d4` on clean `agent-job`; this includes pre-existing
  port and production-build work that this task does not accept as its own.
- Host: Linux x86_64, four CPUs, 15 GiB RAM; Clang 21.1.8; Bun 1.4.0.
- Read the requested porting, build, testing, conventions, runtime async lifetime,
  runtime sanitizer, and libc++ portability guidance. Production-host resource
  restrictions are explicitly waived by this disposable-runner task.
- Dependency preparation, exact commands, pass/fail counts, and findings follow
  as execution produces evidence. No passing result is claimed at this checkpoint.
- Census: 71 selected top-level test binaries, 271 unique module/helper sources,
  44 runtime sources; 386 translation units per profile, plus 71 links. All Links
  closures resolve; none has a registry environment override or skip.
- Installed Boost.JSON/Container 1.90, Brotli 1.2.0, CMake 4.2.3 and Ninja 1.13.2.
  Existing development libraries include OpenSSL 3.5.5, SQLite 3.46.1, zlib 1.3.1,
  ICU 78.2 and Zstd 1.5.7. No dependency was downloaded inside the checkout.
- `sudo -n unshare --net` succeeds. Runs will enable only loopback, then drop
  to the original runner UID/GID 1001 with no supplementary groups. External
  routes and credential variables are absent.
- The initial publication preflight fails without GitHub write authentication;
  preserve this branch for the enclosing runner publisher. No credentials were
  printed, copied, refreshed, or persisted.

## Commands and current execution

```sh
sudo apt-get update -qq
sudo apt-get install -y -qq --no-install-recommends libboost-json-dev libboost-container-dev libbrotli-dev ninja-build cmake
python3 cpp/gateway/tests/helpers/codex_sanitizer_checks.py --profile plain --build-only --jobs 4
```

The shard runner generates its CMake project outside the checkout, uses per-source
object targets and the exact existing Links source order, and serializes links.
Both profiles use `-std=c++20 -Wall -Wextra -Wshadow -Wconversion -Werror
-DBOOST_JSON_NO_LIB -DBOOST_CONTAINER_NO_LIB -DBOOST_SYSTEM_NO_LIB
-DUOS_GATEWAY_NATIVE_LIBRARIES -pthread -O1 -g` and the two gateway include paths.
The sanitizer profile adds `-fsanitize=address,undefined -fno-omit-frame-pointer
-fno-sanitize-recover=undefined`. Links include Boost.JSON/Container, OpenSSL,
pthread, dl, SQLite, zlib, Brotli, Zstd and ICU. Expanded compiler argv are retained
in each profile's `compile_commands.json`; full configure/build output is in
`build.log`. Per-run JSON receipts fingerprint source/header/test/vector inputs,
record actual counts and source identity, and reject changes during execution.
Dirty discovery receipts deliberately have null commit/tree fields.

Publication preflight:

```sh
git -c credential.helper= -c core.askPass=/bin/false push --dry-run origin HEAD:refs/heads/test/cppgw-asan-codex-20261117
```

Exit 128: `fatal: could not read Username for 'https://github.com': No such device or address`.

## Findings

| Test      | Tool | Cause                     | Fix or disposition                   |
| --------- | ---- | ------------------------- | ------------------------------------ |
| Discovery | —    | Execution not yet started | Pending plain and sanitizer receipts |

## Final handoff

Work is in progress. Exact build/run commands, real counts, every source change,
unfinished work, outside-owned defects, and publication status will replace this
checkpoint before stopping.
