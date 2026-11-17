# Authority, kernel and paid-fallback sanitizer findings — 2026-11-17

**Final local result: 50/50 binaries and 444/444 cases pass in each profile.**
No failures, timeouts, skips, ASan/UBSan/LSan diagnostics or suppressions.
GitHub write authentication prevents direct publication and development integration.

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

| Test                             | Kind                | Cause                                                                                                | Fix or disposition                                                                                                                                                                                                                              |
| -------------------------------- | ------------------- | ---------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Discovery                        | Inventory           | 50 test binaries                                                                                     | Normal build and run pass 50 binaries / 444 cases                                                                                                                                                                                               |
| `native_addon_differential_test` | Host portability    | ARM64 provenance hashes encode `process.arch`; native development bootstrap deliberately rejects x64 | Bun 1.4.0 replay of all 50 artifact and 28 bootstrap cases records seven and 26 changed outcomes respectively. Preserve original ARM vectors, add separate Linux x64 vectors, build fixture paths independently so all 28 read attempts execute |
| Full shard                       | ASan / UBSan / LSan | No memory-safety or undefined-behavior defect observed in the selected tests                         | All 50 binaries / 444 cases pass; no production source change or suppression needed                                                                                                                                                             |

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
The final clean-tree run below reconciles all hashes after writers stopped.

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

## Final committed-source receipts

Both profiles pass **50/50 binaries, 444/444 cases**, zero failed cases,
timeouts, skips, ASan/UBSan/LSan diagnostics or suppressions. Each profile has
**377 warning-fatal compilations**: 327 production/support objects (including all
44 runtime sources) and 50 test objects, followed by 50 successful links.
No first-party production C++ source or header needed a change.

- Source commit: `2f339818540dd51dfee45bd2f92f1ca804b2befb`.
- Tree: `eddc81f6bcb61d482b1de3be104449873d7f390d`.
- Both receipts have `worktree_clean`, `inputs_unchanged` and
  `identity_unchanged` equal to true; all 742 first-party input hashes matched.
- Only this findings report changes after that checkpoint. The compiler inputs,
  tests, vectors and drivers remain identical to the tested/reviewed commit.

Exact complete acceptance commands, from the repository root:

```sh
python3 cpp/gateway/tests/helpers/native_shard_checks.py --profile plain --jobs 4 --require-clean
python3 cpp/gateway/tests/helpers/native_shard_checks.py --profile asan --jobs 4 --require-clean
```

For a fresh runner, `--profile both` executes the same normal-then-sanitizer
pipeline. The default timeout is 600 seconds per binary. All build output is in
`/tmp/cppgw-asan-authority-20261117/{plain,asan}`; `compile_commands.json` contains
all expanded compiler commands, `closures.json` the exact link lists, `build.log`
the CMake/Ninja commands and output, and each `runs/<id>` contains per-test logs,
`results.json` and `provenance.json`. Every test has fresh HOME/TMPDIR/cwd and an
allowlisted environment, private network namespace, root/capability drop and
process-group timeout. No provider or credential environment is inherited.

Common compiler flags (Clang 21.1.8, libstdc++ 15):

```text
-std=c++20 -Wall -Wextra -Wshadow -Wconversion -Werror
-DBOOST_JSON_NO_LIB -DBOOST_CONTAINER_NO_LIB -DBOOST_SYSTEM_NO_LIB
-DUOS_GATEWAY_NATIVE_LIBRARIES -pthread -O1 -g
-I <absolute-checkout>/cpp/gateway/include -I <absolute-checkout>/cpp/gateway
```

The sanitizer profile additionally compiles with
`-fsanitize=address,undefined -fno-omit-frame-pointer
-fno-sanitize-recover=undefined` and links with `-fsanitize=address,undefined`.
Every binary links its test object, all 44 runtime objects, its exact Links
objects, Boost.JSON, Boost.Container, OpenSSL (`ssl`, `crypto`), pthread, dl,
SQLite, zlib, Brotli decoder, Zstd and ICU (`icuuc`). Sanitizer execution sets:

```text
ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1
UBSAN_OPTIONS=print_stacktrace=1
ASAN_SYMBOLIZER_PATH=/usr/lib/llvm-21/bin/llvm-symbolizer
```

No LSan suppression file or disabled instrumentation was needed.

| Receipt               | Exact run / SHA-256                                                 |
| --------------------- | ------------------------------------------------------------------- |
| Normal run            | `/tmp/cppgw-asan-authority-20261117/plain/runs/1794915499466536235` |
| Normal results        | `6ab238209308395589fe131dc51397ff5a9e3dfe751687592d440a80e234283d`  |
| Normal provenance     | `e0bf4c5fa3152e16a7530c5353450afcafca44bcaa6cd9ef1a4acf81c83f4bb3`  |
| ASan/UBSan run        | `/tmp/cppgw-asan-authority-20261117/asan/runs/1794915448405899194`  |
| ASan/UBSan results    | `d3757fe8d9a69a8c65de8c066541f2ba685190bdeb2c9fea3e006d5c888da9f9`  |
| ASan/UBSan provenance | `42c316c88e621af4d16b2c968e1feacf5d293397452f446566f624e49cf4fb83`  |

The serial test runtime totals were 17.774 seconds normal and 34.984 seconds
sanitized. Build time is separate. Counts below overlap the preliminary runs;
those checkpoints are not added to the final totals.

## Every changed path and its evidence

| Path                                             | Change, reason and evidence                                                                                                                                                                                                                        |
| ------------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `tests/native_addon_differential_test.cpp`       | Select the separately recorded Linux x64 vectors and construct bootstrap fixture paths independently of the host guard. All original four cases and 78 specs execute; all four cases pass in both profiles. Original ARM payload unchanged.        |
| `tests/native_addon_linux_x64_vectors.inc`       | New Bun 1.4.0 host recording: seven artifact outcomes and 26 bootstrap outcomes differ from ARM64. The isolated real-TypeScript replay reproduces the committed bytes, SHA-256 `b2b2556bdd4c3c692916c319b2017c9ce2e456052ac5aefd4f176f4fa1cbacdc`. |
| `tests/helpers/native_addon_oracle.mjs`          | Reproducible recording of the original 50 artifact and 28 bootstrap specs against actual TypeScript modules. Bun replay, byte comparison and changed-file Prettier pass. No native addon or provider is loaded.                                    |
| `tests/helpers/native_shard_checks.py`           | Portable shard-only object/link/run driver with credential-free OS isolation, strict flags, symbolizer discovery, timeout handling and clean-tree/hash receipts. Both complete runs pass; Python syntax passes.                                    |
| `docs/findings/cppgw-asan-authority-20261117.md` | Early contract and findings, exact commands, original host evidence, full counts, source/tree receipts, review and publication limits. Changed-document formatting and whitespace pass.                                                            |

Exact oracle reproduction, with task-owned scratch and no dotenv/automatic installs:

```sh
UOS_REPO=$(pwd)
UOS_ORACLE_SCRATCH=$(mktemp -d /tmp/cppgw-authority-oracle.XXXXXX)
mkdir -p "$UOS_ORACLE_SCRATCH/home" "$UOS_ORACLE_SCRATCH/tmp" "$UOS_ORACLE_SCRATCH/work"
cd "$UOS_ORACLE_SCRATCH/work"
env -i PATH=/home/runner/.bun/bin:/usr/bin:/bin HOME="$UOS_ORACLE_SCRATCH/home" TMPDIR="$UOS_ORACLE_SCRATCH/tmp" TZ=UTC \
  /home/runner/.bun/bin/bun --no-env-file --no-install "$UOS_REPO/cpp/gateway/tests/helpers/native_addon_oracle.mjs" \
  > "$UOS_ORACLE_SCRATCH/expected.inc"
cmp "$UOS_ORACLE_SCRATCH/expected.inc" "$UOS_REPO/cpp/gateway/tests/native_addon_linux_x64_vectors.inc"
cd "$UOS_REPO"
```

The final replay also ran behind the same private network namespace, UID/GID and
capability drop as the C++ tests; its logs are in
`/tmp/cppgw-asan-authority-20261117/oracle/final-native-addon.{inc,log}`.
It reported `verify: 50 real Bun outcomes, 7 platform-dependent outcomes` and
`bootstrap: 28 real Bun outcomes, 26 platform-dependent outcomes`.

## Verification scope and local review

Changed C++/vector paths map to the 50 binary sweeps and strict compiler/type gate;
the Python driver maps to syntax and both actual executions; the oracle generator
maps to Bun replay/byte comparison and formatting; this report maps to formatting
and whitespace. No application TypeScript or Rust source/import changed, so no
unrelated application checker, test aggregate, native deployment or hosted CI run
was selected. No changed file is near its size cap.

Frozen quality dependencies were prepared outside the checkout with both locks
present and unchanged:

```sh
test -f bun.lock && test -f tools/lint/bun.lock
mkdir -p /tmp/cppgw-asan-authority-20261117/lint
cp tools/lint/package.json tools/lint/bun.lock /tmp/cppgw-asan-authority-20261117/lint/
bun --no-env-file install --frozen-lockfile --ignore-scripts --cwd /tmp/cppgw-asan-authority-20261117/lint
bun --no-env-file --no-install /tmp/cppgw-asan-authority-20261117/lint/node_modules/prettier/bin/prettier.cjs --check \
  cpp/gateway/docs/findings/cppgw-asan-authority-20261117.md cpp/gateway/tests/helpers/native_addon_oracle.mjs
python3 - <<'CHECK_PY'
import ast
from pathlib import Path
ast.parse(Path('cpp/gateway/tests/helpers/native_shard_checks.py').read_text())
CHECK_PY
git diff --check
```

Actual local review command, Codex CLI 0.169.0:

```sh
codex -m codex-auto-review -c 'review_model="codex-auto-review"' review --commit 2f339818540dd51dfee45bd2f92f1ca804b2befb
```

Reviewed head is the source commit above; base is
`141cb4d482a3b0d8624ce0411cbd3b93e45873e6`. The command exited 0 in 128.614
seconds, with `model: codex-auto-review`, `provider: uos` and read-only sandbox
confirmed in the banner. It produced no finding entries. No finding was ingested
and review latency/status was not a gate. The unredacted, credential-free transcript
is `/tmp/cppgw-asan-authority-20261117/review.log`, SHA-256
`63f7237a1e48f4d937a13fc21d716de9386e42134205cc526c499efe45a89fff`.

## Per-binary final counts

| Binary                                                        | Cases | Normal | ASan/UBSan |
| ------------------------------------------------------------- | ----: | ------ | ---------- |
| `api_key_policy_differential_test`                            |     1 | PASS   | PASS       |
| `api_key_policy_dispatch_reuse_test`                          |     7 | PASS   | PASS       |
| `api_keys_test`                                               |     4 | PASS   | PASS       |
| `authority_admission_test`                                    |     9 | PASS   | PASS       |
| `authority_differential_test`                                 |     3 | PASS   | PASS       |
| `authority_durable_test`                                      |    10 | PASS   | PASS       |
| `authority_publication_test`                                  |     4 | PASS   | PASS       |
| `authority_settlement_test`                                   |     3 | PASS   | PASS       |
| `kernel_attestation_differential_test`                        |     1 | PASS   | PASS       |
| `kernel_coverage_test`                                        |     8 | PASS   | PASS       |
| `kernel_policy_queue_differential_test`                       |     1 | PASS   | PASS       |
| `kernel_quota_admission_test`                                 |     1 | PASS   | PASS       |
| `kernel_quota_context_differential_test`                      |     5 | PASS   | PASS       |
| `kernel_quota_reserve_differential_test`                      |    16 | PASS   | PASS       |
| `kernel_quota_values_differential_test`                       |     6 | PASS   | PASS       |
| `kernel_quota_write_differential_test`                        |    25 | PASS   | PASS       |
| `kernel_usage_coverage_test`                                  |    16 | PASS   | PASS       |
| `native_addon_differential_test`                              |     4 | PASS   | PASS       |
| `native_api_key_audit_conformance_test`                       |     5 | PASS   | PASS       |
| `native_api_key_conformance_test`                             |     3 | PASS   | PASS       |
| `native_api_key_differential_test`                            |     1 | PASS   | PASS       |
| `native_api_key_policy_differential_test`                     |     9 | PASS   | PASS       |
| `native_auth_verification_test`                               |     4 | PASS   | PASS       |
| `native_authority_test`                                       |    65 | PASS   | PASS       |
| `native_capture_operator_test`                                |     2 | PASS   | PASS       |
| `native_codex429_conformance_test`                            |     2 | PASS   | PASS       |
| `native_codex_429_differential_test`                          |     3 | PASS   | PASS       |
| `native_codex_capacity_test`                                  |     1 | PASS   | PASS       |
| `native_codex_conformance_test`                               |     1 | PASS   | PASS       |
| `native_codex_evaluation_differential_test`                   |     5 | PASS   | PASS       |
| `native_codex_mutations_differential_test`                    |     2 | PASS   | PASS       |
| `native_contracts_differential_test`                          |     2 | PASS   | PASS       |
| `native_kernel_conformance_test`                              |     5 | PASS   | PASS       |
| `native_kernel_contracts_differential_test`                   |     3 | PASS   | PASS       |
| `native_kernel_differential_test`                             |     3 | PASS   | PASS       |
| `native_paid_conformance_test`                                |     5 | PASS   | PASS       |
| `native_paid_contracts_differential_test`                     |     6 | PASS   | PASS       |
| `native_paid_differential_test`                               |     7 | PASS   | PASS       |
| `native_paid_policy_differential_test`                        |     6 | PASS   | PASS       |
| `native_principal_control_test`                               |    49 | PASS   | PASS       |
| `native_runtime_binding_test`                                 |    50 | PASS   | PASS       |
| `paid_fallback_index_local_development_key_differential_test` |     2 | PASS   | PASS       |
| `paid_fallback_ledger_state_differential_test`                |     4 | PASS   | PASS       |
| `paid_fallback_ledger_stats_differential_test`                |     2 | PASS   | PASS       |
| `paid_fallback_provider_usage_projection_test`                |     6 | PASS   | PASS       |
| `paid_fallback_rollups_differential_test`                     |     4 | PASS   | PASS       |
| `paid_fallback_test`                                          |    25 | PASS   | PASS       |
| `paid_fallback_v3_cutover_test`                               |     2 | PASS   | PASS       |
| `quota_projection_test`                                       |    30 | PASS   | PASS       |
| `worker_boundary_test`                                        |     6 | PASS   | PASS       |

## Outside-owned defects and unfinished delivery

No defect outside the owned paths was observed during these builds or runs.
No production source defect was observed in the selected tests. Linux x64 source
rejection of native development bootstrap is intentional TypeScript behavior,
not a platform guard to remove. The original supported ARM64 bootstrap receipt
branches are preserved but are not runtime-qualified by this x64 job. Darwin,
production launch, live databases and provider/client acceptance remain outside scope.

Direct publication attempt:

```sh
GIT_TERMINAL_PROMPT=0 git push origin HEAD:refs/heads/test/cppgw-asan-authority-20261117
```

It exits 128:

```text
fatal: could not read Username for 'https://github.com': terminal prompts disabled
```

`gh auth status` has no authenticated host. A fresh read-only fetch of
`origin/development` returned `002fe15fa926674c97f0169355d24929bf529710`;
`git merge-base --is-ancestor 2f339818540dd51dfee45bd2f92f1ca804b2befb origin/development`
exits 1. No PR, ancestry-preserving development merge, hosted receipt or final
development checkout handoff is claimed. The enclosing authenticated job owns
publication/integration next; preserve the clean owned `agent-job` branch and its
pre-existing port/build ancestry. Never merge that unknown earlier work merely
to satisfy a checkout handoff. No hosted workflow was dispatched.

## Final handoff summary

Built 377 objects and linked all 50 shard binaries per profile with Clang 21.1.8,
strict C++20 warnings, normal `-O1 -g` and the requested ASan/UBSan flags. Exact
acceptance commands:

```sh
python3 cpp/gateway/tests/helpers/native_shard_checks.py --profile plain --jobs 4 --require-clean
python3 cpp/gateway/tests/helpers/native_shard_checks.py --profile asan --jobs 4 --require-clean
```

Both profiles pass **50 binaries / 444 cases**, with zero failures, timeouts,
skips, sanitizer reports or suppressions. Source commit/tree and all 742 input
hashes match. The native-addon test now uses Bun-proven Linux x64 outcomes while
retaining every ARM expectation and all 78 specs; the new `.inc`, Bun recorder and
isolated runner make that evidence reproducible. This findings file records the
full receipts. No production C++ source/header change was needed.

No outside-owned defect remains to report. Platform/production qualification is
outside scope. GitHub authentication prevents direct push, PR and development
integration; the authenticated enclosing job must publish and integrate the
committed clean task branch. All application tests and oracle probes used isolated fixtures.
