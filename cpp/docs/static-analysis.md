# Static analysis

The pilot uses the qualified SDK's clang-tidy 14.0.6 and Clang Static Analyzer on the actual GCC 12.2 Pi C++20 compilation model. The existing core and C++ check families, `bugprone-dangling-handle` and `bugprone-use-after-move` remain selected. The two added checks are `clang-analyzer-unix.Malloc` and `clang-analyzer-unix.MismatchedDeallocator`. Findings are fatal. `HeaderFilterRegex: ".*"` and `--system-headers` preserve emitted header diagnostics and caller paths; there is no vendor-directory exclusion or automatic fix.

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

The dirty fixture requires exactly five diagnostic IDs at independently marked locations: a malloc leak, a new leak, use after delete, mismatched release in a synthetic system header with a first-party allocation path, and an uninitialized ordinary-call argument. Clean ownership and named-coroutine cancellation-pattern TUs each require a separate zero exit and fresh source-matched profile. The cancellation fixture is static input only; it is not a new executed cancellation proof. The six existing native ASan/UBSan/leak CTests retain the actual socket, cancellation, control, restart and resource evidence.

Writer-lane qualification detected all five expected defects and passed both clean controls plus the wrapper failure tests in 19.514 seconds; the verified 1 GiB task cgroup peaked at 283,787,264 bytes with zero OOM/swap events. This is development evidence on the edited tree. Final canonical source-scan and native sanitizer measurements are pending independent verification; no final source acceptance follows from these fixture results alone.

Jobs remain serial. Each analyzer child uses the existing 1 GiB virtual-address limiter, with an approximately 300-second deadline for a complete control or source phase. The build driver preserves the 256 MiB incremental disk reserve, 768 MiB additional-output bound, 2,000,000,000-byte SDK/candidate ceiling and 2 GiB free-space floor. The external acceptance capture separately verifies physical cgroup limits, peak and OOM/swap counters. Do not apply the compiler address-space ceiling to an ASan runtime's shadow mappings.

## Limits and ownership

These heap models do not establish POSIX descriptor, pipe/socket or arbitrary OpenSSL-resource lifetime safety. They do not prove absence of shared-pointer cycles, retained reachable owners, post-cancellation use-after-free or every exceptional teardown. Clang 14 creates sink/aborted paths for coroutine nodes; a clean coroutine control and profile cannot overcome that engine limitation. Preserve explicit owner/canceller/join reasoning and dynamic evidence.

`BudgetResource` pairs aligned `operator new` and `operator delete`; JSON arenas outlive their values. State file descriptors use the existing RAII owner or explicit close-on-exception paths. Sessions retain complete write buffers across awaited writes and join downstream close observation before releasing storage. Transport awaits retain shared implementation owners, while deadline callbacks use weak ownership. Control frame buffers are initialized and owned across suspension, and completion retains Control. No runtime ownership code is changed by this tooling unit.

The earlier Boost/Control `core.CallAndMessage` and `core.NullDereference` reports disappeared after combined owner/initialization changes. Their cause was not isolated. That history is neither a proven SDK false positive nor permission to suppress SDK-primary findings; retain their complete first-party allocation/call paths for diagnosis.
