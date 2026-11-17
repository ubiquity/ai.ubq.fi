# Portable C++ gateway build: x86_64, 2026-11-17

Status: discovery in progress. This file is maintained during the run; no full-suite acceptance claimed yet.

## Work contract v1

Objective: add a portable, compiler-selectable CI driver, compile all gateway runtime/module sources once per configuration, and run every standalone test on this disposable x86_64 runner in GCC debug and release configurations. Provide sanitizer support, deterministic sharding, isolated test environments, a final result table, and a documented workflow proposal.

Non-goals: deployment/service activation, live databases, provider requests, credential changes, application redesign, changing Bun-recorded expected values, or fixing defects outside the owned paths. Existing production-host build wrappers and presets remain separate.

Invariants: C++20; `-Wall -Wextra -Wshadow -Wconversion -Werror`; Boost JSON/Container/System no-autolink definitions; every runtime object in each test; exact per-test Links closure; existing coroutine/lifetime rules and AArch64 behavior; no credentials inherited by tests; all build artifacts outside the checkout. No test skipped or weakened to obtain success.

Behavior matrix: GCC/Clang selectable with `CXX`; debug `-O0`, release `-O2`, ASan/UBSan `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer`; sorted tests split deterministically by `--shard i/n`; Ubuntu x86_64 is executable evidence here, Ubuntu arm64 and macOS arm64 remain proposed CI platforms.

Owned paths: `tools/ci-build.sh`, portability-only `tools/links.py`, new `cmake/ci/` files, link-list-only `tests/LINKS.txt`, `docs/CI.md`, this findings file, and required portability fixes under include/src/tests. Prohibited runtime files: `src/rt/json.cpp`, `src/rt/url.cpp`, `src/rt/regex*.cpp` and their headers. All other repository paths are read-only.

Acceptance surface: driver listing/filtering/sharding and argument validation; complete GCC debug/release compile and test commands; focused Clang/ASan checks if time permits; format checks for changed supported documents, Python/shell syntax checks and `git diff --check`. No unrelated TypeScript aggregate gates or extra hosted CI dispatch.

Unknowns to resolve through current build evidence: newer Boost API compatibility, missing includes/new warnings, x86_64 syscall and signed-char behavior, fixture paths, system SQLite behavior, compiler coroutine behavior, and direct-library versus dlopen selection. Stop when full runs and evidence are recorded, or a concrete unavailable prerequisite/ownership boundary prevents further progress. Report unexecuted gates explicitly.

## Runner and discovery

- Initial commit: `09d44bd5` (`agent-job` branch); clean checkout.
- Ubuntu 26.04.1, Linux x86_64, 4 CPUs, approximately 15 GiB RAM.
- GCC 15.2.0, Clang 21.1.8, Bun 1.4.0, CMake and Ninja already available.
- Installed distribution Boost 1.90 JSON/Container, Brotli development headers and mold; all other required development libraries were already present. SQLite is 3.46.1, ICU 78.2, OpenSSL 3.5.5.
- All build outputs will live under `/tmp`; no environment credentials are recorded.

## Commands and results

Initial full run in progress: `CXX=g++ UOS_CI_JOBS=4 cpp/gateway/tools/ci-build.sh --config debug > /tmp/cppgw-debug-driver.log 2>&1`. The branch contains 437 discoverable test targets (one more than the prompt), 325 canonical modules and 44 runtime sources. The initial discovery also compiled two dot-prefixed staging sources; the driver now excludes these from production inventory, matching the porting convention. A final clean-inventory receipt is required.

## Source changes and evidence

- `tools/ci-build.sh` and new `cmake/ci/{driver,isolate,test_driver}.py`: portable Ninja object build; exact Links closures; compiler/config identity; test result table and receipts; true per-test network/mount/PID namespaces, private temporary state and credential-free environment; six driver controls with real GCC and Clang compilation. All six controls pass on both compilers (8.107 s GCC, 10.517 s Clang at this checkpoint).
- `tests/rt/process_test.cpp`: keep ARM64 expectations and add Bun-observed x86_64 `process.arch == "x64"`. The isolated Bun probe also verified the unchanged pinned Bun revision.
- `tests/authority_settlement_test.cpp`: replace a conditional value expression with explicit saved/new-state branches. GCC 15 debug compilation diagnosed `dangling pointer to an unnamed temporary may be used [-Werror=dangling-pointer=]` at the temporary AuthorityState returned in the conditional. The factory owns its state and performs value assignment; explicit branches preserve both cases and compile without suppressions. Original diagnostic: `/tmp/uos-gateway-build/g++-70ce93550778/debug/obj/tests/authority_settlement_test.cpp.o.log` (also retained in the compile log).
- `tests/codex_account_routing_part3_test.cpp`: give the assertion helper's diagnostic string a named lifetime. GCC 15 reported `possibly dangling reference to a temporary [-Werror=dangling-reference]` because `required` returns a reference while accepting a temporary string label. The returned reference still targets the same stored quota block; all assertions are unchanged. Original diagnostic remains in the compile log.
- `docs/CI.md`: prerequisites, exact interfaces/commands, isolation and output contracts, measurement qualifications and a manual-only proposed platform/config/shard workflow. No `.github/` files changed.

## Unfinished work and out-of-scope defects

- The TypeScript oracle explicitly rejects x86_64 confinement: isolated Bun 1.4.0 (`--no-env-file --no-install`) prints `process.arch = x64` and `getLinuxPermissionsSupport()` throws `Linux confinement requires Linux ARM64`. `src/runtime/permissions-linux.ts:55` is outside the owned paths. C++ reproduces that platform gate, while its existing five confinement test cases assume ARM64. Enabling x86_64 confinement would change oracle behavior; no such change is made without a source contract change. Full-run failures will be preserved.
- `tests/rt/sqlite_test.cpp` has native-profile expectations for host SQLite 3.54.0; this distribution supplies 3.46.1. Actual failures remain to be measured; expected values will not be weakened.
- GitHub CLI has no authenticated GitHub session on this runner. Delivery will be attempted through the configured Git remote; credentials will not be read or repurposed.
- No production or Mac acceptance is claimed.
