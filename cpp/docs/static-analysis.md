# Static analysis

The pilot uses the qualified SDK's clang-tidy 14.0.6 and Clang Static Analyzer on the actual GCC 12.2 Pi C++20 compilation model. The existing core and C++ check families, `bugprone-dangling-handle` and `bugprone-use-after-move` remain selected. The two added checks are `clang-analyzer-unix.Malloc` and `clang-analyzer-unix.MismatchedDeallocator`. New or actionable findings are fatal; only the four exact pinned reports documented below can be classified, with their raw failed child exit preserved. `HeaderFilterRegex: ".*"` and `--system-headers` preserve emitted header diagnostics and caller paths; there is no vendor-directory exclusion or automatic fix.

This selection follows the requested GPT Pro recommendation from 2026-11-13, checked against the installed engine and representative defect controls. It adds modeled C allocation-family leak, use-after-free and deallocation mismatch coverage to the already enabled C++ new/delete checks. Cppcheck, GCC `-fanalyzer`, alpha checks, ownership annotation rollout and SDK upgrades are deferred.

## Commands

Use the same read-only qualified SDK as the ordinary C++ build; no install, download or separate dependency graph is needed.

```sh
python3 cpp/tools/build.py /absolute/qualified-sdk pi-release configure
python3 cpp/tools/build.py /absolute/qualified-sdk pi-release tidy
python3 cpp/tools/build.py /absolute/qualified-sdk pi-release complete
```

The `complete` Pi workflow enforces analysis after its build and six CTests. `tidy` delegates to `cpp/tools/analyze.py`; the always-run CMake target `cpp-static-analysis` invokes that same runner. An unchanged Ninja tree does not make the explicit analysis target up to date. The native sanitizer workflow remains separate.

For focused validation after configuration:

```sh
python3 cpp/tools/analyze.py /absolute/qualified-sdk pi-release controls
python3 cpp/tools/analyze.py /absolute/qualified-sdk pi-release check
python3 cpp/tests/analysis/test_analyze.py /absolute/qualified-sdk
```

The default runner mode includes controls and the complete source scan. `controls` runs the three static fixtures; `check` scans the thirteen production/test TUs after metadata and coverage validation. The wrapper tests additionally exercise missing tools/config/database, an unsupported checker, substituted or duplicate compilation entries, a malformed frontend input, invalid profiles, timeout and process failure. Deliberately incorrect C++ fixtures are never linked or executed.

## Coverage and receipts

The Pi preset enables an `OBJECT EXCLUDE_FROM_ALL` fixture target solely to generate accurate compilation commands. Its three TUs are separate from the twelve production TUs and `protocol_tests.cpp`: sixteen unique database entries, thirteen real roots and three controls. Source membership, compiler/sysroot, C++20 mode and lane paths must match; a file count alone is insufficient. Missing or foreign entries fail before analysis. Reconfigure after relocating a lane; do not rewrite an old database.

Every configured positive check pattern must match the installed full checker catalog, and all required IDs must appear in the effective selected listing. Clang 14 can return zero from a listing that silently omits an unknown name, so process success alone is insufficient.

Each TU gets a serial child, raw stdout/stderr, status, fixes YAML when emitted, and one fresh `--enable-check-profile` / `--store-check-profile` JSON naming that exact source. A profile proves the AST consumer was invoked; it does not prove parsing succeeded or every path was explored. Frontend errors, fallback/skip messages, crashes, timeouts and malformed or missing profiles remain failures. Pinned Clang 14 legitimately omits fixes YAML when no selected diagnostic exists; the wrapper records that absence and still requires the profile and frontend/status checks. It never invents a clean YAML report.

Receipts under `.data/cpp-evidence/analysis-*/` bind the Git revision/status, configuration, compilation database, tool, source hashes, expected and processed roots, commands, raw paths and elapsed time. The ordinary build driver retains its additional disk/timing receipt. Child RSS is labeled as the cumulative `RUSAGE_CHILDREN` maximum and is distinct from an independently measured cgroup peak.

## Controls and resource limits

The dirty fixture requires exactly seven diagnostic IDs at independently marked locations: a malloc leak, a new leak, use after delete, mismatched release in a synthetic system header with a first-party allocation path, an uninitialized ordinary-call argument, and a genuine same-invocation smart-pointer move reported by both `bugprone-use-after-move` and `clang-analyzer-cplusplus.Move`. Clean ownership and named-coroutine cancellation-pattern TUs each require a separate zero exit and fresh source-matched profile. The cancellation fixture is static input only; it is not a new executed cancellation proof. The six existing native ASan/UBSan/leak CTests retain the actual socket, cancellation, control, restart and resource evidence.

The final v4 profile requires eleven wrapper failure/mutation tests and three separately counted real Clang control TUs, including exactly seven expected dirty-control diagnostic records at the marked locations. The ownership and named-coroutine controls remain clean and genuine first-party same-invocation move misuse stays fatal. Earlier writer-only measurements are development evidence and are not relabelled as final canonical acceptance. Final measured qualification is recorded separately below.

Jobs remain serial. Each analyzer child uses the existing 1 GiB virtual-address limiter, with an approximately 300-second deadline for a complete control or source phase. The build driver preserves the 256 MiB incremental disk reserve, 768 MiB additional-output bound, 2,000,000,000-byte SDK/candidate ceiling and 2 GiB free-space floor. The external acceptance capture separately verifies physical cgroup limits, peak and OOM/swap counters. Do not apply the compiler address-space ceiling to an ASan runtime's shadow mappings.

## Limits and ownership

The first canonical scan processed all thirteen real TUs and failed on four `bugprone-use-after-move` reports in pinned Beast `http/impl/read.hpp` at 248:61, 251:13, 287:41 and 301:51. Its raw exit1 receipt remains failed. Source inspection and one reduced actual-Asio static probe established that `BOOST_ASIO_CORO_YIELD` transfers the handler and exits that invocation before later use; callback reentry binds the moved-to handler. Clang14's flow-sensitive, non-path-sensitive move checker does not correlate that macro resume path. The equivalent explicit-return operation was clean, while genuine same-invocation misuse remained fatal under both move checks.

`clang14-beast-read-baseline.json` classifies only those four exact reports from the Session TU: checker, primary SDK-relative path/offset/line/column/message, every move/loop note, warning level and cardinality must match. The loader, clang-tidy, Clang/LLVM frontend libraries, three related SDK headers and check-policy hashes must also match. Session/Server hashes are provenance, not eligibility constraints. The runner preserves all raw exit1/status/YAML/stdout/stderr/profile evidence and reports `classified:4`, `new:0` separately; it does not claim zero warnings. New or changed findings, first-party primaries, missing/stale entries, identity drift and unclassified failed exits remain fatal. Controls never receive this classification. Mutation tests and the actual genuine-move positive keep that boundary explicit. The original failure is `fae1741f-4e00-446f-a5eb-3540ddcf1016`; reduced proof is `b40d5db8-1a12-4d60-8d04-3d757ccbf877`. The corrected canonical scan and native acceptance are qualified only by the final exact-source measurements below; the original failed scan remains failed.

These heap models do not establish POSIX descriptor, pipe/socket or arbitrary OpenSSL-resource lifetime safety. They do not prove absence of shared-pointer cycles, retained reachable owners, post-cancellation use-after-free or every exceptional teardown. Clang 14 creates sink/aborted paths for coroutine nodes; a clean coroutine control and profile cannot overcome that engine limitation. Preserve explicit owner/canceller/join reasoning and dynamic evidence.

`BudgetResource` pairs aligned `operator new` and `operator delete`; JSON arenas outlive their values. State file descriptors use the existing RAII owner or explicit close-on-exception paths. Sessions retain complete write buffers across awaited writes and join downstream close observation before releasing storage. Transport awaits retain shared implementation owners, while deadline callbacks use weak ownership. Control frame buffers are initialized and owned across suspension, and completion retains Control. No runtime ownership code is changed by this tooling unit.

The earlier Boost/Control `core.CallAndMessage` and `core.NullDereference` reports disappeared after combined owner/initialization changes. Their cause was not isolated. That history is neither a proven SDK false positive nor permission to suppress SDK-primary findings; retain their complete first-party allocation/call paths for diagnosis.

## Measured qualification

Final qualification binds source `959bc0c643b0a1611313087665a964988358e37e`, tree `9d7eade3059069f242eec7bd9c7816995c01361b`. Runtime `cpp/src/**` and `cpp/include/**` are unchanged from the shipped pilot; this is analysis/configuration/control-fixture enforcement. Later documentation commits retain the analyzed-source identity and must prove unchanged runtime/tooling inputs.

The first canonical scan at `6a1c02674c45bc4b9fcbf6d17a72226fc074e9a4` correctly FAILED after all thirteen real TUs: capture233.814 seconds, cgroup peak568,934,400B. Its four Beast diagnostics and raw exit1 remain historical negative evidence under `fae1741f-4e00-446f-a5eb-3540ddcf1016`; this is not a successful v4 result. The reduced Asio probe `b40d5db8-1a12-4d60-8d04-3d757ccbf877` is a separate diagnostic experiment.

| Final v4 workload                                | Workload wall s | Child max RSS KiB | Cgroup peak B | Allocated-output delta B |
| ------------------------------------------------ | --------------- | ----------------- | ------------- | ------------------------ |
| Eleven wrapper tests + three real Clang controls | 20.199          | 326500            | 291459072     | 491520                   |
| Complete thirteen-real-TU scan                   | 219.434         | 599148            | 565817344     | 339968                   |
| Native sanitizer configure/build checkpoint      | 143.558         | 921620            | 1068220416    | 133648384                |
| Six native sanitizer CTests                      | 26.093          | 124876            | 216006656     | 487424                   |

All phases exited0 with actual serial1GiB cgroup/swap0/CPU1 limits; memory.max/OOM/oom_kill and swap counters remained0. The native build peak1,068,220,416B was near its1GiB cap, not a measurement of uncapped demand. Child `RUSAGE_CHILDREN` RSS and aggregate cgroup/file-cache charge are different and not additive. Allocated-output deltas are before/after snapshots, not transient disk peaks; final free2,673,020,928B remained above2GiB. Existing SDK/dependencies were reused, with no install or extra graph. ASan runtime shadow mappings were not given the analyzer's virtual-address ceiling.

The table uses the workload/controller resource clock consistently. Evidence capture includes setup/teardown: controls20.538s, scan219.879s, native checkpoint143.953s and CTest26.789s. The raw CTest report was23.61s and passed exactly6/6: protocol1.23s, socket6.84s, disconnect5.80s, control5.86s, recovery1.87s and resources2.01s. Those different clocks are retained, not mixed. Native flags include `-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer`, with leak detection, halt/abort and UBSan halt/stacktrace enabled through the existing candidate environment.

The accepted source scan processed all13 roots with fresh source-matched frontend profiles: twelve raw child exits0, Session raw child exit1 with exactly four retained diagnostics. Final policy status was `classified:4`, `new:0`, `missing:0`, `unclassified_exit:0`; this is **not zero warnings**. The three controls detected all seven expected dirty diagnostic records and kept both ownership/coroutine controls clean; genuine first-party same-invocation misuse remained fatal. Eleven wrapper tests include malformed report, baseline fingerprint/count/path/check/hash mutations and process/input failures. Emitted fixes YAML was fully parsed with the pinned Bun built-in YAML parser; absent YAML is legitimate only with otherwise qualified clean processing.

Every registered phase ran `/usr/bin/python3 /tmp/ai-cpp-static-analysis-20261113/final-verification-v4/run.py <phase>` from the exact canonical worktree; that capture driver invokes the committed command modes above and binds its argv/input hashes. Repository evidence namespace is `c18ba89e18e8c4c810e8dfab831978ea7ed5aa33d6f343bf3282dea4e12db13c`: controls `1619f071-0ef6-45a6-8161-277b224c8b32`, scan `e03a2c80-b652-47dc-809a-b2a117f8d0c6`, native checkpoint `9bb1ae87-45fa-4a6d-b109-94f261f12af4`, native CTest `e4f2264e-f9b7-4712-8d15-d86de714436d`. Cheap/configure receipts and all six actual cgroup teardown proofs are retained in the same packet; units are not-found/inactive/dead and cgroups absent.

Source/tooling input manifest SHA256 is `d7d114fdbbc89b601bd6901784bc0085250602ffbef13af2a3beba648e06a2c0`; strict scan qualification SHA256 `8594f0a45cbd00d551e1cb572a6dac4b3c22014fa98255276b7ee4aeaa646032`; phase-resource summary SHA256 `f3987ced7dc1996a8f92e68c84f1d73c253f5f1df1139931933e0636158c991d`; cleanup SHA256 `c513dc7c885726d96326bce355e79cd2f966e677575d96d961d4bbe896a49b14`. The checked-in baseline also binds exact tool/frontend/SDK-header/check-policy hashes; no broad suppression, fallback compilation or source/runtime rewrite is authorized. Static analysis and clean profiles do not prove shared-cycle, POSIX-FD or deferred coroutine/cancellation lifetime safety; retain dynamic sanitizer/socket evidence and explicit ownership reasoning.
