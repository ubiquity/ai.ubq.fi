# Codex module ASan/UBSan bring-up, 2026-11-17

Final local result: plain **71/71 binaries, 831/831 cases**; ASan/UBSan **70/71 binaries, 830 reported passing cases**. Four supervisor leak reports are fixed. The sole remaining LSan failure is the unowned HTTP runtime cycle described below; it remains unsuppressed.

## Work contract v1

Objective: build and run every top-level `tests/{codex_,banked_reset,cluster_}*_test.cpp`
first without sanitizers, then with Clang AddressSanitizer and UndefinedBehaviorSanitizer.
Preserve the TypeScript/Bun 1.4.0 oracle, assertion values, observable ordering, and
the documented coroutine rules. Fix root causes only in the assigned Codex/model
sources, their headers, shard tests/vector files, and `tests/helpers/codex_*`.
Other owned outputs are this report and, only for a documented model limitation,
`tests/lsan-codex.supp`.

Non-goals: other modules/runtime changes, host-specific build-tool repairs,
TypeScript or workflow changes, deployment, live credentials, production data,
and external provider requests. Outside-owned defects are reported with exact
commands and first diagnostics. No delegated agents are used.

Behavior matrix: all selected tests run unchanged in the plain profile and the
same tests run with ASan/UBSan and leak detection; supported providers remain
mocked/local fixtures. Every binary receives a fresh empty HOME, TMPDIR and cwd,
an allowlisted credential-free environment, and a 600-second process-group timeout.
All runtime translation units link into every binary; module Links closures select
the remaining objects. Each source compiles once per profile, with four workers.

Acceptance surface: warning-fatal C++20 compile/link, plain shard execution,
sanitized shard execution, regression cases for newly exposed paths, changed
document formatting, and `git diff --check`. No unrelated TypeScript aggregate
is selected. Before the final receipt all source writers stop; record commit/tree,
consumed-input hashes, commands, and actual counts. Material changes to this
contract invalidate affected evidence and increment its version.

Unknowns at discovery: distribution dependency availability, closure/link errors,
host-specific fixture assumptions, sanitizer failures, and publication permissions.
Stop when all selected binaries have both receipts and observed failures have
either owned fixes with reruns or exact outside-ownership reports. A blocked
publication remains explicitly incomplete for the enclosing job's publisher.

Long-operation budget: four compile workers, expected 10–30 minutes per cold
profile, incremental objects and full logs under `/tmp/cppgw-asan-codex-20261117`.
Observe live progress at most once a minute and diagnose failed commands before
retry. Runtime is expected to be seconds per binary; preserve all failure logs,
kill the process group after 600 seconds, and count every selected test.

## Discovery checkpoint

- Starting commit: `141cb4d4` on clean `agent-job`; this includes pre-existing
  port and production-build work that this task does not accept as its own.
- Host: Linux x86_64, four CPUs, 15 GiB RAM; Clang 21.1.8; Bun 1.4.0.
- Read the requested porting, build, testing, conventions, runtime async lifetime,
  runtime sanitizer, and libc++ portability guidance. Production-host resource
  restrictions are explicitly waived by this disposable-runner task.
- Dependency preparation, exact commands, pass/fail counts, and findings follow
  as execution produces evidence. No passing result is claimed at this checkpoint.
- Census: 71 selected top-level test binaries, 271 unique module/helper sources,
  44 runtime sources; 386 translation units per profile, plus 71 links. All Links
  closures resolve; none has a registry environment override or skip.
- Installed Boost.JSON/Container 1.90, Brotli 1.2.0, CMake 4.2.3 and Ninja 1.13.2.
  Existing development libraries include OpenSSL 3.5.5, SQLite 3.46.1, zlib 1.3.1,
  ICU 78.2 and Zstd 1.5.7. No dependency was downloaded inside the checkout.
- `sudo -n unshare --net` succeeds. Runs will enable only loopback, then drop
  to the original runner UID/GID 1001 with no supplementary groups. External
  routes and credential variables are absent.
- The initial publication preflight fails without GitHub write authentication;
  preserve this branch for the enclosing runner publisher. No credentials were
  printed, copied, refreshed, or persisted.

## Commands and current execution

```sh
sudo apt-get update -qq
sudo apt-get install -y -qq --no-install-recommends libboost-json-dev libboost-container-dev libbrotli-dev ninja-build cmake
python3 cpp/gateway/tests/helpers/codex_sanitizer_checks.py --profile plain --build-only --jobs 4
```

The shard runner generates its CMake project outside the checkout, uses per-source
object targets and the exact existing Links source order, and serializes links.
Both profiles use `-std=c++20 -Wall -Wextra -Wshadow -Wconversion -Werror
-DBOOST_JSON_NO_LIB -DBOOST_CONTAINER_NO_LIB -DBOOST_SYSTEM_NO_LIB
-DUOS_GATEWAY_NATIVE_LIBRARIES -pthread -O1 -g` and the two gateway include paths.
The sanitizer profile adds `-fsanitize=address,undefined -fno-omit-frame-pointer
-fno-sanitize-recover=undefined`. Links include Boost.JSON/Container, OpenSSL,
pthread, dl, SQLite, zlib, Brotli, Zstd and ICU. Expanded compiler argv are retained
in each profile's `compile_commands.json`; full configure/build output is in
`build.log`. Per-run JSON receipts fingerprint source/header/test/vector inputs,
record actual counts and source identity, and reject changes during execution.
Dirty discovery receipts deliberately have null commit/tree fields.

Publication preflight:

```sh
git -c credential.helper= -c core.askPass=/bin/false push --dry-run origin HEAD:refs/heads/test/cppgw-asan-codex-20261117
```

Exit 128: `fatal: could not read Username for 'https://github.com': No such device or address`.

## Findings

| Test                                               | Tool                                 | Cause                                                                                         | Fix or disposition                                                                                                                          |
| -------------------------------------------------- | ------------------------------------ | --------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| `codex_supervisor_secret_test`                     | Clang 21 `-Werror,-Wshadow`          | Explicit lambda init-captures shadow their structured-binding source names at line 106        | Rename the two capture bindings; every generated case, fixture and assertion is unchanged                                                   |
| Shard runner                                       | Local preflight audit                | Direct invocation could inherit the host network namespace                                    | Fail closed before building/running tests unless the Linux namespace exposes only `lo`; host invocation exits 1 before launching any binary |
| `codex_supervisor_brief_handler_differential_test` | LSan, 30,848 bytes / 456 allocations | Socket event listeners retain `ConnectionState`, which owns the socket                        | Three weak event captures; final plain and sanitized reruns both pass                                                                       |
| `codex_supervisor_followups_test`                  | LSan, 9,317 bytes / 126 allocations  | Same supervisor transport ownership cycle                                                     | Same owned root fix; final plain and sanitizer reruns pass                                                                                  |
| `codex_supervisor_handlers_differential_test`      | LSan, 23,188 bytes / 340 allocations | Same cycle on successful close and failed socket opening                                      | Both new lifetime regressions fail before the fix and pass after it; all three cases are sanitizer-clean                                    |
| `codex_supervisor_inventory_coverage_test`         | LSan, 48,679 bytes / 843 allocations | Same cycle retains scripted sockets, frames, callbacks and handshake promises                 | Same owned root fix; final plain and sanitizer reruns pass                                                                                  |
| `codex_serial_routing_http_test`                   | LSan, 5,824 bytes / 45 allocations   | `runtime/http.cpp` stores an abort callback capturing its own `ServeState` through `shutdown` | Outside owned paths: report only; no suppression or fixture workaround                                                                      |

Initial plain compilation: 385/386 translation units passed; the one failing unit
reported two instances of `error: declaration shadows a structured binding
[-Werror,-Wshadow]` on `[wrap = wrap, value = value]`. No other compile error
occurred. No source warning or test assertion was disabled. Incremental rebuild
then passed all 386 translation units and all 71 links.

## Plain execution checkpoint

The first complete plain run passed **71/71 binaries, 829/829 cases**, zero
failures, skips, crashes, or timeouts. The summed per-binary runtime was 6.724 s.
All 41 cases of the mechanically changed supervisor-secret suite passed.

```sh
sudo -n unshare --net -- /bin/sh -c 'ip link set lo up && exec setpriv --reuid=1001 --regid=1001 --clear-groups env -i PATH=/usr/local/bin:/usr/bin:/bin /usr/bin/python3 /home/runner/work/_temp/agent-jobs/cppgw-asan-codex-20261117/checkout/cpp/gateway/tests/helpers/codex_sanitizer_checks.py --profile plain --run-only --require-clean'
python3 cpp/gateway/tests/helpers/codex_sanitizer_checks.py --profile asan --build-only --jobs 4
```

Plain receipt: `/tmp/cppgw-asan-codex-20261117/plain/runs/1794915532175469780`.
Commit `83d8a088a3f229b887b633866fc1296f6fab5dde`, tree
`8aa234f654ca8b1ff9991691b52cc9e0443a81a4`. All 986 fingerprinted inputs and
checkout identity stayed unchanged; `worktree_clean` was true.
`results.json` SHA-256:
`331e2997683aaca39f43d0da12983b3e47b7810866daa26db2da294bb3e5c203`.
`provenance.json` SHA-256:
`1588ea5b0389337d500b6b3ba05d3f4093a852aeca069abed0bc38cd255c0caf`.

The compiler's explicit `-print-prog-name=llvm-symbolizer` selects
`/usr/lib/llvm-21/bin/llvm-symbolizer`; sanitizer runs set `ASAN_SYMBOLIZER_PATH`
to it and refuse a missing tool. A separate network-namespace preflight observed
`connect_ex(("192.0.2.1", 443)) == 101` (`ENETUNREACH`). At this checkpoint the sanitizer build was
running, with no sanitized execution result claimed yet. Based on the measured
plain compile phase, budget 30–55 minutes for its instrumented cold build, with
the same four-worker limit and incremental recovery if a command fails.

## Initial sanitizer sweep and root-cause checkpoint

All 386 instrumented translation units and 71 links passed. The first unsuppressed
ASan/UBSan run executed all 71 binaries: **66 PASS, five LSan failures**, zero
UBSan diagnostics, invalid-access reports, or timeouts. The passing binaries
reported **779/779 cases**. The five LSan exits occurred before buffered harness
summaries were flushed; no passing-case count is inferred for those processes.

```sh
sudo -n unshare --net -- /bin/sh -c 'ip link set lo up && exec setpriv --reuid=1001 --regid=1001 --clear-groups env -i PATH=/usr/local/bin:/usr/bin:/bin /usr/bin/python3 /home/runner/work/_temp/agent-jobs/cppgw-asan-codex-20261117/checkout/cpp/gateway/tests/helpers/codex_sanitizer_checks.py --profile asan --run-only --require-clean'
```

Receipt: `/tmp/cppgw-asan-codex-20261117/asan/runs/1794917514646346783`.
Commit `73437d33fae1b2f39468b0b10e669c0c0c0f25d5`, tree
`607d7c88dd63e9b18d5a908f189a9b819d75171c`; inputs and identity stayed unchanged.
`results.json` SHA-256 `2143b8c3935ca6ac20ff38aab5288b075f81e11c1962d251245023483bf19ec3`;
`provenance.json` SHA-256 `84833c97b49595081e795ababc06e04193a11baebdb7d4210ef5556104393c53`.
The five full allocation reports remain in that receipt's `logs/` directory.

All four supervisor reports allocate retained listeners at original
`src/codex/supervisor_transport.cpp:193–195`, with socket construction at 191.
`ConnectionState -> socket -> listener -> ConnectionState` is an ordinary C++
ownership defect, not a documented JavaScript model limit. The three callbacks
now lock weak state owners per invocation; connection objects and in-flight
coroutines continue to retain state strongly. No await, callback order, payload,
error text, or expected vector changes. No LSan suppression is justified.

Two native lifetime cases were added to the matching handlers differential test:
release after normal close, and release when opening emits an error. Before the
production fix, the focused plain run printed **2 run, 0 passed, 2 failed**, both
at `EXPECT(socket.expired())`. Receipt:
`/tmp/cppgw-asan-codex-20261117/plain/runs/1794917701992770366` (dirty discovery
receipt with null commit/tree). Its reproducing commands are:

```sh
python3 cpp/gateway/tests/helpers/codex_sanitizer_checks.py --profile plain --build-only --jobs 4 --filter codex_supervisor_handlers_differential_test
sudo -n unshare --net -- /bin/sh -c 'ip link set lo up && exec setpriv --reuid=1001 --regid=1001 --clear-groups env -i PATH=/usr/local/bin:/usr/bin:/bin /usr/bin/python3 /home/runner/work/_temp/agent-jobs/cppgw-asan-codex-20261117/checkout/cpp/gateway/tests/helpers/codex_sanitizer_checks.py --profile plain --run-only --filter codex_supervisor_handlers_differential_test --case "supervisor transport releases"'
python3 cpp/gateway/tests/helpers/codex_sanitizer_checks.py --profile both --build-only --jobs 4
```

This production source edit invalidates affected previous passing receipts;
source-matching plain and sanitized reruns below supersede the affected earlier evidence.

## Outside-owned HTTP defect

`codex_serial_routing_http_test` already awaits shutdown of both its loopback
provider and gateway (`Fixture::stop()`); changing its assertions or cleanup
would hide a production ownership defect. `src/runtime/http.cpp:2002` stores
`state->abort = EventListener([shutdown] { ... })`, while the `shutdown` closure
at 1996 retains `state`. The callback is stored even when no abort signal was
supplied. Thus `ServeState -> abort callback -> shutdown -> ServeState` survives
normal shutdown and retains both server states, listeners and settled promises.

First report lines, with the template allocator frames omitted here but preserved
in the full log:

```text
ERROR: LeakSanitizer: detected memory leaks
Indirect leak of 608 byte(s) in 1 object(s) allocated from:
  uos::runtime::http::serveHttp(...) src/runtime/http.cpp:1914:18
  Fixture::start() tests/codex_serial_routing_http_test.cpp:106:13
Indirect leak of 608 byte(s) in 1 object(s) allocated from:
  uos::runtime::http::serveHttp(...) src/runtime/http.cpp:1914:18
  Fixture::start() tests/codex_serial_routing_http_test.cpp:95:14
SUMMARY: AddressSanitizer: 5824 byte(s) leaked in 45 allocation(s).
```

Reproduce after the instrumented build with the full sanitizer command above plus
`--filter codex_serial_routing_http_test`. Exit is 1. The source is outside
`src/codex`/`src/models`, so it remains unchanged and unsuppressed for its owner.

## Local review checkpoint

Actual local Codex review mode ran to exit 0 in 97.726 s:

```sh
timeout 180s codex -c 'review_model="codex-auto-review"' review --base 141cb4d482a3b0d8624ce0411cbd3b93e45873e6
```

Reviewed head: `0cccb394efe8a43366ca56020bf6281159d9d2aa`.
Exact base is in the command. CLI 0.169.0 reported `model: ubiquity/deepseek-flash`,
`provider: uos`, rather than confirming the requested review model. Reject this
receipt for official review ingestion: no finding is ingested, and no matching
review identity or official review completion is claimed. Review is not a gate.
The transcript was checked for environment credential values before writing;
none is printed or persisted. Log `/tmp/cppgw-asan-codex-20261117/review.log`,
SHA-256 `94876d1973a3ff6159eb3e3f63d19e07c2cb27643b4669e82a359ce41cdec0ce`.

Original returned findings (the CLI repeated both in its final output):

```text
- [P2] Enforce network isolation before launching tests — /home/runner/work/_temp/agent-jobs/cppgw-asan-codex-20261117/checkout/cpp/gateway/tests/helpers/codex_sanitizer_checks.py:164-165
  When this helper is invoked directly, its child test processes inherit the caller's network namespace; `network_namespace` is only recorded, not validated or changed. The documented `unshare --net` wrapper is used for one plain run but is not enforced by the helper, so a normal invocation can violate the credential-free, no-external-network isolation contract. Fail closed unless the process is in an isolated namespace, or create the namespace before launching each test.

- [P1] Complete the sanitizer execution before finalizing the report — /home/runner/work/_temp/agent-jobs/cppgw-asan-codex-20261117/checkout/cpp/gateway/docs/findings/cppgw-asan-codex-20261117.md:128-134
  The added report explicitly states that sanitized execution has not been run and leaves the final handoff marked "Work is in progress." This does not satisfy the stated objective of running all selected binaries under ASan/UBSan, so the change cannot provide the promised sanitizer acceptance evidence until the ASan/UBSan run and final receipts are recorded.
```

The independently reproduced network weakness is fixed in the runner, which now
records the checked interface list and refuses the host namespace before launching
tests. This preserves the already-used kernel isolation and makes it mandatory.
The incomplete-run observation described that intentional checkpoint. The final
both-profile execution below completes the assigned run and retains its one
explicit outside-ownership failure.

## Final source-matching verification

The final source/test implementation is commit
`09d35a1e44b761ad4fe10c8afe50bd41c1714134`, tree
`c2f7989e906250d9143a9078ad2bd2abaa3504ca`. Both profiles built **386/386**
translation units and **71/71** links with the warning-fatal flags above.
After all source writers stopped, one complete both-profile execution used:

```sh
python3 cpp/gateway/tests/helpers/codex_sanitizer_checks.py --profile both --build-only --jobs 4
sudo -n unshare --net -- /bin/sh -c 'ip link set lo up && exec setpriv --reuid=1001 --regid=1001 --clear-groups env -i PATH=/usr/local/bin:/usr/bin:/bin /usr/bin/python3 /home/runner/work/_temp/agent-jobs/cppgw-asan-codex-20261117/checkout/cpp/gateway/tests/helpers/codex_sanitizer_checks.py --profile both --run-only --require-clean'
```

- Plain: **71 PASS, zero failed binaries; 831 run, 831 passed, zero failed cases**;
  summed per-binary time 7.954 s.
- ASan/UBSan: **70 PASS, one LSan-failed binary**; the passing binaries report
  **830 run, 830 passed, zero failed cases**; summed per-binary time 25.677 s.
  The HTTP binary's buffered harness summary is unavailable after LSan exits;
  its separate plain run passes its one case. Do not interpret its missing
  sanitized summary as a skipped or passing case.
- Zero timeouts, skipped binaries, UBSan diagnostics, or invalid-access reports.
  No suppression file was created, and leak detection remained enabled.
- All four previously leaking supervisor binaries pass unsuppressed: brief
  handler 1 case, followups 14, handlers 3 (including both regressions), inventory
  coverage 33. All original Bun-recorded vectors and assertions remain unchanged.
- Both receipts have `worktree_clean`, `inputs_unchanged`, `identity_unchanged`
  true and checked network interfaces `['lo']`. All 986 fingerprinted inputs
  still match after execution. Sorted input-map SHA-256:
  `5f3aca3570bb4413d56131cdfef44e9d3f6788bdb29c6f20ae2ed4eada71412d`.
- Compiler: Clang 21.1.8; binary SHA-256
  `78e9dd2ccc9e88859b3a29109a44bfab00e3d6cb70f00f3daae063eed3d05251`.

| Profile    | Run directory under `/tmp/cppgw-asan-codex-20261117` | `results.json` SHA-256                                             | `provenance.json` SHA-256                                          |
| ---------- | ---------------------------------------------------- | ------------------------------------------------------------------ | ------------------------------------------------------------------ |
| Plain      | `plain/runs/1794917864086170993`                     | `bfb55344de1414c6d1cf1e6bd49384c46d67017b8e6b4b35c36b057299e974ca` | `433f6023085b3bc35d211cdfac663e626b6cc36e2e9a60628d49fa4df7e36344` |
| ASan/UBSan | `asan/runs/1794917872372228339`                      | `b46706a5e4bb03d3209e5b698e49d3d23200e605f89729962f6ad92804faf8c1` | `f19e40d8557f7672bc21b2d67baaf188a03f2fb9fe93cabfa355631c463cfcf7` |

Only documentation changes follow the implementation receipt. No consumed C++
source, header, test/vector or Python driver input changes afterwards.

The cases column below is the actual plain count. Every ASan PASS has the same
count; the one LSan failure has no flushed sanitized harness summary.

| Binary                                             | Cases | Plain | ASan/UBSan                    |
| -------------------------------------------------- | ----: | ----- | ----------------------------- |
| `banked_reset_branch_coverage_test`                |    25 | PASS  | PASS                          |
| `cluster_w04c1_headers_test`                       |     1 | PASS  | PASS                          |
| `cluster_w04c1_link_test`                          |     5 | PASS  | PASS                          |
| `cluster_w04c1_recorder_without_capacity_test`     |     1 | PASS  | PASS                          |
| `codex_account_affinity_test`                      |     3 | PASS  | PASS                          |
| `codex_account_email_test`                         |     2 | PASS  | PASS                          |
| `codex_account_routing_part1_test`                 |    26 | PASS  | PASS                          |
| `codex_account_routing_part2_test`                 |    31 | PASS  | PASS                          |
| `codex_account_routing_part3_test`                 |    23 | PASS  | PASS                          |
| `codex_auth_cache_part1_test`                      |    26 | PASS  | PASS                          |
| `codex_auth_cache_part2_test`                      |    20 | PASS  | PASS                          |
| `codex_auth_cache_part3_test`                      |    20 | PASS  | PASS                          |
| `codex_auth_cache_part4_test`                      |     5 | PASS  | PASS                          |
| `codex_auth_coverage_test`                         |    34 | PASS  | PASS                          |
| `codex_auth_parse_differential_test`               |     1 | PASS  | PASS                          |
| `codex_auth_repair_test`                           |    19 | PASS  | PASS                          |
| `codex_banked_reset_account_day_cap_test`          |     5 | PASS  | PASS                          |
| `codex_banked_reset_capacity_recovery_test`        |    12 | PASS  | PASS                          |
| `codex_banked_reset_daily_capacity_incident_test`  |     4 | PASS  | PASS                          |
| `codex_banked_reset_differential_test`             |    34 | PASS  | PASS                          |
| `codex_banked_reset_part1_test`                    |    36 | PASS  | PASS                          |
| `codex_banked_reset_part2_test`                    |    26 | PASS  | PASS                          |
| `codex_banked_reset_part3_test`                    |     1 | PASS  | PASS                          |
| `codex_banked_reset_provider_differential_test`    |     6 | PASS  | PASS                          |
| `codex_banked_reset_provider_test`                 |     7 | PASS  | PASS                          |
| `codex_capacity_routing_coverage_test`             |    20 | PASS  | PASS                          |
| `codex_catalog_metered_enrichment_test`            |     1 | PASS  | PASS                          |
| `codex_catalog_observation_test`                   |     6 | PASS  | PASS                          |
| `codex_catalog_reasoning_order_test`               |     4 | PASS  | PASS                          |
| `codex_catalog_stale_while_revalidate_test`        |     1 | PASS  | PASS                          |
| `codex_catalog_test`                               |    27 | PASS  | PASS                          |
| `codex_context_admission_test`                     |    16 | PASS  | PASS                          |
| `codex_dispatch_differential_test`                 |    13 | PASS  | PASS                          |
| `codex_experiment_differential_test`               |     3 | PASS  | PASS                          |
| `codex_external_auth_differential_test`            |     4 | PASS  | PASS                          |
| `codex_external_native_auth_test`                  |     7 | PASS  | PASS                          |
| `codex_manual_reset_state_differential_test`       |     3 | PASS  | PASS                          |
| `codex_manual_reset_state_test`                    |     4 | PASS  | PASS                          |
| `codex_manual_reset_test`                          |    13 | PASS  | PASS                          |
| `codex_metered_routing_matrix_test`                |     2 | PASS  | PASS                          |
| `codex_model_availability_test`                    |    26 | PASS  | PASS                          |
| `codex_model_switch_replay_test`                   |    12 | PASS  | PASS                          |
| `codex_models_availability_differential_test`      |     4 | PASS  | PASS                          |
| `codex_models_branch_coverage_test`                |    30 | PASS  | PASS                          |
| `codex_models_differential_test`                   |    11 | PASS  | PASS                          |
| `codex_models_fetch_differential_test`             |     5 | PASS  | PASS                          |
| `codex_native_auth_test`                           |    31 | PASS  | PASS                          |
| `codex_overage_fallback_test`                      |     8 | PASS  | PASS                          |
| `codex_overage_usage_settings_test`                |     5 | PASS  | PASS                          |
| `codex_quota_differential_test`                    |     3 | PASS  | PASS                          |
| `codex_reset_settings_migration_test`              |     9 | PASS  | PASS                          |
| `codex_responses_differential_test`                |     1 | PASS  | PASS                          |
| `codex_responses_error_differential_test`          |     1 | PASS  | PASS                          |
| `codex_responses_error_test`                       |     7 | PASS  | PASS                          |
| `codex_responses_operations_differential_test`     |     1 | PASS  | PASS                          |
| `codex_retry_after_test`                           |     3 | PASS  | PASS                          |
| `codex_routing_mutations_branch_coverage_test`     |     9 | PASS  | PASS                          |
| `codex_routing_mutations_differential_test`        |     1 | PASS  | PASS                          |
| `codex_routing_state_differential_test`            |     5 | PASS  | PASS                          |
| `codex_selection_differential_test`                |     1 | PASS  | PASS                          |
| `codex_serial_routing_http_test`                   |     1 | PASS  | FAIL: outside-owned HTTP LSan |
| `codex_session_test`                               |     3 | PASS  | PASS                          |
| `codex_supervisor_brief_handler_differential_test` |     1 | PASS  | PASS                          |
| `codex_supervisor_brief_test`                      |    37 | PASS  | PASS                          |
| `codex_supervisor_config_differential_test`        |    12 | PASS  | PASS                          |
| `codex_supervisor_followups_test`                  |    14 | PASS  | PASS                          |
| `codex_supervisor_handlers_differential_test`      |     3 | PASS  | PASS                          |
| `codex_supervisor_inventory_coverage_test`         |    33 | PASS  | PASS                          |
| `codex_supervisor_inventory_differential_test`     |     1 | PASS  | PASS                          |
| `codex_supervisor_secret_test`                     |    41 | PASS  | PASS                          |
| `codex_supervisor_test`                            |    15 | PASS  | PASS                          |

## Every changed path and evidence

| Path relative to `cpp/gateway`                          | Reason and evidence                                                                                                                                                                                                                                                                                                                                                                                             |
| ------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/codex/supervisor_transport.cpp`                    | Break socket/state ownership cycles with three weak listener captures. The four original LSan failures disappear; both new lifetime regressions pass, and all original callback/error/vector assertions remain unchanged.                                                                                                                                                                                       |
| `tests/codex_supervisor_handlers_differential_test.cpp` | Add close and failed-open lifetime regressions using the existing routed socket. Before the production fix: 2/2 fail at weak-reference expiration; after: both pass plain and sanitized, alongside the unchanged original differential.                                                                                                                                                                         |
| `tests/codex_supervisor_secret_test.cpp`                | Rename two lambda init-captures rejected by Clang 21 as structured-binding shadows. All 41 existing cases pass in both profiles; fixture values and assertions are identical.                                                                                                                                                                                                                                   |
| `tests/helpers/codex_sanitizer_checks.py`               | Build every source once per profile, honor exact Links order, retain commands/input hashes/receipts, require loopback-only kernel networking, allowlist child environments with fresh HOME/TMPDIR/cwd, select the matching symbolizer, and kill each test's process group after 600 s. Host-network negative preflight refuses execution; isolated positive preflight and both full profiles execute correctly. |
| `docs/findings/cppgw-asan-codex-20261117.md`            | This contract, all initial/final results, exact reproduction commands, source/receipt identities, review evidence and outside-owned defect handoff.                                                                                                                                                                                                                                                             |

No model source or public header changed. No expectation, case, vector, sanitizer
option, compiler warning, or outside-owned implementation was weakened or removed.
Bun 1.4.0 is installed, but no new TypeScript oracle observation was necessary:
only ownership/capture bindings changed, without moving awaits or observable
operations, and every recorded source vector remains checked.

Final auxiliary gates are Python AST syntax, the two changed tests' Links
resolution, the same locked Prettier 3.9.6 over this document, and whitespace.
The formatter is installed outside the checkout to honor the no-downloads/owned-
paths rules; the repository wrapper bootstraps in-checkout dependencies. Both
repository lockfiles remain unchanged. Exact preparation/check commands:

```sh
mkdir -p /tmp/cppgw-asan-codex-20261117/lint
cp tools/lint/package.json tools/lint/bun.lock /tmp/cppgw-asan-codex-20261117/lint/
bun --no-env-file install --frozen-lockfile --ignore-scripts --cwd /tmp/cppgw-asan-codex-20261117/lint
python3 -c 'import ast,pathlib; ast.parse(pathlib.Path("cpp/gateway/tests/helpers/codex_sanitizer_checks.py").read_text())'
python3 cpp/gateway/tools/links.py --check cpp/gateway/tests/codex_supervisor_handlers_differential_test.cpp cpp/gateway/tests/codex_supervisor_secret_test.cpp
bun --no-env-file --no-install /tmp/cppgw-asan-codex-20261117/lint/node_modules/prettier/bin/prettier.cjs --check cpp/gateway/docs/findings/cppgw-asan-codex-20261117.md
git diff --check
```

No TypeScript imports/types changed. No unrelated application test/lint/typecheck,
full verify aggregate, native release build, hosted CI, or deployment was selected.
All generated build/dependency/run artifacts stay under `/tmp`, with none inside
the checkout.

## Final local review receipt

The implementation commit was independently reviewed in the actual local review
mode, with both the session model and review model explicitly selected:

```sh
timeout 180s codex -m codex-auto-review -c 'review_model="codex-auto-review"' review --commit 09d35a1e44b761ad4fe10c8afe50bd41c1714134
```

Head: `09d35a1e44b761ad4fe10c8afe50bd41c1714134`; parent/base:
`73437d33fae1b2f39468b0b10e669c0c0c0f25d5`. Exit 0 in 29.460 s.
CLI banner confirms `model: codex-auto-review`, `provider: uos`. No findings were
returned. This is separate from the earlier identity-mismatched receipt, which
remains rejected for official ingestion. Exact log:
`/tmp/cppgw-asan-codex-20261117/review-fix.log`, SHA-256
`ad33b546084fc2563408ff7b5c8f36278db2a0044df503d68b3bd18ef8e1e0ad`.
Original assessment (the CLI printed it twice):

> The listener ownership cycle is correctly broken with weak captures, while active connections and pending operations continue to retain the connection state. The added regression tests cover both normal closure and failed opening.

Review did not gate or delay any deterministic check or delivery step.

## Publication and remaining ownership

The only unresolved sanitizer defect is the HTTP runtime abort/shutdown cycle
above, owned by the runtime/server shard. Fix it in `src/runtime/http.cpp`, then
rebuild and rerun `codex_serial_routing_http_test` unsuppressed. This task does not
claim a completely green sanitized shard. All assigned owned-source findings are
fixed and source-matching receipts are complete.

A fresh read-only fetch of `origin/development` returned
`002fe15fa926674c97f0169355d24929bf529710`. The implementation tip is not its
ancestor (`git merge-base --is-ancestor 09d35a1e44b761ad4fe10c8afe50bd41c1714134 origin/development`
exits 1). The initial manual push preflight failed with missing HTTPS write
authentication, as recorded above. No GitHub token was substituted, exposed,
refreshed or written. No PR/development merge or accepted development checkout
handoff is claimed. Preserve the clean owned `agent-job` branch for the enclosing
runner's promised publisher and the port owner's ancestry-preserving integration;
do not merge its pre-existing production-build work as accepted task work.

Final manual publication was attempted on documentation tip
`e099c63d075a8cc773adf96ca3e7ed4bbae2f7de` (tree
`605677143af14f5de39b907c22fdc3308e5764ba`) with:

```sh
GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false git -c credential.helper= push origin HEAD:refs/heads/test/cppgw-asan-codex-20261117
```

It exited 128: `fatal: could not read Username for 'https://github.com': terminal prompts disabled`.
The following fetch succeeded; all seven then-created task tips remain outside
`origin/development`. No local `development` branch exists. Checkout status was
clean on `agent-job`. The terminal commands and proof are retained in
`/tmp/cppgw-asan-codex-20261117/handoff.json`. This documentation follow-up adds
only that receipt and the short reproduction command below; all 986 tested
source/driver/vector inputs remain unchanged.

## Final handoff summary

Built 71 Codex/banked-reset/cluster test binaries with Clang 21.1.8, strict C++20
warnings, `-O1 -g`, then ASan/UBSan using the commands above. Plain passes all
71 binaries / 831 cases; ASan/UBSan passes 70 binaries / 830 reported cases, with
one unsuppressed outside-owned HTTP LSan failure (5,824 bytes / 45 allocations).

Fixed the supervisor socket/listener ownership cycle in `supervisor_transport.cpp`,
added two lifetime regressions that fail before the fix and pass after it, renamed
Clang-rejected test captures without changing assertions, and supplied the isolated
shard driver. Every changed path and its evidence is listed above. No UBSan or
invalid-access diagnostic remains, and no suppression was used.

The runtime/server owner must fix the reported HTTP cycle. Local work is committed;
manual GitHub publication/integration requires the enclosing authenticated runner.
Exact counts, source identities and review receipts are above. Reproduce both
profiles with the same isolated runner (from the repository root):

```sh
UOS_CODEX_CHECKS="$PWD/cpp/gateway/tests/helpers/codex_sanitizer_checks.py"
python3 "$UOS_CODEX_CHECKS" --profile both --build-only --jobs 4
sudo -n unshare --net -- /bin/sh -c 'ip link set lo up && exec setpriv --reuid=1001 --regid=1001 --clear-groups env -i PATH=/usr/local/bin:/usr/bin:/bin /usr/bin/python3 "$1" --profile both --run-only --require-clean' sh "$UOS_CODEX_CHECKS"
```
