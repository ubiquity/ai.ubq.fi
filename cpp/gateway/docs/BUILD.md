# C++ gateway build

Contract v7 targets this Linux AArch64 VPS first, macOS second, and a Raspberry Pi only after ABI and memory qualification. Native primary acceptance is pending. The retained GCC 12 SDK build is comparison evidence. No build result authorizes deployment, service activation, credential access, production database use, or provider spend.

## Profiles and dependencies

| Profile            | Compiler and C++ library                                  | Purpose                                                                                      | Current receipt                                |
| ------------------ | --------------------------------------------------------- | -------------------------------------------------------------------------------------------- | ---------------------------------------------- |
| `release`          | Host GCC 16.1.1, libstdc++, Boost 1.92                    | Primary Linux release, `-O2 -g0`, warnings fatal                                             | Pending                                        |
| `clang-release`    | LLVM 23.1.5, libstdc++, Boost 1.92                        | Independent compiler and full-suite gate                                                     | Pending                                        |
| `asan-ubsan-clang` | Clang 23, AddressSanitizer and UndefinedBehaviorSanitizer | Whole suite, serial runtime, measured physical-memory guard                                  | Pending                                        |
| `asan-ubsan-gcc`   | GCC 16, AddressSanitizer and UndefinedBehaviorSanitizer   | Explicit optional report-quality comparison                                                  | Not selected                                   |
| `tsan-threaded`    | Clang 23, ThreadSanitizer                                 | SQLite worker and Asio resolver only                                                         | Pending                                        |
| `fuzz-clang`       | Clang 23 and libFuzzer                                    | Bounded parser targets and seed corpus                                                       | Pending                                        |
| `clang-libcxx`     | Clang 23 and installed libc++                             | Standard-library compatibility gate; requires ABI-matching C++ dependencies for full linkage | Pending                                        |
| `sdk-release`      | Retained GCC 12.2, libstdc++, Boost 1.81                  | Comparison; GCC 12 coroutine workarounds remain mandatory                                    | Foundation built; full optimized suite pending |

The owner-installed `/home/codex/toolchains/uos-cpp-2026.11` and retained SDK are read/execute only. Workers install and download nothing. Native profiles select host OpenSSL 3.6.5, SQLite 3.54.0, zlib 1.3.2, ICU 78.3, Brotli 1.2.0 and Zstd 1.5.7 headers/libraries; the library transition and exact runtime closure are recorded in [RUNTIME.md](RUNTIME.md). Each compiler/profile has a separate output tree, tool view, cache namespace, ownership manifest and compilation database. Never mix compiler objects or libstdc++ and libc++ dependency archives.

## Bounded commands

Run from the worktree root through the driver; do not invoke a compiler, Ninja or CMake directly on this production host. The approved v7 interface is below; native profile execution and final command receipts remain pending while its build owner finishes integration.

```sh
python3 cpp/gateway/tools/build.py native release configure --tests --supervisor-log /tmp/claude-1002/codex/resources.log
python3 cpp/gateway/tools/build.py native release build --tests --supervisor-log /tmp/claude-1002/codex/resources.log
python3 cpp/gateway/tools/build.py native release test --tests --jobs 1 --supervisor-log /tmp/claude-1002/codex/resources.log
python3 cpp/gateway/tools/build.py native release package --jobs 1 --supervisor-log /tmp/claude-1002/codex/resources.log
```

Use an explicit `clang-release`, sanitizer or fuzz profile for its own gate. The `native` spelling does not select the retained SDK. The SDK comparison uses its exact existing SDK path plus `sdk-release`; the old SDK `release` alias is transitional and must never reinterpret an in-flight build. Existing `tools/unit.sh`, `tools/links.py` and `tools/run-tests.sh` remain the per-test comparison flow documented in [TESTING.md](TESTING.md).

The generated inventory compiles all ported modules and runtime sources once into per-source objects. `uos_gateway_modules` builds the static library; `uos_gateway`, `uos_kv_convert` and `uos_kv_proof` link the executable entrypoints. The gateway uses whole-archive linkage to retain module registrars. Tests select their actual `Links` closures, including local stand-ins, instead of importing the whole application archive. The runtime-owned entrypoint source now requires `--runtime-manifest <absolute-path>`, validates package/state/library/artifact identities, refuses ports 7999/8000 and has no development fallback. Primary executable/package and same-binary execution receipts remain pending; see [RUNTIME.md](RUNTIME.md) for the closed launch schema. Do not launch an unverified candidate outside the isolated harness.

## Resource and evidence policy

Only one full build/profile owns the full-release lock at a time. All compiler paths share two global slots with the existing unit wrappers. Compilers and source-bearing compiler/link probes have a 2 GiB address-space cap. Object-only links acquire both slots, retain a 3 GiB cap and require at least 6 GiB available memory after locking. Build admission also requires free disk above 8 GiB, healthy supervisor status, and the 2 GiB host reserve. There is no automatic memory-limit escalation, compiler substitution or unrelated artifact cleanup.

ASan runtime needs a large shadow address range and must not inherit the compiler or ordinary fixture virtual-memory cap. Its whole-suite runtime is serial, with measured real RSS, a host reserve and exact owned-process settlement. Every skipped, failed or interrupted case must be explicit. TSan covers only actual threaded code. Metadata queries, static analysis and fuzzing retain their own bounded commands and receipts.

Receipts bind command, cwd, compiler/profile/tool identities, consumed source and header hashes, exit status, counts, wall time, RSS and artifacts. A later consumed-source edit invalidates its affected receipt. Task-local evidence is the approved output-scope exception to the normal home-state evidence helper. Successful compilation, a fixture response and production acceptance are distinct facts.

## Native compiler compatibility

The GCC 16 warning-fatal configuration exposed three optimizer-sensitive source shapes: variant copy assignment during KV-prefix range insertion, hidden tuple objects created by direct structured bindings from `co_await`, and a copied optional auth variant inside an aggregate expression. The shared key constructor now reserves and copy-constructs prefix components, including rvalue prefixes; seventeen observed awaited bindings and one synchronous pair binding inside a coroutine use named owners before same-constness references; audit construction branches around the helper call at its original aggregate position. Class layout, key contents, await expressions/turns and actor evaluation order are preserved. The original five-line prefix probe fails and the changed probe passes under identical strict GCC 16 flags; the changed application translation units pass targeted compilation, including real release CMake objects for embeddings and all four kernel quota files. Unchanged runtime fixtures and the complete primary suite remain separate required evidence. No warning or assertion is disabled.

## Measured SDK comparison

The accepted foundation contains 325 module implementations and 44 runtime translation units, with 369 archive members. All optimized production compilations used `-O2 -g0 -Wall -Wextra -Wshadow -Wconversion -Werror`. The full optimized suite was not run in that foundation.

| Measurement                                | SDK GCC 12 comparison result     | Qualification                                          |
| ------------------------------------------ | -------------------------------- | ------------------------------------------------------ |
| Largest successful production compiler RSS | 1,227,092 KiB                    | Compiler memory, not gateway runtime RSS               |
| Final unique production compile wall time  | 4,262.539 s                      | Sum across two slots, not elapsed cold-build time      |
| Resumed final gateway phase                | 1,351.582 s                      | Reused 166 valid objects                               |
| Recorded source-build phases               | 2,211.323 s                      | Includes earlier failures; not a clean cold build      |
| Optimized final gateway link               | 2.104 s; 331,944 KiB peak RSS    | Actual optimized objects                               |
| Historical large `-O0` cache link          | 15.010 s; 2,605,840 KiB peak RSS | Different object set; never executed                   |
| CLI phase owned allocation                 | 544,714,752 bytes                | Build-directory allocation, not installed package size |

SDK gateway SHA-256: `674e95f30ac8b8b852a6e3cafe1e011b29908dfdac0fc4875443b13dbbf1545c`. Foundation receipt: `.data/cpp-gateway-evidence/foundation-final/terminal.json`, SHA-256 `7a9e1478265d29f60b839fed5e89e96c8879b97e2a8e637b8c2b51090614258a`. The parent independently verified four artifact hashes, 13 owned files and all 369 source/object pairs. These receipts do not satisfy the new native compiler gates.

## Native build-speed and hardening gates

| Gate      | Required comparison                                                                                           | Result      |
| --------- | ------------------------------------------------------------------------------------------------------------- | ----------- |
| ccache    | Disabled/cold/warm on identical heavy translation units; compiler-content and profile namespace bound         | Pending     |
| mold      | Identical objects versus BFD; link wall time and peak RSS                                                     | Pending     |
| PCH       | Identical two-slot workload with and without heavy Boost PCH; include build/storage cost                      | Pending     |
| LTO       | Separate bounded candidate; keep release profile unchanged until measured                                     | Not adopted |
| Hardening | Fortify, stack protector, selected C++ assertions and PIE/RELRO with exact flags, ELF proof and workload cost | Pending     |
| Packaging | Reproducible target, hashes, required assets/runtime closure, missing-artifact refusal and safe startup       | Pending     |

Keep the SDK physically intact after primary qualification; retire its acceptance role explicitly. [PERFORMANCE.md](PERFORMANCE.md) owns runtime measurements, [MACOS.md](MACOS.md) the unexecuted Mac/Pi portability plan, and [OPEN-STAGES.md](OPEN-STAGES.md) the remaining acceptance boundaries.
