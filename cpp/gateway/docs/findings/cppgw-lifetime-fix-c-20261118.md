# Native gateway lifetime fixes, Area C

## Contract v1

Reverify the 23 DEFECT rows in the Area C audit against the integrated gateway, then repair
confirmed lifetime defects at their existing entry points. Prioritize the Codex retry, abort
listeners and fail-fast request reads. The audit merged at `78b5276cad1ad07ae0e037cb1354cccb28352bec`
without a production source change. The assigned integrated parent is
`aae39d0ea73e7b5678dc92c437f7da6fb32f0c06`. [Area C audit](cppgw-lifetime-c-20261118.md)

| Request surface                                  | Invariants                                                                                                                                                                |
| ------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Codex, provider and paid fallback                | Keep the selected account/provider, deadline and retry behavior, HTTP statuses and error reasons. Preserve delivery receipts and one accounting settlement per admission. |
| Embeddings and KV                                | Preserve keys, values, consistency levels, fail-fast rejection and independent completion of started reads.                                                               |
| Native supervisor, Claude and provider discovery | Preserve abort and timeout behavior, shared work, request coalescing and cache generations.                                                                               |
| DeepSeek, Harmony, chat and live                 | Preserve wire and stream behavior. The audit assigns no defect rows to these modules.                                                                                     |

Write only Area C source and headers, their tests and this report. Shared runtime, other C++ areas,
TypeScript, workflows, deployment and credentials are outside this work. Tests use local mocks
behind the CI driver's OS fixture boundary, with an empty home, fresh temporary directories and
a clean environment. No provider traffic. Dependencies and build outputs live outside the checkout.

Acceptance is one building commit per defect or family with a report row, a deterministic
regression where possible, the affected link targets in release and ASan after each family, all
four affected ASan shards, one full release shard, and
`python3 cpp/gateway/tools/links.py --check`. The exact failing interleavings and achievable
test coverage are discovery questions. Stop after those gates and the report reconcile, or
checkpoint the building subset and mark outstanding work LEFT if interrupted.

Dependency preparation uses the pinned upstream Boost, SQLite and ICU sources, Clang 21 and
the Ubuntu development packages. Budget 20 minutes for dependency preparation and 90 minutes
per cold four-worker CI configuration. The logs and driver receipts are under
`/var/tmp/cppgw-lifetime-fix-c-20261118`. Check compiler and test phase transitions and diagnose
a failed component through its log before retrying. Bound individual test executions through
the driver. Use incremental builds for the defect checkpoints.

## Defect ledger

IDs include the original audit line ranges. The verification section identifies which gates ran against each commit.

| ID                                                    | Verdict and evidence                                                                                                                                                                                                | Commit                 | Test                                                                                                                                                                                 |
| ----------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `codex/dispatch.cpp:628–635`                          | FIXED — abort can destroy the injected sleeper's closure before the losing Task resumes. The default delay is an owning free coroutine. Start the same callable synchronously from the owner retained by the child. | `f014705a`             | `codex_dispatch_differential_test.cpp` resumes the sleeper after the original abort rejection.                                                                                       |
| `codex/dispatch.cpp:623–635`                          | FIXED — tearing down a suspended frame skipped removal of the listener that owned the signal. Remove the listener at scope exit, including frame destruction.                                                       | `f014705a`             | `codex_dispatch_differential_test.cpp` destroys a Loop with both branches pending.                                                                                                   |
| `codex/auth_refresh.cpp:535–548`                      | FIXED — teardown drops the shared Promise finalizer. The listener holds a weak signal, and the waiter unregisters on frame destruction so a live source cannot retain a composed signal.                            | `328b0619`, `23e449ec` | `codex_dispatch_differential_test.cpp` abandons the wait with the upstream abort source still live.                                                                                  |
| `codex/supervisor_transport.cpp:134–140`              | FIXED — the listener used to retain its own pending call connection. It now locks a weak connection, and connection destruction unregisters any remaining per-call listeners.                                       | `328b0619`, `23e449ec` | `codex_supervisor_inventory_coverage_test.cpp` verifies that the socket and composed signal are released while the abort source lives.                                               |
| `embeddings/voyage.cpp:198–207`                       | FIXED — the timer owns the listener state and its signal. The listener holds that state weakly. Destroying a pending timer unregisters even while its abort source lives.                                           | `328b0619`, `23e449ec` | `embeddings_coverage_test.cpp` verifies teardown releases the composed signal.                                                                                                       |
| `provider/claude_code.cpp:100–119`                    | FIXED — abandoning the racing frame discarded its on-settlement disposer. The frame now unregisters at scope exit, including teardown.                                                                              | `328b0619`             | `claude_code_differential_test.cpp` abandons the operation on a scoped Loop.                                                                                                         |
| `provider/metered.cpp:193–230`                        | FIXED — billing or catalog transport can be abandoned with a signal owning itself through the listener. The reaction-owned race state now owns the signal, and its destructor unregisters from live source signals. | `328b0619`             | `metered_test.cpp` abandons the operation on a scoped Loop.                                                                                                                          |
| `provider/surplus.cpp:318–354`                        | FIXED — preheader abandonment left the same listener and signal cycle. The reaction owns the race state and the listener only locks it weakly.                                                                      | `328b0619`             | `surplus_test.cpp` abandons the operation on a scoped Loop.                                                                                                                          |
| `codex/manual_reset.cpp:88,161–162,199–201`           | FIXED — the public override can transfer the last store owner into a reset. Both the initial snapshot and promotion use the now-owning readEntry.                                                                   | `df33c21e`             | `provider_kv_fanout_lifetime_test.cpp`, manual reset.                                                                                                                                |
| `codex/reset_settings.cpp:59–67,108–112`              | FIXED — both migration paths start virtual reads that can outlive a rejected sibling. Each read keeps the store and strong consistency.                                                                             | `df33c21e`             | `provider_kv_fanout_lifetime_test.cpp`, reset opt-out migration.                                                                                                                     |
| `embeddings/handlers.cpp:137–140`                     | FIXED — the buffered embeddings handler can unwind while the second cache read is still pending. Each started cache read now retains the store.                                                                     | `df33c21e`             | `provider_kv_fanout_lifetime_test.cpp`, embeddings cache lookup.                                                                                                                     |
| `embeddings/ledger.cpp:161–168`                       | FIXED — a rejected chunk can return the existing 503 before a second chunk resumes. Each chunk read retains the lease store.                                                                                        | `df33c21e`             | `provider_kv_fanout_lifetime_test.cpp`, embeddings replay.                                                                                                                           |
| `embeddings/voyage.cpp:126–136`                       | FIXED — a quota-triggered eviction may fail with another index read outstanding. The writer takes a store owner and each eviction read keeps its own copy.                                                          | `df33c21e`             | `provider_kv_fanout_lifetime_test.cpp`, embeddings cache eviction.                                                                                                                   |
| `paid_fallback/ledger_backfill.cpp:451–457`           | FIXED — a failed state read can return before either cursor get completes. The state and cursor tasks each keep the passed store.                                                                                   | `df33c21e`             | `provider_kv_fanout_lifetime_test.cpp`, paid backfill.                                                                                                                               |
| `paid_fallback/ledger_settlement.cpp:266–271,322–327` | FIXED — both batches could reject while an injected sibling still used its store. Each request, pending and settlement gate read keeps an owner.                                                                    | `df33c21e`             | `provider_kv_fanout_lifetime_test.cpp`, paid deferral and settlement.                                                                                                                |
| `provider/health.cpp:254–266`                         | FIXED — replacing the global store after a failed health read could free the old receiver before a sibling finished. Read frames now retain the old store independently.                                            | `df33c21e`             | `provider_kv_fanout_lifetime_test.cpp`, provider health.                                                                                                                             |
| `provider/health.cpp:158–172`                         | FIXED — the injected clock can reset the queue while an earlier transition is pending. Find the current tail after the clock call. The default clock does not reset the queue.                                      | `b4533584`             | `provider_selection_health_differential_test.cpp` clears the pending queue from the clock and verifies both records persist.                                                         |
| `provider/metered.cpp:684–707,712–717`                | FIXED — destroying the waiting Loop left a process-wide Promise with no producer. An identity-checked scope guard clears the slot on frame destruction.                                                             | `831e2672`             | `metered_claude_cache_test.cpp` starts a new lookup on a second Loop.                                                                                                                |
| `provider/selection.cpp:82–98,264–277`                | FIXED — Loop teardown bypassed the process-wide Promise reset. The frame guard keeps the existing load token check and leaves newer saves and reads intact.                                                         | `831e2672`             | `provider_selection_test.cpp` abandons a deferred store read and loads the replacement store on a second Loop.                                                                       |
| `provider/surplus.cpp:675–698,703–708`                | FIXED — a second Loop could join a Promise abandoned on the first. An identity-checked scope guard removes only the request from its own frame.                                                                     | `831e2672`             | `surplus_test.cpp` starts a new lookup on a second Loop.                                                                                                                             |
| `provider/anthropic_chat_responses.cpp:519`           | FIXED — bounded body reads passed an owning options aggregate in the await expression. Construct the Task before the await as PORTING.md requires.                                                                  | `83110328`             | `anthropic_chat_responses_test.cpp` covers the buffered route. The Clang sanitizer profile cannot establish the GCC 12.2 compiler hazard.                                            |
| `provider/anthropic_responses.cpp:779,857,880`        | FIXED — the two bounded reads and the native HTTP error normalizer passed owning options aggregates in the await expressions. Name each Task before awaiting it.                                                    | `83110328`             | `anthropic_responses_test.cpp` covers HTTP errors and buffered success. The Clang sanitizer profile cannot establish the GCC 12.2 compiler hazard.                                   |
| `provider/claude_code_usage_refresh.cpp:35,75`        | FIXED — both usage-lease reads supplied braced options inside the await expressions. The options contain only an optional enum. Name the options and Tasks to follow the port rule.                                 | `83110328`             | `claude_usage_retry_test.cpp` and `claude_capacity_analytics_test.cpp` cover claim and cleanup. The inline options contain only an optional enum, so this is a portability-rule fix. |

The audit checker passed on `0e8a22d4`, with 290 reviewed files, 368 SAFE rows, 23 DEFECT rows and zero UNCLEAR rows. These observations belong to the shared runtime.

- `src/kv.cpp`, `attemptOpenKv`. Replacing `selectedOpener` while an injected coroutine
  opener is suspended can destroy the closure its frame still uses. This was reported in
  the audit and is outside the assigned paths.
- `src/rt/abort.cpp`, `rt::sleep(double, AbortSignal)`. Dropping a timer on Loop teardown
  leaves its once listener installed. A surviving source can retain a composed signal and
  the callback's pending result through that listener. This follows the same ownership
  path as the Voyage case. The shared helper was inspected and has no new runtime witness here.

## Verification

The integrated base documents 450 green ARM64 test binaries. This repair uses Ubuntu 24.04 ARM64,
Clang 21.1.8, Boost 1.92.0 static JSON and Container, SQLite 3.54.0, ICU 78.3,
OpenSSL 3.0.13, zlib 1.3, Brotli 1.1.0 and Zstd 1.5.5. The saved `/var/tmp/cppgw-lifetime-fix-c-20261118/prepare.sh`
records the setup commands. Package setup took 19.917 s, Boost 95.878 s, SQLite 61.935 s and ICU
263.635 s with overlapping builds. SQLite's fixture probe reported 3.54.0 in both configurations.

### Before the fixes

The baseline builds used the preceding commit's production source and the new test files. For the
old Voyage cache-writer signature, the baseline test passed the store by reference and released its
owner immediately after the write returned. The baseline diagnostics and driver receipts are under
`/var/tmp/cppgw-lifetime-fix-c-20261118/receipts`.

| Regression               | Baseline observation                                                                                                                                                                      |
| ------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Codex retry              | The signal release assertion failed. ASan then reported heap-use-after-free when the sleeper resumed after the abort rejection.                                                           |
| Abort listeners          | Six binaries failed with LSan diagnostics. Shared refresh leaked 776 bytes, supervisor 3,680, Voyage 496, Claude 848, Metered 920 and Surplus 920.                                        |
| KV receiver lifetime     | Nine of nine cases failed their receiver-liveness assertion in the ASan profile. The weak observer keeps the combined allocation alive, so the assertion identifies the lifetime failure. |
| Health clock             | ASan reported heap-use-after-free when the code copied the erased queue entry's promise.                                                                                                  |
| Shared provider lookups  | Three of three new cases failed when the second loop ran out of work while waiting on the first loop's promise. The other 38 cases in those binaries passed.                              |
| Composed abort signals   | All three strengthened tests failed their signal-release assertions while the caller signal was still live. The other 71 cases passed.                                                    |
| Owning await expressions | Existing contract fixtures cover the corrected paths. The Clang sanitizer run does not exercise the GCC 12.2 compiler behavior.                                                           |

### Checkpoint and final commands

The staged `ci.sh` launcher uses `env -i`, an empty HOME, fresh TMPDIR, `CC=clang-21`,
`CXX=clang++-21`, and a 5,400-second command budget. It sets `UOS_CI_BUILD_DIR` to the
evidence root's `build` directory, `UOS_CI_SQLITE_PREFIX` to `sqlite`, and `PKG_CONFIG_PATH`
to `icu78/lib/pkgconfig`. The C++ flags add `-isystem` for the Boost source. The linker flags
add Boost's `stage/lib` and ICU's `lib`, `-Wl,-rpath` for ICU and `-licudata`. The driver's
300-second per-binary bound and OS fixture isolation apply. The two profiles ran on separate
detached worktrees to keep each run's source stable. Retry used four workers per invocation
and the remaining checkpoints used two per profile.

```bash
evidence=/var/tmp/cppgw-lifetime-fix-c-20261118
# Set profile to release or asan in the corresponding worktree.
# Retry checkpoint.
CPPGW_JOBS=4 bash "$evidence/ci.sh" python3 -B "$evidence/select.py" src/codex/dispatch.cpp -- --config "$profile"

# Remaining checkpoints.
mapfile -t changed < <(git diff-tree --no-commit-id --name-only -r HEAD -- cpp/gateway/src cpp/gateway/tests)
CPPGW_JOBS=2 bash "$evidence/ci.sh" python3 -B "$evidence/select.py" "${changed[@]#cpp/gateway/}" -- --config "$profile"

# Final ASan coverage, from the candidate worktree.
mapfile -t changed < <(git diff --name-only 78b5276c HEAD -- cpp/gateway/src cpp/gateway/tests)
for shard in 1 2 3 4; do
  CPPGW_JOBS=2 bash "$evidence/ci.sh" python3 -B "$evidence/select.py" "${changed[@]#cpp/gateway/}" -- --config asan --shard "$shard/4"
done
CPPGW_JOBS=2 bash "$evidence/ci.sh" bash cpp/gateway/tools/ci-build.sh --config release --shard 1/4
python3 -B cpp/gateway/tools/links.py --check
git diff --check 78b5276c HEAD
bash "$evidence/ci.sh" /home/runner/.bun/bin/bun --no-env-file --no-install \
  "$evidence/format/node_modules/prettier/bin/prettier.cjs" \
  --check cpp/gateway/docs/findings/cppgw-lifetime-fix-c-20261118.md
```

The selector resolves every test with `tools/links.py` and passes through explicit changed
tests and tests whose link sets contain any changed source. It invokes the same `driver.py`
as `ci-build.sh`. The Voyage header changed with its implementation, and the selector
includes both source call sites of the modified writer. The final ASan scope is the union
of the checkpoint scopes. Fixture subprocesses start with credential-free environments and
private network, mount and PID namespaces. The boundary proves loopback and outside-route denial.

| Checkpoint                                 | Release binaries / cases / elapsed | ASan binaries / cases / elapsed |
| ------------------------------------------ | ---------------------------------- | ------------------------------- |
| Retry `f014705a`                           | 158 / 2,185 / 3,220.934 s          | 158 / 2,185 / 1,548.973 s       |
| Abort listeners `328b0619`                 | 175 / 2,437 / 184.472 s            | 175 / 2,437 / 407.115 s         |
| KV receiver lifetime `df33c21e`            | 180 / 2,576 / 215.148 s            | 180 / 2,576 / 456.420 s         |
| Health clock `b4533584`                    | 177 / 2,496 / 63.634 s             | 177 / 2,496 / 181.269 s         |
| Shared provider lookups `831e2672`         | 172 / 2,372 / 87.581 s             | 172 / 2,372 / 251.528 s         |
| Provider await expressions `83110328`      | 136 / 1,955 / 64.380 s             | 136 / 1,955 / 194.129 s         |
| Composed abort listener cleanup `23e449ec` | 166 / 2,291 / 93.661 s             | 166 / 2,291 / 230.330 s         |

Every checkpoint passed with all 369 production objects built and zero failed cases. Each receipt
identifies its commit, tree and native input digest. The fixed release and ASan gates for a
checkpoint selected identical tests and returned identical counts.

### Final candidate

Commit `23e449ecb2371d9246b5c2afa6cd84cd0df72fcc`, tree `84e62753ca8a52ded6ff8318c8dc353898646f2a`.
Native input SHA-256 `9c3822da2d152a1d3afede02839a41c559b7bf793dd50fae22f420a52340d56c` matched in all five receipts.
The report-only follow-up commits preserve these sources, headers, tests and build-driver inputs.

| Selection   | Binaries | Cases | Object phase (s) | Test phase (s) | Elapsed (s) |
| ----------- | -------- | ----- | ---------------- | -------------- | ----------- |
| ASan 1/4    | 49       | 748   | 0.008            | 49.357         | 49.863      |
| ASan 2/4    | 49       | 734   | 0.007            | 46.487         | 46.990      |
| ASan 3/4    | 49       | 643   | 0.007            | 37.270         | 37.777      |
| ASan 4/4    | 49       | 609   | 0.007            | 39.271         | 39.776      |
| Release 1/4 | 113      | 1,182 | 234.154          | 27.842         | 262.498     |

All 196 affected sanitizer binaries passed across four shards with 2,734 cases. The other
255 binaries were outside the changed-source link scope. The release run used the complete
first shard. All final runs reported zero build failures, test failures or timeouts. Link
resolution passed for all 451 test declarations. The changed report passed Prettier and the
repair diff passed `git diff --check`.

### Branch handoff

`git fetch --no-tags origin refs/heads/development:refs/remotes/origin/development` resolved
development to `5ddac3dd154d74b1049197fb7cf5f53551609072`. The task branch descends from the
assigned C++ integration branch. Its tip is outside the fetched development history. The
command `GIT_TERMINAL_PROMPT=0 git push --dry-run origin HEAD:refs/heads/agent-jobs/cppgw-lifetime-fix-c-20261118`
failed because GitHub write credentials were unavailable. The publication, pull request and
hosted matrix are pending. The local commits remain on `agent-job` for the job owner.

## Summary

Cancelled requests could leave callbacks behind, let retries or database reads access freed state, or stall later provider lookups. All 23 Area C findings are fixed. Pending operations keep the state they need, teardown releases abandoned callbacks and lookups, and the Claude response and usage paths follow the port’s coroutine rules.

On Linux ARM64, all 196 affected sanitizer binaries and the full 113-binary release shard passed. Two shared-runtime findings remain outside this branch, and the GCC 12.2 comparison remains unverified. GitHub write credentials were unavailable, so the fixes are committed locally and the PR and hosted CI are pending.
