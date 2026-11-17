# Authority, kernel and paid-fallback sanitizer findings — 2026-11-17

## Contract v1

Objective: build and execute the 50 top-level `tests/*_test.cpp` binaries whose
names start with `native_`, `authority_`, `kernel_`, `paid_`, `api_`, `quota_`, or
`worker_`, first without sanitizers and then with AddressSanitizer and UBSan.
Preserve the TypeScript/Bun 1.4.0 behavioral oracle, every assertion, module
structure, strict warnings and the documented coroutine/lifetime rules.

Owned production paths are `src/{authority,kernel,paid_fallback,worker}/**`,
`src/{api_keys,api_key_policy}.cpp` and their matching `include/uos` headers.
Owned fixtures are this shard's tests/vectors, `tests/helpers/native_*` and
`tests/lsan-authority.supp`. All paths in this report are under `cpp/gateway`
unless stated otherwise. Other sources, shared runtime, build tools, CI,
TypeScript, deployment, credentials and live provider calls are outside scope.
Report defects outside ownership with exact commands and initial diagnostics.

Matrix: Linux x86_64, Clang 21.1.8/libstdc++, normal `-O1 -g`, followed by
`-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
-fno-sanitize-recover=undefined`. Every test links all 44 runtime sources and
its exact existing Links closure; compile each source once per profile.
No Darwin, GCC 12, production or provider acceptance is claimed.

Acceptance surface: all 50 standalone binaries in each profile, warning-fatal
C++ compilation, driver syntax, changed-document formatting and
`git diff --check`. Use fresh empty HOME/TMPDIR/cwd, a credential-free
environment allowlist, leak/stack-use-after-return/strict-string detection,
UBSan stack traces and a 600-second process-group timeout per binary. Record
actual case counts and retain unsuppressed first-failure evidence. Suppress only
a narrowly documented JavaScript-model ownership-cycle limitation, if observed.

Unknowns: runner dependency versions, native fixture preparation, platform-specific
expectations and sanitizer failures. Resolve each from current commands or the
Bun oracle before changing behavior. Stop when both sweeps have terminal receipts
and every failure is fixed or explicitly reported at its ownership boundary.

Long-operation budget: four compiler workers, approximately 15–40 minutes per
initial profile, serial test execution, maximum 600 seconds per binary. Keep
objects, full command/log receipts and downloads under `/tmp`, emit compact
progress, and diagnose failed commands before rerunning. Incremental rebuilds
must track consumed headers. No generated artifacts belong in the checkout.

## Discovery checkpoint

- Starting commit: `141cb4d4` on clean `agent-job`.
- Read `PORTING.md`, `docs/PORT-NOTES.md`, `docs/TESTING.md`, `docs/BUILD.md`,
  both prior runner reports, runtime coroutine/lifetime guidance and the existing
  runtime sanitizer driver. The user explicitly overrides production-host build
  restrictions on this disposable runner.
- Inventory: 50 binaries; 283 unique resolved module/support sources plus 44
  runtime sources. All 50 Links declarations resolve successfully.
- Host: Linux x86_64, four CPUs, approximately 15 GiB RAM, Clang 21.1.8,
  GCC 15.2, Bun 1.4.0. Boost.JSON/Container and Brotli development packages need
  preparation. No test has run yet.
- `gh auth status` reports no authenticated host. Publication will be attempted
  without exposing or writing any model gateway credential.

## Findings

| Test                             | Kind             | Cause                                                                                                | Fix or disposition                                                                                                                                                                                                                              |
| -------------------------------- | ---------------- | ---------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Discovery                        | Inventory        | 50 test binaries                                                                                     | Normal build and run pass 50 binaries / 444 cases                                                                                                                                                                                               |
| `native_addon_differential_test` | Host portability | ARM64 provenance hashes encode `process.arch`; native development bootstrap deliberately rejects x64 | Bun 1.4.0 replay of all 50 artifact and 28 bootstrap cases records seven and 26 changed outcomes respectively. Preserve original ARM vectors, add separate Linux x64 vectors, build fixture paths independently so all 28 read attempts execute |

## Dependency and oracle checkpoint

Installed Boost.JSON/Container 1.90, Brotli 1.2, CMake 4.2.3 and Ninja 1.13.2:

```sh
sudo -n apt-get update -qq
sudo -n apt-get install -y -qq --no-install-recommends libboost-json-dev libboost-container-dev libbrotli-dev cmake ninja-build
```

Existing system dependencies: OpenSSL 3.5.5, SQLite 3.46.1, zlib 1.3.1,
ICU 78.2 and Zstd 1.5.7. SQLite meets this shard's stated minimum; use an exact
newer library only if a test demonstrates a version-specific requirement.

The new `tests/helpers/native_shard_checks.py` is a shard-scoped equivalent of
the prior runtime driver: external CMake/Ninja project, one object per source,
exact per-test link closures, isolated serial execution, durable JSON/log receipts
and input-hash/commit reconciliation. It never inherits provider variables or
credentials. Linux tests run after `sudo unshare --net`, loopback setup, external
route-denial preflight, UID/GID drop and capability removal. No dependency,
binary, build directory or downloaded corpus is placed in the checkout.

Initial normal build command:

```sh
python3 cpp/gateway/tests/helpers/native_shard_checks.py --profile plain --jobs 4
```

Bun host observation, using an empty environment and fresh HOME/TMPDIR/cwd:

```text
{"bun":"1.4.0","platform":"linux","arch":"x64"}
{"name":"Error","message":"Native KV development platform is unsupported"}
```

`tests/helpers/native_addon_oracle.mjs` replays the original specs and fixtures
through the actual TypeScript `verifyNativeKvArtifact` and
`readNativeKvDevelopmentBinding`, without loading an addon. Run it with Bun
1.4.0 `--no-env-file --no-install` in that same isolated environment, saving
stdout to a temporary `.inc`, then compare it with
`tests/native_addon_linux_x64_vectors.inc`. All original 50/28 specs, four test
cases and ARM expected values remain; no case is skipped. The x64 recording
correctly changes the `architecture both x64` mutation into a successful artifact
verification because x64 matches this host. No production platform guard changes.

## Reproduction and terminal results

Normal checkpoint: all **377 source/test objects** compile warning-free, all
**50 binaries** link, and execution passes **444/444 cases**, zero failures or
timeouts. The normal profile uses `-O1 -g` without sanitizers. Its run is
`/tmp/cppgw-asan-authority-20261117/plain/runs/1794914082261162107`.
These are real execution results, not yet a committed-tree acceptance receipt:
the fixture changed during the initial build, correctly setting
`inputs_unchanged: false` and making the driver return 1 despite passing tests.
The final clean-tree run will reconcile all hashes after writers stop.

Sanitizer bring-up begins with the six authority binaries (including native
authority/RPC integration), then expands to all 50 using the same object cache:

```sh
python3 cpp/gateway/tests/helpers/native_shard_checks.py --profile asan --filter authority_ --jobs 4
python3 cpp/gateway/tests/helpers/native_shard_checks.py --profile asan --jobs 4
```

The initial authority sanitizer subset passes **6/6 binaries, 94/94 cases**,
zero ASan/UBSan/LSan diagnostics and zero suppressions. This includes all 65
native authority integration cases. Run:
`/tmp/cppgw-asan-authority-20261117/asan/runs/1794915142374129637`.
The driver was improved during this preliminary run to discover the compiler's
matching LLVM symbolizer and distinguish passing tests from stale provenance;
the final committed-tree receipt supersedes its input/identity checkpoint.

No production source change is required by either completed checkpoint.
The five task paths so far are the shard driver, Bun oracle generator,
Linux x64 vector file, native-addon portability fixture and this findings file.
Both the original ARM vector payload (SHA-256
`85ebd0f64ead1923adb896f2aea5871f48f9ee5e71f90f8b86676629e832b3f5`)
and all 50/28 scenario specs remain byte-for-byte unchanged.

Full-shard ASan/UBSan results and final committed-tree receipts are pending.
