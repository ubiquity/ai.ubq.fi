# Portable C++ gateway build: x86_64, 2026-11-17

Status: discovery in progress. This file is maintained during the run; no full-suite acceptance claimed yet.

## Work contract v4

Objective: add a portable, compiler-selectable CI driver, compile all gateway runtime/module sources once per configuration, and run every standalone test on this disposable x86_64 runner in GCC debug and release configurations. Provide sanitizer support, deterministic sharding, isolated test environments, a final result table, and a documented workflow proposal.

Non-goals: deployment/service activation, live databases, provider requests, credential changes, application redesign, changing Bun-recorded expected values, or fixing defects outside the owned paths. Existing production-host build wrappers and presets remain separate.

Invariants: C++20; `-Wall -Wextra -Wshadow -Wconversion -Werror`; Boost JSON/Container/System no-autolink definitions; every runtime object in each test; exact per-test Links closure; existing coroutine/lifetime rules and AArch64 behavior; no credentials inherited by tests; all build artifacts outside the checkout. No test skipped or weakened to obtain success.

Contract v4 inventory amendment: exclude dot-prefixed files and directories from both production and runtime inventories, preserving the port's unpublished staging convention. A new isolated inventory control covers this. The current checkout still selects the same 369 production sources, 44 runtime sources and 437 tests. The in-flight v3 debug receipt is historical after this driver edit; fresh final-tree runs are required.

Contract v3 fixture amendment: every test uses a private, bounded 512 MiB tmpfs. The first run on the runner's ext4 scratch clamped valid large timestamps; isolated Bun probes reproduce the original recorded raw seconds exactly on tmpfs. This preserves all timestamp expectations and makes the fixture filesystem explicit. All discovery receipts remain historical; rerun the frozen source and prepared dependencies for acceptance.

Contract v2 dependency amendment: use published SQLite 3.54.0 for the current native-library expectations. Earlier discovery builds using distribution SQLite 3.46.1 and intermediate source edits are not final acceptance receipts. Rebuild the SQLite-consuming object and rerun after library preparation. No expected values change.

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

Discovery (not final acceptance): the first resumed GCC debug sweep returned nonzero with 426 PASS, 7 FAIL, 4 BUILD_FAIL, 0 TIMEOUT; 4,933 reported cases, 4,902 passed, 31 failed. The object phase was 1,169.647 s using previously populated objects; link/test wall time 284.626 s; total 1,454.936 s. The earlier interrupted cache-population phase is separate, so this is not a cold-build measurement. Source edits during discovery and the later SQLite preparation require a fresh final receipt. Original receipt and failure evidence are preserved under `/tmp/cppgw-discovery-failures`.

Focused post-fix previews: process 10/10; KV-budget part 1 24/24; filesystem 10/10; regex 16/16; banked-reset capacity recovery 12/12; named provider limits 3/3. Evidence: `/tmp/cppgw-process-preview.json`, `/tmp/cppgw-budget-fixed-preview.json`, `/tmp/cppgw-focused-portability.json`. Preview links used the existing debug objects and are diagnostic controls, not the final full-source receipt.

Initial full run in progress: `CXX=g++ UOS_CI_JOBS=4 cpp/gateway/tools/ci-build.sh --config debug > /tmp/cppgw-debug-driver.log 2>&1`. The branch contains 437 discoverable test targets (one more than the prompt), 325 canonical modules and 44 runtime sources. The initial discovery also compiled two dot-prefixed staging sources; the driver now excludes these from production inventory, matching the porting convention. A final clean-inventory receipt is required.

## Source changes and evidence

- `tools/ci-build.sh` and new `cmake/ci/{driver,isolate,test_driver}.py`: portable Ninja object build; exact Links closures; compiler/config identity; test result table and receipts; true per-test network/mount/PID namespaces, private temporary state and credential-free environment; six driver controls with real GCC and Clang compilation. All six controls pass on both compilers (8.107 s GCC, 10.517 s Clang at this checkpoint).
- `tests/rt/process_test.cpp`: keep ARM64 expectations and add Bun-observed x86_64 `process.arch == "x64"`. The isolated Bun probe also verified the unchanged pinned Bun revision.
- `tests/authority_settlement_test.cpp`: replace a conditional value expression with explicit saved/new-state branches. GCC 15 debug compilation diagnosed `dangling pointer to an unnamed temporary may be used [-Werror=dangling-pointer=]` at the temporary AuthorityState returned in the conditional. The factory owns its state and performs value assignment; explicit branches preserve both cases and compile without suppressions. Original diagnostic is retained in `/tmp/cppgw-debug-final.log` and the archived build compile log; a focused recompile passed after the change.
- `tests/helpers/codex_account_routing_harness.hpp`: take diagnostic labels by `std::string_view`, materializing a string only on the error path. Two independent GCC 15 failures (`codex_account_routing_part3_test` and `codex_banked_reset_capacity_recovery_test`) diagnosed `possibly dangling reference to a temporary [-Werror=dangling-reference]` from the helper's temporary string label, although the return refers to its first argument. The helper signature now expresses that separation without a warning suppression; return identity, error text and all assertions stay unchanged. The earlier per-call named-label workaround was removed in favor of this shared fix.
- `tests/{jev_compaction,jev_codex}_smoke_test.cpp`: apply the existing GCC-only `-Wsubobject-linkage` diagnostic pragmas only under GCC. Clang 21's strict syntax check failed with `unknown warning group '-Wsubobject-linkage', ignored [-Werror,-Wunknown-warning-option]`. The real implementation includes and every assertion remain unchanged; GCC keeps its preexisting approved test-context exception.
- `tests/provider_capacity_named_limits_test.cpp`: the same GCC 15 temporary-label diagnostic occurred in its private `member` helper. Accept the key as `std::string_view` and preserve the identical missing-member message and JSON reference.
- `src/kernel/quota_reservations.cpp`: name the tuple returned by `co_await reads.wait()` before binding its entries. GCC 15 x86_64 aborted with `free(): invalid pointer`; a namespace-isolated GDB backtrace reaches the `KvEntryMaybe` tuple destructor in `readKernelQuotaPolicyState`. The separate binding keeps the same concurrent Promise reads, suspension point and moves into the result. The same 24 KV-budget cases then pass. Original trace: `/tmp/cppgw-budget-gdb.log`; fixed preview: `/tmp/cppgw-budget-fixed-preview.json`.
- `tests/rt/fs_test.cpp`: preserve ARM64 `O_NOFOLLOW == 32768` and use the isolated Bun-observed x86_64 value `131072`. Runtime flags already use the native OS constants. The timestamp expectations are untouched; private tmpfs supplies their required Linux timestamp range. Bun on tmpfs stored raw seconds `8640000000000/-8640000000000`, `9200000000000000000/1`, and `9223372036854775807/9223372036854775807` exactly (`/tmp/cppgw-bun-tmpfs.log`). The filesystem suite then passes all 10 cases.
- `tests/rt/regex_test.cpp`: sequence `matchAll` before `word.test` in the reentrant replacement callback. C++ `operator+` operand evaluation order produced `1t 2f` on this compiler; isolated Bun confirms the existing `2t 1f` expectation and `lastIndex == 0`. Binding the count preserves JavaScript evaluation order and all 16 cases pass. No regex implementation/header owned by the other agent was changed.
- `docs/CI.md`: prerequisites, exact interfaces/commands, isolation and output contracts, measurement qualifications and a manual-only proposed platform/config/shard workflow. No `.github/` files changed.

## Unfinished work and out-of-scope defects

- The TypeScript oracle explicitly rejects x86_64 confinement: isolated Bun 1.4.0 (`--no-env-file --no-install`) prints `process.arch = x64` and `getLinuxPermissionsSupport()` throws `Linux confinement requires Linux ARM64`. `src/runtime/permissions-linux.ts:55` is outside the owned paths. C++ reproduces that platform gate, while its existing five confinement test cases assume ARM64. Enabling x86_64 confinement would change oracle behavior; no such change is made without a source contract change. Full-run failures will be preserved.
- The distribution SQLite 3.46.1 does not meet the preexisting native-profile 3.54.0 expectations in `tests/rt/sqlite_test.cpp`. The published `https://www.sqlite.org/2026/sqlite-autoconf-3540000.tar.gz` was prepared outside the checkout, SHA-256 `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed`. Configure: `./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'`; build: `make -j1`; installation: `sudo make install && sudo ldconfig`. The `SQLITE_DQS=3` library setting preserves the documented double-quoted-string contract. No expected value is edited.
- The TypeScript `src/runtime/native-kv-bootstrap.ts:13` also rejects x86_64: an isolated Bun probe of `nativeKvDevelopmentPaths` prints `Native KV development platform is unsupported`. This is another source-level platform contract outside the owned paths, not permission to bypass the native backend checks.
- Existing non-portability build defect in `cpp/gateway/tests/serve_test.cpp:48`: `auto volatile entry=&serve::importMetaMain` is ambiguous because `include/uos/serve.hpp` now declares both the no-argument and `ServeStartupOptions` overloads. Failing command is the generated GCC debug compile in `/tmp/cppgw-debug-final.log`; first diagnostic: `error: unable to deduce ‘volatile auto’ from ‘& uos::serve::importMetaMain’`, followed by `couldn’t deduce template parameter ‘auto’`. This is a source API/test mismatch independent of architecture or compiler version, outside the portability-only source-edit allowance. It is reported without changing the assertion or invoking either launcher. Next owner action: explicitly type the intended no-argument function pointer.
- GitHub CLI has no authenticated GitHub session on this runner. Delivery will be attempted through the configured Git remote; credentials will not be read or repurposed.
- Debug supervisor-secret failures are the original 2-second wall-clock assertions at test lines 167 and 239; 39/41 cases pass. The assertions are unchanged. Await release evidence before deciding whether optimized performance still fails.
- No production or Mac acceptance is claimed.
