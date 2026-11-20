# Native gateway follow-up combination 3

## Checklist

- [x] Fetch and inspect the three reviewed branches from `claude/cpp-gateway-port` (`0c545781`).
- [x] Merge `agent-jobs/cppgw-macos-final-20261119-44637373710-1` (Apple Silicon confirmation).
- [x] Merge `agent-jobs/cppgw-stream-retry-20261119-44629153076-1` (pre-output stream retry).
- [x] Merge `agent-jobs/cppgw-build-cache-20261119-44628404886-1` (compiler cache).
- [x] Reconcile release commit binding with the compiler launcher in packaging and tooling.
- [x] Restrict `UOS_PACKAGE_GIT_COMMIT` to translation units that read it.
- [x] Show two package-style builds at different commits share non-identity cache hits.
- [x] Run `python3 cpp/gateway/tools/links.py --check`.
- [x] Run `actionlint` on both workflows.
- [x] Build release with the launcher and run release-identity, health, startup, Codex routing/stream-retry and Gemini tests.
- [x] Portable package build + verifier (Docker) with health bound to the built commit.
- [x] Record evidence and leave the checkout clean.

## State

Starting commit: `0c545781` (`claude/cpp-gateway-port`, delivered native gateway).

## Merge record

Each branch was fetched from `origin` and merged with `git merge --no-ff`. The tips remain ancestors of this branch.

| Source branch | Tip | Merge commit | Result |
| --- | --- | --- | --- |
| `agent-jobs/cppgw-macos-final-20261119-44637373710-1` | `316de915900be274f1586a9f91e48fd1ad2e8654` | `73317117` | Clean |
| `agent-jobs/cppgw-stream-retry-20261119-44629153076-1` | `824b181bd5d10844f85646ad70f5914fbc8ccbb1` | `ad90b303` | Clean |
| `agent-jobs/cppgw-build-cache-20261119-44628404886-1` | `fd5ffecef8e4d29415de149f8e42f613d7065360` | `e7090c03` | Conflicts in two packaging files, resolved below |

## Reconciliation

- `packaging/macos/build.py` keeps the delivered `--release-commit` binding and the cache branch's launcher/environment plus the `-fdebug-prefix-map`. It still compiles per-executable entry objects with the launcher. The commit definition now goes only to `packaging/macos/gateway-version.cpp`; it is no longer put on the startup object or passed on the link line.
- `packaging/portable/Dockerfile` keeps `SOURCE_COMMIT` validation and `-DUOS_GATEWAY_GIT_COMMIT`, and keeps the cache branch's dependency stage, launcher resolution and ccache cache mount. The duplicate ccache install in the `cpp` stage was dropped because the parent toolchain stage already installs it.
- `packaging/portable/build.sh` keeps the delivered commit/tree/epoch validation and the cache branch's launcher build argument. `tools/macos-build.sh` keeps `--release-commit` and adds the launcher; the commit definition is attached per identity source instead of to the global flag list.
- `CMakeLists.txt` no longer propagates `UOS_PACKAGE_GIT_COMMIT` through the interface settings. `uos_object_for_source` applies it only to `src/release.cpp`, `tests/health_test.cpp` and `tests/release_identity_test.cpp`.
- The release workflow still has no cache restore. It sets `PORTABLE_REBUILD=1` and an explicit empty launcher for the published build.

## Compiler-cache evidence

Two package-style CMake builds used the same fixed source tree and the portable Dockerfile's `cpp` stage, changing only the bound commit: `623b55a1704195b89316de54673f7ae5e781cb15` first and `e7090c03f13b375dd4d7df8db9d0103969b0481a` second. BuildKit's persistent `/var/cache/ccache` mount supplied the shared cache.

| Build | Cacheable calls | Hits | Direct hits | Preprocessed hits | Misses |
| --- | ---: | ---: | ---: | ---: | ---: |
| `623b55a1` | 984 | 6 | 0 | 6 | 978 |
| `e7090c03` | 1365 | 386 | 380 | 6 | 979 |
| Delta | 381 | 380 | 380 | 0 | 1 |

The one new miss is `src/release.cpp`; the other 380 cacheable compilations hit. Both logs end with the `ccache --show-stats` output. Logs: `/tmp/portable-build.log` and `/tmp/portable-cache-second.log`.

## Portable package

`packaging/portable/build.sh` built `/tmp/portable-out/uos-gateway-linux-arm64.tar.gz` from `623b55a1704195b89316de54673f7ae5e781cb15`; SHA-256 `65daf7f29ff5c2f1a240bf52cb540b2bc3b182061e7ebb8ba607d60599f17bcb`. `packaging/portable/verify.sh` passed both Debian 12 and Debian 13 at 512 MiB and 256 MiB with two CPUs. Every health sample reported:

```json
{"release": {"deployment_id": "vps-623b55a1704195b89316de54673f7ae5e781cb15", "git_sha": "623b55a1704195b89316de54673f7ae5e781cb15"}, "status": "available"}
```

Evidence is under `/tmp/portable-verify`; the verifier exited 0.

## Log


- `65b4a0a4` started this checklist.
- Mac final tip `316de915` merged cleanly; the merge commit preserves its ancestry.
- Stream retry tip `824b181b` merged cleanly; the merge commit preserves its ancestry.
- Build-cache tip `fd5ffece` merged with the two conflicts above; both sides were kept.
- `623b55a1` restricted the commit definition to the three consumers and removed it from the startup object and link line.


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


## Committed-diff review

Base `origin/claude/cpp-gateway-port` (`0c545781`), head `83371c33`, model `gpt-6.2-sol`, exit 0.

```sh
timeout -k 5s 600s codex review --base origin/claude/cpp-gateway-port \
  -c 'model="gpt-6.2-sol"' -c 'review_model="gpt-6.2-sol"' \
  -c 'sandbox_mode="read-only"' -c 'shell_environment_policy.inherit="core"'
```

The complete result contained one finding:

> [P2] Keep transient SSE throttling distinct from quota exhaustion — `/home/runner/work/_temp/agent-jobs/cppgw-combine-3-20261120/checkout/cpp/gateway/src/codex/index.cpp:88-89`
>
> When Codex sends a generic `rate_limit_exceeded` or `too_many_requests` event with a retry hint, the stream classifier assigns it status 429. Passing that synthetic response to `recordCodexResponseHealth` writes a durable `quota_exhausted` observation, so an account that was briefly throttled can appear exhausted in provider health, even if the next attempt succeeds and leaves a false `last_429_at_ms`. Keep the retry behavior, but record exhaustion only when the event supplies authoritative quota evidence, consistent with [AGENTS.md:14](AGENTS.md#L14).

This is a stream-retry health-classification nuance rather than a merge or cache regression. It is recorded for a focused follow-up so this delivery keeps the reviewed branch's retry and settlement behavior unchanged. The combined release tests, including the 32 stream-retry cases and the loopback routing case, pass.
