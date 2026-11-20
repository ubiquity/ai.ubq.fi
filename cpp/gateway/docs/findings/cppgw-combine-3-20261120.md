# Native gateway follow-up combination 3

## Checklist

- [x] Fetch and inspect the three reviewed branches from `claude/cpp-gateway-port` (`0c545781`).
- [x] Merge `agent-jobs/cppgw-macos-final-20261119-44637373710-1` (Apple Silicon confirmation).
- [x] Merge `agent-jobs/cppgw-stream-retry-20261119-44629153076-1` (pre-output stream retry).
- [x] Merge `agent-jobs/cppgw-build-cache-20261119-44628404886-1` (compiler cache).
- [x] Reconcile release commit binding with the compiler launcher in packaging and tooling.
- [x] Restrict `UOS_PACKAGE_GIT_COMMIT` to translation units that read it.
- [ ] Show two package-style builds at different commits share non-identity cache hits.
- [x] Run `python3 cpp/gateway/tools/links.py --check`.
- [x] Run `actionlint` on both workflows.
- [x] Build release with the launcher and run release-identity, health, startup, Codex routing/stream-retry and Gemini tests.
- [ ] Portable package build + verifier (Docker) with health bound to the built commit.
- [ ] Record evidence and leave the checkout clean.

## State

Starting commit: `0c545781` (`claude/cpp-gateway-port`, delivered native gateway).

## Log


- `65b4a0a4` started this checklist.
- Mac final tip `316de915` merged cleanly; the merge commit preserves its ancestry.
- Stream retry tip `824b181b` merged cleanly; the merge commit preserves its ancestry.

- Build-cache tip `fd5ffece` merged with conflicts in `packaging/macos/build.py` and `packaging/portable/Dockerfile`; both sides were kept: packaged builds bind `SOURCE_COMMIT`/`--release-commit` and compile through the configured launcher.
- `UOS_PACKAGE_GIT_COMMIT` now reaches only `src/release.cpp`, `tests/health_test.cpp`, `tests/release_identity_test.cpp` in CMake and `tools/macos-build.sh`; `packaging/macos/gateway-version.cpp` keeps its local definition. `packaging/macos/build.py` no longer puts the definition on the startup object or the link line.


## Verification so far

- `python3 -B cpp/gateway/tools/links.py --check` resolved every registered test.
- `actionlint` 1.7.12 with `cpp/gateway/tools/ci-actionlint.yaml` passed on `.github/workflows/cpp-gateway-ci.yml` and `.github/workflows/cpp-gateway-release.yml`.
- `cmake` configure with tests enabled and `UOS_GATEWAY_GIT_COMMIT` set emits the definition on exactly three commands: `src/release.cpp`, `tests/health_test.cpp` and `tests/release_identity_test.cpp`. Reconfiguring with a different commit changes only those three commands out of 873.
- Release builds through `cpp/gateway/tools/ci-build.sh` used `/usr/bin/ccache` as the launcher and loaded pinned SQLite 3.54.0 inside the fixture boundary. Results:

| Test | Cases |
| --- | ---: |
| `tests/release_identity_test.cpp` | 1/1 |
| `tests/health_test.cpp` (plus the two other `health` matches) | 35/35 |
| `tests/runtime_native_startup_test.cpp` | 1/1 |
| `tests/kv_budget_part1_test.cpp` | 32/32 |
| `tests/codex_serial_routing_http_test.cpp` | 2/2 |
| `tests/serve_delivery_http_test.cpp` | 10/10 |
| `tests/gemini_permissions_test.cpp` | 3/3 |
| `tests/gemini_subscription_test.cpp` | 27/27 |

The driver ran each test in a fresh network/mount/pid namespace, with a fresh home and temp directory and the credential-free environment.
