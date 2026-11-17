# Codex module ASan/UBSan bring-up, 2026-11-17

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
| `codex_supervisor_brief_handler_differential_test` | LSan, 30,848 bytes / 456 allocations | Socket event listeners retain `ConnectionState`, which owns the socket                        | Replace the three strong event captures with weak captures; regression and sanitizer reruns pending                                         |
| `codex_supervisor_followups_test`                  | LSan, 9,317 bytes / 126 allocations  | Same supervisor transport ownership cycle                                                     | Same owned root fix; rerun pending                                                                                                          |
| `codex_supervisor_handlers_differential_test`      | LSan, 23,188 bytes / 340 allocations | Same cycle on successful close and failed socket opening                                      | Two new native lifetime regressions fail before the fix; rerun pending                                                                      |
| `codex_supervisor_inventory_coverage_test`         | LSan, 48,679 bytes / 843 allocations | Same cycle retains scripted sockets, frames, callbacks and handshake promises                 | Same owned root fix; rerun pending                                                                                                          |
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
`connect_ex(("192.0.2.1", 443)) == 101` (`ENETUNREACH`). The sanitizer build is
now running; no sanitized execution result is claimed yet. Based on the measured
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
source-matching plain and sanitized reruns are pending.

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
The incomplete-run observation describes this intentional checkpoint; the
instrumented build continues, and final sanitizer acceptance remains pending.

## Final handoff

Work is in progress. Exact build/run commands, real counts, every source change,
unfinished work, outside-owned defects, and publication status will replace this
checkpoint before stopping.
