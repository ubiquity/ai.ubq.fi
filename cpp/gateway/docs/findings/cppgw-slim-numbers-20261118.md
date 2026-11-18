# Shrink C++ gateway numeric helpers

## Contract v1

Retire `parseFloat`, `numberToString(double, int)` and `toFixed` after migrating their remaining callers to bounded types. Preserve Codex session UUID bytes, decimal JSON and persisted key bytes, numeric coercion and validation, and BigInt. The provider matrix has one touched path. Codex dispatch derives the same session identity from the same SHA-256 material. Provider selection, authentication, accounting, TypeScript, dependencies and deployment are outside this change.

Production edits are confined to the three numeric API regions and their private dead code in `src/rt/number.cpp` and `include/uos/rt/number.hpp`, and to the UUID nibble operation in `src/codex/dispatch.cpp`. The directly affected numeric fixtures, the deviation notes and this report are the remaining write paths.

The starting commit is `9b4cc9aa25ddaa8a229fdc480d949931fbcaceca`, tree `2fdf141d7ba938ec52a1cdf8e2d52c87a1812eae`. Its registry contains 442 binaries. The BigInt handoff records 441 passing binaries and 5,027 passing cases out of 5,028 in both profiles after its ICU and sanitizer controls. The inherited failure is the Number fixture's BigInt expectations for incomplete signed and prefixed tokens. Starting production line counts are 758 in `src/rt/number.cpp`, 246 in `include/uos/rt/number.hpp` and 883 in `src/codex/dispatch.cpp`. [BigInt handoff](cppgw-std-bigint-20261117.md#terminal-verification-and-handoff)

The baseline command is `bash cpp/gateway/tools/ci-build.sh --config release`. The final gates use the same driver in release and `asan` modes. The runtime object in every link list makes all 442 registered tests in scope. The Number, JSON, KV, Codex dispatch and API key policy receipts are tracked individually. Tests run on Linux ARM64 with the driver's credential-free namespace setup. Changed documents use pinned Prettier and the patch uses `git diff --check`.

Runner-specific unknowns are the baseline failure inventory and cold build times. Budget 45 minutes for the baseline and 75 minutes for the sanitizer compile and sweep. Track compiled objects and completed binaries in the driver's logs, use incremental objects for retries and retain prior failure evidence. Stop after both final profiles match the baseline and the committed scope, counts, timings and any handoff blockers are recorded.
