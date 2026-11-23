# Core module AddressSanitizer and UBSan findings — 2026-11-17

Owned fixes pass four new lifetime regressions in both profiles; all nine original
Bun body-ordering traces remain unchanged. Both full clean-commit sweeps execute
118 binaries / 1,315 cases: plain passes 116 binaries, ASan/UBSan passes 104. The
23 failed assertions are deliberate ARM64-only host refusals; twelve sanitizer
binaries additionally fail on the explicitly excluded HTTP server state cycle.
No test was weakened or skipped and no sanitizer suppression was used. Publishing
and development integration remain pending GitHub write authentication.

## Contract v1

Objective: build and run the 118 top-level module tests not claimed by the Codex,
provider, authority, or storage shards, first without sanitizers and then with
Clang AddressSanitizer and UndefinedBehaviorSanitizer. Preserve the Bun 1.4.0
behavioral oracle, assertions, warning policy, module names, coroutine ordering,
and actual transport/delivery contracts. Fix root causes only in owned paths.

Owned production paths: admin, auth (except passkeys.cpp), handler, harmony,
jev_compaction, lib, live, runtime (except http.cpp), systemone, decisions, and
unclaimed top-level sources, with matching headers. Owned tests are the selected
top-level tests and their vectors plus the explicitly assigned helpers. Other
sources, runtime primitives, build tools, CI, TypeScript, credentials, deployment,
production state and paid inference are outside this task. No test weakening,
unobserved oracle changes, or undocumented suppressions are permitted.

Behavior matrix: Linux x86_64 / Clang 21.1.8 / libstdc++ 15, plain `-O1 -g`
followed by `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
-fno-sanitize-recover=undefined`. Every binary links all 44 runtime objects and
its exact resolved Links closure. Compile each distinct source once per profile.
Use the task's four shard-prefix exclusion lists verbatim; nested runtime tests
belong to the earlier runtime shard and are not counted again.

Acceptance surface: 118 standalone binaries in each configuration, strict C++20
warning-fatal compilation, affected regressions in both profiles, changed-document
formatting and `git diff --check`. Each test runs in a new HOME/TMPDIR/cwd with
an environment allowlist (no inherited credentials), in its own process group,
with a 600-second timeout. Sanitizer runs enable leaks, stack-use-after-return,
strict string checks and UBSan stack traces. All build output and logs stay under
`/tmp/cppgw-asan-core-20261117`, outside the checkout.

Unknowns to resolve from evidence: host dependencies, Links closure completeness,
host-specific fixture assumptions, and sanitizer failures. Stop when both complete
sweeps have terminal counts and every failure is fixed or documented at its
ownership boundary. Publication must be attempted even if work is incomplete;
do not integrate pre-existing unfinished port work as accepted task work.

Long-operation budget: four compiler workers; expect 10–30 minutes per cold
profile, preserve incremental objects and per-operation logs, report progress at
least once per minute. Per-test runtime budget is 600 seconds. On compilation or
runtime failure, retain exact commands and first diagnostics, diagnose before
retrying, then rebuild affected objects. Do not repeat unchanged failed commands.

## Discovery

- Starting commit: `141cb4d4`, branch `agent-job`, clean checkout.
- Host: disposable Ubuntu x86_64 runner, four CPUs, approximately 15 GiB RAM.
- Compiler: Ubuntu Clang 21.1.8; Bun is the pinned 1.4.0.
- Read PORTING.md, PORT-NOTES.md, TESTING.md, BUILD.md, async lifetime rules and
  both prior sanitizer/runtime and libc++ portability reports.
- Initial inventory: 118 selected top-level test files and 44 runtime sources.
- Links resolution succeeds for all 118 tests, with no registry skips or extra
  environment assignments. The union contains 460 distinct translation units.
- Installed Boost.JSON/Container 1.90 and Brotli development packages. Existing
  OpenSSL, zlib, ICU and Zstd packages satisfy the build. Prepared SQLite 3.54.0
  from the same hash-verified official amalgamation as the prior runtime job.
- Added `tests/helpers/core_sanitizers.py`, using per-source objects and one
  ephemeral linked binary at a time. Acceptance executions use private Linux
  network namespaces, prove external-route denial, then drop to the runner UID.
  Dirty-tree discovery receipts deliberately have null accepted commit/tree IDs;
  `--require-clean` binds final acceptance to the committed tree.
- GitHub publication preflight fails: `git push --dry-run` exits 128 because it
  cannot read a username; `gh auth status` confirms no authenticated host.
  No credential was printed, written, refreshed or replaced.

## Findings

Owned lifetime findings discovered during the sanitizer sweep:

- `RawClient` in `tests/helpers/loopback_http.*` retained callbacks capturing itself
  after close. The live-sideband and serve-delivery leak reports include its
  65,536-byte buffers. Release listeners after synchronous close; move the pending
  destroy event's listener map into the posted event and retain only a weak link
  from the client. Registrations between destroy and close retain their order.
  Three new weak-ownership regressions pass plain and ASan/UBSan; the original
  helper rebuilt against all three fails every ownership assertion (0/3 pass).
- `src/runtime/http_body.cpp` retained an unconsumed stream through
  `state -> controller -> stream algorithms -> state`. The standalone abandonment
  regression fails before the fix even in plain mode (0/1 pass). Keep the startup
  controller through synchronous initialization, then borrow the stream state
  weakly and acquire a controller only during an event. Original nine Bun-recorded
  ordering cases plus the new ownership case pass 10/10 in both profiles, with
  no leak or undefined-behavior report.
- `src/runtime/http.cpp` is explicitly excluded. Its `ServeState::abort` owns a
  shutdown callback capturing `state` strongly even without an external abort
  signal; shutdown never clears that self-cycle. Every HTTP leak binary includes
  the allocation at line 1914 and the abort-listener allocation at line 2002.
  This is a real outside-owned defect, not a documented GC-model suppression.

| Test                                                                               | Tool                             | Cause                                                                                                                          | Fix or disposition                                                                                                                                                             |
| ---------------------------------------------------------------------------------- | -------------------------------- | ------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Plain baseline                                                                     | Clang 21.1.8                     | All 460 translation units compile; two platform-specific fixtures reject x64                                                   | 116/118 binaries pass; 1,311 cases run, 1,288 pass, 23 fail; no skips or timeouts                                                                                              |
| Native permission/bootstrap fixtures                                               | Bun 1.4.0 oracle                 | These APIs intentionally support ARM64 only                                                                                    | Linux/x64 returns `Linux confinement requires Linux ARM64` and `Native KV development platform is unsupported`; retain the fail-closed behavior                                |
| `permissions_linux_test`                                                           | Plain assertions, 5/5 failed     | The five enforcement/artifact fixtures assume a supported ARM64 process; the implementation rejects x64 before artifact access | Bun reproduces the same rejection. Preserve every original assertion and real OS enforcement; report the platform limit instead of spoofing architecture or skipping the cases |
| `runtime_native_startup_test`                                                      | Plain assertions, 18/18 failed   | Fixture setup calls `nativeKvDevelopmentPaths`, which intentionally rejects x64 before startup receipt validation              | Bun reproduces `Native KV development platform is unsupported`. Keep all original supported-host expectations and report these cases as failed, not skipped or passed          |
| `live_sideband_http_test`, `serve_delivery_http_test`, three new `RawClient` cases | LSan / weak ownership assertions | Closed raw clients retain self-capturing listeners, including when the pending close event is discarded                        | Fix the owned loopback helper; old implementation fails all 3 regressions, fixed code passes 3/3 in plain and ASan/UBSan                                                       |
| `runtime_http_differential_test`, new `runtime_http_body_test` abandonment case    | LSan / weak ownership assertion  | Unconsumed stream algorithms and their stored controller retain one another                                                    | Fix the owned body state with a weak stream reference after synchronous startup; before 0/1, after 10/10 full body cases in both profiles                                      |
| Twelve HTTP integration binaries listed below                                      | LSan                             | An unconditional `ServeState::abort` callback closes over its owning state via `shutdown`                                      | `src/runtime/http.cpp` is explicitly excluded. Report the allocation/cycle and reproduce without suppression; no edit to the excluded file                                     |

## Initial unsuppressed sanitizer findings

All 460 instrumented compilations and all 118 links succeeded. The first sweep
passed 104 binaries; 12 exited on LeakSanitizer and two on the supported-host
guards. No ASan access violation or UBSan diagnostic occurred. The initial
prototype's stdio buffering lost the case-summary lines of the 12 LSan exits:
only 1,148 summarized cases (1,125 passed, 23 failed) are known for that run, not
a complete shard case total. The helper now uses system `stdbuf -oL` so final
receipts retain every case result before LeakSanitizer terminates the process.

The first sanitizer run also predates the helper's symbolizer-path correction;
its input-matching flag is false. It is discovery evidence only and is
superseded by the complete clean-commit sweep below. All sanitizer logs below are retained
under `/tmp/cppgw-asan-core-20261117/asan/runs/1794915791048038785`.

| Test                                 | Sanitizer | Initial bytes / allocations | Cause and disposition                                                  |
| ------------------------------------ | --------- | --------------------------- | ---------------------------------------------------------------------- |
| `chart_source_proxy_test`            | LSan      | 2,657 / 27                  | Excluded HTTP server state cycle; report                               |
| `failover_stress_http_test`          | LSan      | 21,632 / 90                 | Excluded HTTP server state cycle and retained request tasks; report    |
| `jev_compaction_fallback_quota_test` | LSan      | 9,552 / 88                  | Excluded HTTP server state cycle; report                               |
| `live_calls_http_test`               | LSan      | 69,568 / 614                | Excluded HTTP server state cycle; report                               |
| `live_sideband_http_test`            | LSan      | 182,565 / 1,120             | Fix owned RawClient retention; separately report excluded HTTP cycle   |
| `passkeys_part1_test`                | LSan      | 2,923 / 26                  | Excluded HTTP server state cycle; no edit to excluded passkeys.cpp     |
| `runtime_early_sse_test`             | LSan      | 69,068 / 134                | Excluded HTTP state retains handler/input/stream graphs; report        |
| `runtime_http_differential_test`     | LSan      | 76,401 / 82                 | Fix owned unconsumed body cycle; separately report excluded HTTP cycle |
| `runtime_http_sse_headers_test`      | LSan      | 5,440 / 53                  | Excluded HTTP server state cycle; report                               |
| `runtime_http_test`                  | LSan      | 39,541 / 377                | Excluded HTTP server state cycle and pending request graphs; report    |
| `serve_delivery_http_test`           | LSan      | 69,752 / 41                 | Fix owned RawClient retention; separately report excluded HTTP cycle   |
| `systemone_test`                     | LSan      | 2,720 / 28                  | Excluded HTTP server state cycle; report                               |

Every one reports the exact initial error `ERROR: LeakSanitizer: detected memory leaks`.
For example, chart-source's first allocation frames include:

```text
Indirect leak of 608 byte(s) in 1 object(s) allocated from:
    #0 ... operator new(unsigned long)
    #9 ... uos::runtime::http::serveHttp(...) src/runtime/http.cpp:1914:18
    #10 ... uosAsyncTestBody28() tests/chart_source_proxy_test.cpp:172:111
SUMMARY: AddressSanitizer: 2657 byte(s) leaked in 27 allocation(s).
```

The excluded root is at `src/runtime/http.cpp:2002`: the state owns its abort
listener, whose closure owns `shutdown`, whose closure owns the state. The listener
is created even when there is no supplied abort signal. Normal server close drops
server listeners, but the finished callback only unregisters the abort listener
from a supplied signal and never breaks the state's own reference. Descendant
allocations alone do not imply a separate defect in their defining modules.
No suppression covers this bug. Reproduce any row, preserving all real cases:

```sh
python3 cpp/gateway/tests/helpers/core_sanitizers.py asan --filter chart_source_proxy_test
```

Replace the filter with the table's binary name for its original failing command.
The excluded source and its matching header remain untouched.

## Source changes and selected verification

| Changed path under `cpp/gateway/`                                    | Reason and evidence                                                                                                                                                                           |
| -------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/runtime/http_body.cpp`                                          | Break the unconsumed-body controller cycle without adding an await or changing EOF/abort/cancel behavior; all nine original Bun event traces and new ownership case pass in both profiles     |
| `tests/helpers/loopback_http.cpp`, `tests/helpers/loopback_http.hpp` | Release terminal listener captures and let an abandoned posted close own its captures outside the client; 3/3 lifetime regressions pass in both profiles and fail 0/3 against original source |
| `tests/runtime_http_test.cpp`                                        | Add peer-close, explicit-destroy/late-registration, and discarded-close regressions using independent weak ownership assertions; retain all 14 original cases                                 |
| `tests/runtime_http_body_test.cpp`                                   | Add an unconsumed-body abandonment regression; retain all nine original recorded-value/order cases                                                                                            |
| `tests/helpers/core_sanitizers.py`                                   | Reproducible 118-binary selection, exact Links objects, isolated environments/network/process groups, timeouts, source/identity receipts, symbolized reports and line-buffered case summaries |
| `docs/findings/cppgw-asan-core-20261117.md`                          | Contract, dependencies, exact commands, original failures, oracle evidence, source changes and final handoff limits                                                                           |

The changed C++ implementation/helper and their two regression files map to strict
warning-fatal compilation, the three RawClient regressions, the ten body cases,
and the complete shard in both profiles after source freeze. Python syntax,
changed-document Prettier, the size ratchet and `git diff --check` cover the helper
and report. No TypeScript source/import, Rust, CI or deployment gate changed.

Focused results:

```sh
python3 cpp/gateway/tests/helpers/core_sanitizers.py plain --filter runtime_http_test --case RawClient
python3 cpp/gateway/tests/helpers/core_sanitizers.py asan --filter runtime_http_test --case RawClient
python3 cpp/gateway/tests/helpers/core_sanitizers.py plain --filter runtime_http_body_test
python3 cpp/gateway/tests/helpers/core_sanitizers.py asan --filter runtime_http_body_test
```

Results: 3/3, 3/3, 10/10 and 10/10 respectively; zero sanitizer reports.
The original-helper negative control is retained at
`/tmp/cppgw-asan-core-20261117/negative-control.py` and `negative-control/log`:
`git show 141cb4d4:cpp/gateway/tests/helpers/loopback_http.{hpp,cpp}` supplies the
original helper/header; compile those and the current regression test with the
documented plain flags, then use the existing Ninja link command with only those
two object paths replaced. Running the `RawClient` cases returns exit 1,
`3 run, 0 passed, 3 failed`. The standalone body regression likewise returned
`1 run, 0 passed, 1 failed` before the owned source fix.

Bun 1.4.0 `node:net` confirms deferred-close registration order: create `new Socket()`,
register a once-close listener, call `destroy()`, register another close listener,
await close, and record the listeners. The isolated oracle returned
`{"bun":"1.4.0","events":["original","late"]}`. No TS expected value was changed.

Exact close-order oracle (the new socket never connects to a provider):

```sh
python3 - <<'ORACLE'
import shutil, subprocess, tempfile
program = 'import {Socket} from "node:net";\nconst events=[]; const socket=new Socket();\nconst closed=new Promise(resolve=>socket.once("close",()=>{events.push("original");resolve();}));\nsocket.destroy(); socket.on("close",()=>events.push("late")); await closed;\nconsole.log(JSON.stringify({bun:Bun.version,events}));'
with tempfile.TemporaryDirectory(prefix='co-order-') as scratch:
    subprocess.run([shutil.which('bun'), '--no-env-file', '--no-install', '-e', program],
                   cwd=scratch, env={'PATH':'/usr/bin:/bin','HOME':scratch,'TMPDIR':scratch}, check=True)
ORACLE
```

## Commands and terminal results

Run from the repository root. Dependency preparation (all generated files outside
the checkout):

```sh
sudo -n apt-get update -qq
sudo -n apt-get install -y -qq libboost-json-dev libboost-container-dev libbrotli-dev
sudo -n apt-get install -y -qq lld-21
mkdir -p /tmp/cppgw-asan-core-20261117
curl --fail --silent --show-error --location \
  https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip \
  -o /tmp/cppgw-asan-core-20261117/sqlite.zip
python3 - <<'PY'
from pathlib import Path
import hashlib, zipfile
archive = Path('/tmp/cppgw-asan-core-20261117/sqlite.zip')
assert hashlib.sha256(archive.read_bytes()).hexdigest() == '68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d'
with zipfile.ZipFile(archive) as source:
    source.extractall(archive.parent)
PY
clang -O2 -fPIC -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_COLUMN_METADATA=1 \
  -DSQLITE_DQS=3 -shared -Wl,-soname,libsqlite3.so \
  -o /tmp/cppgw-asan-core-20261117/libsqlite3.so \
  /tmp/cppgw-asan-core-20261117/sqlite-amalgamation-3540000/sqlite3.c -ldl -pthread -lm
```

The initial discovery run uses an external prototype of the committed helper:

```sh
python3 /tmp/cppgw-asan-core-20261117/check.py plain
```

The reproducible helper generates its CMake driver under `/tmp`, records every
expanded compilation in `compile_commands.json`, builds all distinct sources once
per profile and links each binary's original ordered Links list plus every runtime
object. It retains invocation-specific logs/results/provenance under
`<build-root>/<profile>/runs/<id>`, and deletes each binary after execution.
The initial prototype uses the default GNU linker; the checked-in helper uses
`-fuse-ld=/usr/bin/ld.lld-21` (Ubuntu LLD 21.1.8) to avoid repeated slow full-gateway
links. Compilation flags and original Links lists are unchanged.
Acceptance commands, after committing all source changes:

```sh
python3 cpp/gateway/tests/helpers/core_sanitizers.py plain --require-clean
python3 cpp/gateway/tests/helpers/core_sanitizers.py asan --require-clean
```

Both profiles use `-std=c++20 -Wall -Wextra -Wshadow -Wconversion -Werror
-DBOOST_JSON_NO_LIB -DBOOST_CONTAINER_NO_LIB -DBOOST_SYSTEM_NO_LIB
-DUOS_GATEWAY_NATIVE_LIBRARIES -pthread -O1 -g`, the two gateway include directories,
Boost.JSON, Boost.Container, OpenSSL ssl/crypto, pthread, dl, the explicit SQLite
library, zlib, Brotli decoder, Zstd and ICU uc. ASan additionally uses
`-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined`.
Execution sets `ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1`
and `UBSAN_OPTIONS=print_stacktrace=1`. No suppression currently exists or is used.

Initial plain terminal receipt:
`/tmp/cppgw-asan-core-20261117/plain/runs/1794914047262551911`.
All 460 compilations succeeded. All 118 binaries linked and executed: 116 passed,
two failed; 1,311 cases ran, 1,288 passed and 23 failed. The consumed source hashes
stayed unchanged. This is dirty-tree discovery evidence, not a committed-source
acceptance claim. Every failed case belongs to the two unsupported-host fixtures
described above. This initial checkpoint preceded sanitizer execution; the final
source-matching sanitizer results are recorded below.

## Platform oracle and retained coverage limits

This observation ran the actual TypeScript modules, not a reimplementation, under
Bun 1.4.0 with no dotenv, auto-install, inherited provider variables or real home:

```sh
python3 - <<'PY'
from pathlib import Path
import json, shutil, subprocess, tempfile
root = Path.cwd()
modules = [root / 'src/runtime/permissions-linux.ts', root / 'src/runtime/native-kv-bootstrap.ts']
program = '''
const p = await import(MODULE0);
const b = await import(MODULE1);
for (const [name, call] of [
  ["default support", () => p.getLinuxPermissionsSupport()],
  ["explicit support", () => p.getLinuxPermissionsSupport({path:"/missing-artifact",sha256:"0".repeat(64)})],
  ["enforce", () => p.applyLinuxPermissions({read:[],write:[]},{path:"/missing-artifact",sha256:"0".repeat(64)})],
  ["development paths", () => b.nativeKvDevelopmentPaths()]
]) {
  try { console.log(name, JSON.stringify(call())); }
  catch (error) { console.log(name, error.message); }
}
console.log(JSON.stringify({bun:Bun.version,platform:process.platform,arch:process.arch}));
'''
for index, module in enumerate(modules):
    program = program.replace('MODULE' + str(index), json.dumps(str(module)))
with tempfile.TemporaryDirectory(prefix='co-oracle-') as scratch:
    scratch = Path(scratch)
    for name in ['h', 't', 'w']:
        (scratch / name).mkdir()
    subprocess.run([shutil.which('bun'), '--no-env-file', '--no-install', '-e', program],
                   cwd=scratch / 'w', env={'PATH':'/usr/bin:/bin', 'HOME':str(scratch / 'h'),
                   'TMPDIR':str(scratch / 't')}, check=True)
PY
```

Observed output:

```text
default support Linux confinement requires Linux ARM64
explicit support Linux confinement requires Linux ARM64
enforce Linux confinement requires Linux ARM64
development paths Native KV development platform is unsupported
{"bun":"1.4.0","platform":"linux","arch":"x64"}
```

Original C++ diagnostics:

```text
permissions_linux_test:
    uncaught exception: Linux confinement requires Linux ARM64
5 run, 0 passed, 5 failed
runtime_native_startup_test:
    uncaught exception: Native KV development platform is unsupported
18 run, 0 passed, 18 failed
```

Both C++ modules match their TypeScript architecture guards. A fully green plain
shard cannot be claimed on this x64 host while preserving those guards and every
supported-ARM64 fixture assertion. All tests still run and fail normally; none is
deleted, disabled, weakened, architecture-spoofed or counted as skipped. Actual
ARM64 native startup and Landlock/seccomp enforcement require their supported host.
The sanitizer run continues over every binary so this host limit does not hide
other defects.

## Source freeze and remaining acceptance

All owned C++ writers have stopped after the two ownership fixes. The complete
plain and ASan/UBSan commands above ran once on the clean implementation commit.
Both terminal receipts, every residual diagnostic, full commit/tree IDs and input
hashes are recorded below. A documentation-only follow-up may append
the final receipts without changing tested C++ or helper inputs.

Local review uses actual `codex review --commit <implementation-sha>` with
`review_model="codex-auto-review"`, a 180-second operation budget and a retained
transcript. Verify the announced model identity before accepting any official
review evidence. Review latency/findings are not a gate; deterministic evidence,
the ownership boundary and explicit platform failures remain the acceptance facts.
No hosted workflow or production deployment is authorized or triggered.

## Exact committed-source terminal receipts

Implementation commit: `f4d548f204dd253b830e5c96e37ac6624b7fbd27`.
Tree: `b27febb68ca46ff733831469706c4eb2b7b63317`.
Parent/base: `141cb4d482a3b0d8624ce0411cbd3b93e45873e6` (the pre-existing port and production-build work are retained, not accepted as task work).

Both final sweeps ran all 118 binaries and all 1,315 cases. Each receipt has
`worktree_clean`, `inputs_unchanged` and `identity_unchanged` set to true.
All 849 consumed first-party input hashes match between profiles. The sorted
`<sha256>  <gateway-relative-path>` manifest SHA-256 is
`40e363372ef4d75cb3eb81ee6576959da1cc9e14f5d41ba65b2f0262572b74a2`.

| Profile        | Compilation/link       | Binaries passed / failed | Cases run / passed / failed | Disposition                                                                     |
| -------------- | ---------------------- | ------------------------ | --------------------------- | ------------------------------------------------------------------------------- |
| Plain `-O1 -g` | 460 objects, 118 links | 116 / 2                  | 1,315 / 1,292 / 23          | All 23 failed cases are the two ARM64-only fixtures                             |
| ASan/UBSan     | 460 objects, 118 links | 104 / 14                 | 1,315 / 1,292 / 23          | Same platform failures plus 12 LSan process exits after passing case assertions |

No binary was skipped, failed to build, timed out, or lacked its final case summary.
No ASan access violation or UBSan report occurred. No suppression was added or used.
Final builds reuse the already compiled, dependency-checked objects; these are
complete execution sweeps on the final committed source, not additional cold builds.

Exact commands (each exits 1 because its reported failures remain):

```sh
python3 cpp/gateway/tests/helpers/core_sanitizers.py plain --require-clean
python3 cpp/gateway/tests/helpers/core_sanitizers.py asan --require-clean
```

- `plain`: `/tmp/cppgw-asan-core-20261117/plain/runs/1794918179251074621`.
  `results.json` SHA-256: `504be5ff9ab69142de23fc843470f5a7cd787fdbfcbb073e3b40ce1d38e2a979`.
  `provenance.json` SHA-256: `0684e7101f6790ee416ef0c9bc571afb1d0ddf1539823c7f5049b287fd80778c`.
- `asan`: `/tmp/cppgw-asan-core-20261117/asan/runs/1794918179175283813`.
  `results.json` SHA-256: `a2df3501236713e572a4ada61977dd06ea1b08b3f66e247d8ee269df117ab8e9`.
  `provenance.json` SHA-256: `62d419543094908f116895f32dd08b6804c0298932d88edeb03cedaaca5d2e6d`.

### Final residual leak evidence

The owned cycles are fixed. RawClient allocation frames are absent from all final
leak reports. The standalone incoming-body suite is sanitizer-clean, and the full
HTTP differential leak drops by 70,473 bytes. The following remaining reports
include the excluded HTTP `ServeState`/abort/shutdown cycle; each is an unsuppressed
failed binary requiring the `src/runtime/http.cpp` owner.

| Binary                               | Final leaked bytes | Final allocations |
| ------------------------------------ | -----------------: | ----------------: |
| `chart_source_proxy_test`            |              2,657 |                27 |
| `failover_stress_http_test`          |             21,632 |                90 |
| `jev_compaction_fallback_quota_test` |              9,552 |                88 |
| `live_calls_http_test`               |             69,568 |               614 |
| `live_sideband_http_test`            |            115,000 |             1,097 |
| `passkeys_part1_test`                |              2,923 |                26 |
| `runtime_early_sse_test`             |             69,068 |               134 |
| `runtime_http_differential_test`     |              5,928 |                46 |
| `runtime_http_sse_headers_test`      |              5,440 |                53 |
| `runtime_http_test`                  |             39,541 |               377 |
| `serve_delivery_http_test`           |              2,824 |                28 |
| `systemone_test`                     |              2,720 |                28 |

### Final per-binary counts

Case counts are identical in both profiles; LSan means the case assertions passed
but LeakSanitizer made the binary fail. FAIL denotes the ARM64-only case failures.

| Binary                                       | Run | Passed | Failed | Plain | ASan/UBSan |
| -------------------------------------------- | --: | -----: | -----: | ----- | ---------- |
| `admin_api_key_audit_test`                   |  13 |     13 |      0 | PASS  | PASS       |
| `admin_api_keys_coverage_test`               |  22 |     22 |      0 | PASS  | PASS       |
| `admin_auth_part1_test`                      |  27 |     27 |      0 | PASS  | PASS       |
| `admin_auth_part2_test`                      |   5 |      5 |      0 | PASS  | PASS       |
| `admin_capacity_status_test`                 |   1 |      1 |      0 | PASS  | PASS       |
| `admin_codex_coverage_test`                  |  33 |     33 |      0 | PASS  | PASS       |
| `admin_codex_reset_test`                     |   4 |      4 |      0 | PASS  | PASS       |
| `admin_codex_scope_experiment_test`          |   6 |      6 |      0 | PASS  | PASS       |
| `admin_defaults_differential_test`           |  21 |     21 |      0 | PASS  | PASS       |
| `admin_error_log_test`                       |   5 |      5 |      0 | PASS  | PASS       |
| `admin_index_harness_test`                   |  10 |     10 |      0 | PASS  | PASS       |
| `admin_kernel_coverage_test`                 |  24 |     24 |      0 | PASS  | PASS       |
| `admin_models_catalog_test`                  |  33 |     33 |      0 | PASS  | PASS       |
| `admin_namespace_include_order_test`         |   1 |      1 |      0 | PASS  | PASS       |
| `admin_native_owner_test`                    |   4 |      4 |      0 | PASS  | PASS       |
| `agent_messages_test`                        |   4 |      4 |      0 | PASS  | PASS       |
| `analytics_differential_test`                |   4 |      4 |      0 | PASS  | PASS       |
| `auth_relay_differential_test`               |   1 |      1 |      0 | PASS  | PASS       |
| `auth_relay_test`                            |   3 |      3 |      0 | PASS  | PASS       |
| `auth_responses_branch_coverage_test`        |  23 |     23 |      0 | PASS  | PASS       |
| `bounded_response_body_test`                 |   6 |      6 |      0 | PASS  | PASS       |
| `c1_provider_handlers_differential_test`     |  41 |     41 |      0 | PASS  | PASS       |
| `chart_source_proxy_test`                    |  22 |     22 |      0 | PASS  | LSan       |
| `compaction_summary_forwarding_test`         |  17 |     17 |      0 | PASS  | PASS       |
| `config_differential_test`                   |   3 |      3 |      0 | PASS  | PASS       |
| `debug_routing_differential_test`            |   3 |      3 |      0 | PASS  | PASS       |
| `decisions_test`                             |  95 |     95 |      0 | PASS  | PASS       |
| `defaults_test`                              |   2 |      2 |      0 | PASS  | PASS       |
| `failover_stress_http_test`                  |   2 |      2 |      0 | PASS  | LSan       |
| `forwarded_payload_unicode_test`             |  12 |     12 |      0 | PASS  | PASS       |
| `handler_admission_differential_test`        |   3 |      3 |      0 | PASS  | PASS       |
| `handler_http_differential_test`             |   6 |      6 |      0 | PASS  | PASS       |
| `handler_replay_background_test`             |   4 |      4 |      0 | PASS  | PASS       |
| `handler_terminal_admission_stage_test`      |   4 |      4 |      0 | PASS  | PASS       |
| `handler_terminal_usage_rollup_test`         |   5 |      5 |      0 | PASS  | PASS       |
| `harmony_adapter_test`                       |  12 |     12 |      0 | PASS  | PASS       |
| `harmony_classifier_test`                    |   8 |      8 |      0 | PASS  | PASS       |
| `harmony_conversation_test`                  |   7 |      7 |      0 | PASS  | PASS       |
| `harmony_differential_test`                  |  10 |     10 |      0 | PASS  | PASS       |
| `harmony_parse_test`                         |  11 |     11 |      0 | PASS  | PASS       |
| `harmony_probes_coverage_test`               |  24 |     24 |      0 | PASS  | PASS       |
| `harmony_probes_test`                        |  12 |     12 |      0 | PASS  | PASS       |
| `harmony_render_test`                        |   9 |      9 |      0 | PASS  | PASS       |
| `harmony_tools_backend_differential_test`    |   4 |      4 |      0 | PASS  | PASS       |
| `harmony_tools_fakes_differential_test`      |   4 |      4 |      0 | PASS  | PASS       |
| `harmony_tools_result_differential_test`     |   4 |      4 |      0 | PASS  | PASS       |
| `harmony_tools_router_test`                  |  21 |     21 |      0 | PASS  | PASS       |
| `harmony_tools_schemas_test`                 |   8 |      8 |      0 | PASS  | PASS       |
| `health_test`                                |  22 |     22 |      0 | PASS  | PASS       |
| `http_differential_test`                     |   5 |      5 |      0 | PASS  | PASS       |
| `images_test`                                |  37 |     37 |      0 | PASS  | PASS       |
| `inference_deadline_differential_test`       |   6 |      6 |      0 | PASS  | PASS       |
| `inference_deadline_test`                    |   5 |      5 |      0 | PASS  | PASS       |
| `jev_codex_smoke_test`                       |   6 |      6 |      0 | PASS  | PASS       |
| `jev_compaction_differential_test`           |  12 |     12 |      0 | PASS  | PASS       |
| `jev_compaction_fallback_quota_test`         |   7 |      7 |      0 | PASS  | LSan       |
| `jev_compaction_smoke_test`                  |   6 |      6 |      0 | PASS  | PASS       |
| `jev_compaction_test`                        |  23 |     23 |      0 | PASS  | PASS       |
| `jev_hybrid_test`                            |  11 |     11 |      0 | PASS  | PASS       |
| `key_compatibility_test`                     |   1 |      1 |      0 | PASS  | PASS       |
| `live_calls_http_test`                       |  11 |     11 |      0 | PASS  | LSan       |
| `live_handler_differential_test`             |  24 |     24 |      0 | PASS  | PASS       |
| `live_routes_differential_test`              |   1 |      1 |      0 | PASS  | PASS       |
| `live_sideband_http_test`                    |  21 |     21 |      0 | PASS  | LSan       |
| `local_admin_auth_test`                      |  17 |     17 |      0 | PASS  | PASS       |
| `local_development_key_test`                 |  17 |     17 |      0 | PASS  | PASS       |
| `model_metadata_test`                        |  17 |     17 |      0 | PASS  | PASS       |
| `models_metadata_differential_test`          |   4 |      4 |      0 | PASS  | PASS       |
| `openapi_runtime_contract_test`              |  11 |     11 |      0 | PASS  | PASS       |
| `optional_telemetry_queue_differential_test` |   6 |      6 |      0 | PASS  | PASS       |
| `passkeys_part1_test`                        |  40 |     40 |      0 | PASS  | LSan       |
| `passkeys_part2_test`                        |   6 |      6 |      0 | PASS  | PASS       |
| `passkeys_test`                              |   5 |      5 |      0 | PASS  | PASS       |
| `passkeys_webauthn_differential_test`        |   1 |      1 |      0 | PASS  | PASS       |
| `passport_differential_test`                 |   1 |      1 |      0 | PASS  | PASS       |
| `passport_sessions_test`                     |  12 |     12 |      0 | PASS  | PASS       |
| `passport_test`                              |   7 |      7 |      0 | PASS  | PASS       |
| `permissions_linux_test`                     |   5 |      0 |      5 | FAIL  | FAIL       |
| `public_provider_cache_controls_test`        |   4 |      4 |      0 | PASS  | PASS       |
| `recent_model_context_differential_test`     |   3 |      3 |      0 | PASS  | PASS       |
| `recent_model_context_test`                  |   3 |      3 |      0 | PASS  | PASS       |
| `reliability_context_test`                   |   8 |      8 |      0 | PASS  | PASS       |
| `reliability_differential_test`              |   6 |      6 |      0 | PASS  | PASS       |
| `reliability_failure_differential_test`      |   3 |      3 |      0 | PASS  | PASS       |
| `reliability_feedback_test`                  |   8 |      8 |      0 | PASS  | PASS       |
| `reliability_harness_test`                   |  45 |     45 |      0 | PASS  | PASS       |
| `reliability_hash_test`                      |   2 |      2 |      0 | PASS  | PASS       |
| `reliability_loops_test`                     |  10 |     10 |      0 | PASS  | PASS       |
| `reliability_retry_differential_test`        |   4 |      4 |      0 | PASS  | PASS       |
| `reliability_retry_test`                     |   6 |      6 |      0 | PASS  | PASS       |
| `reliability_state_test`                     |   7 |      7 |      0 | PASS  | PASS       |
| `reliability_verify_test`                    |  12 |     12 |      0 | PASS  | PASS       |
| `request_delivery_lifecycle_test`            |   1 |      1 |      0 | PASS  | PASS       |
| `request_differential_test`                  |  12 |     12 |      0 | PASS  | PASS       |
| `request_policy_differential_test`           |   9 |      9 |      0 | PASS  | PASS       |
| `runtime_config_differential_test`           |   8 |      8 |      0 | PASS  | PASS       |
| `runtime_config_test`                        |   4 |      4 |      0 | PASS  | PASS       |
| `runtime_early_sse_test`                     |   4 |      4 |      0 | PASS  | LSan       |
| `runtime_http_body_test`                     |  10 |     10 |      0 | PASS  | PASS       |
| `runtime_http_differential_test`             |   5 |      5 |      0 | PASS  | LSan       |
| `runtime_http_early_sse_differential_test`   |   3 |      3 |      0 | PASS  | PASS       |
| `runtime_http_sse_headers_test`              |   2 |      2 |      0 | PASS  | LSan       |
| `runtime_http_tasks_test`                    |   1 |      1 |      0 | PASS  | PASS       |
| `runtime_http_test`                          |  17 |     17 |      0 | PASS  | LSan       |
| `runtime_matrix_compaction_header_test`      |   4 |      4 |      0 | PASS  | PASS       |
| `runtime_matrix_legacy_replay_test`          |   7 |      7 |      0 | PASS  | PASS       |
| `runtime_native_startup_test`                |  18 |      0 |     18 | FAIL  | FAIL       |
| `runtime_websocket_test`                     |   2 |      2 |      0 | PASS  | PASS       |
| `serve_delivery_http_test`                   |   7 |      7 |      0 | PASS  | LSan       |
| `serve_test`                                 |   5 |      5 |      0 | PASS  | PASS       |
| `smoke_test`                                 |   2 |      2 |      0 | PASS  | PASS       |
| `static_assets_test`                         |  38 |     38 |      0 | PASS  | PASS       |
| `static_differential_test`                   |   1 |      1 |      0 | PASS  | PASS       |
| `supervisor_log_test`                        |   3 |      3 |      0 | PASS  | PASS       |
| `systemone_test`                             |  28 |     28 |      0 | PASS  | LSan       |
| `transport_normalization_coverage_test`      |  36 |     36 |      0 | PASS  | PASS       |
| `types_differential_test`                    |  10 |     10 |      0 | PASS  | PASS       |
| `utils_test`                                 |   7 |      7 |      0 | PASS  | PASS       |

### Toolchain and dependency identity

Clang, LLD and llvm-symbolizer are Ubuntu 21.1.8; CMake is 4.4.5 and Ninja 1.13.2.
Both profiles use libstdc++ 15. Distribution development packages: Boost.JSON and
Boost.Container 1.90.0-6ubuntu1, OpenSSL 3.5.5-1ubuntu3.7, zlib 1.3.1, ICU 78.2,
Brotli 1.2.0-3ubuntu0.1 and Zstd 1.5.7. The distribution SQLite is 3.46.1; tests
explicitly use the independently built, hash-verified SQLite 3.54.0 above.

| Artifact                                      | SHA-256                                                            |
| --------------------------------------------- | ------------------------------------------------------------------ |
| `/usr/bin/clang++`                            | `78e9dd2ccc9e88859b3a29109a44bfab00e3d6cb70f00f3daae063eed3d05251` |
| `/usr/bin/ld.lld-21`                          | `ad20c410781a94e1d7b856fc3dd17e5a2b0efe083110e83a4db2eba57bf0b156` |
| `/usr/bin/llvm-symbolizer-21`                 | `68699e9acea9aa0d2efd641b8561914a23102864774fcc1053e51b60bdfae0ed` |
| `/tmp/cppgw-asan-core-20261117/libsqlite3.so` | `43fe49e65bc25cfc1cad9084c88ce77a884057087aafc117a42028f6f1aa2d28` |

`ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer-21` gives source locations. System
`stdbuf -oL` preserves case output; it does not disable sanitizer checks. Each
private network namespace brings up loopback, proves external connections return
ENETUNREACH, drops supplementary groups and root privileges, then launches the
credential-free test environment. Timeouts kill the whole test process group.

## Local review receipt

Actual local review mode, Codex CLI 0.169.0, exit 0 within the 180-second budget:

```sh
timeout 180s codex -c 'review_model="codex-auto-review"' review --commit f4d548f204dd253b830e5c96e37ac6624b7fbd27
```

Reviewed head: `f4d548f204dd253b830e5c96e37ac6624b7fbd27`.
Reviewed parent/base: `141cb4d482a3b0d8624ce0411cbd3b93e45873e6`.
The CLI announced `model: ubiquity/deepseek-flash`, `provider: uos`, `sandbox: read-only`,
despite the requested `review_model`. Exact reviewer identity is unverified, so
this receipt is rejected for official backlog ingestion. No findings were ingested
and no identity-mismatched review was marked complete. Review was not a delivery
or test gate. The retained transcript contains no credential redactions:
`/tmp/cppgw-asan-core-20261117/review.log`, SHA-256
`70f1e623871a486c7d982d7d0f4009bf96d940f6f5e7f7c8f48951838daf4c2a`.

The CLI printed this original final finding twice; both copies are preserved:

```text
The ownership-cycle fix does not break the strong captures held by the readable stream's source algorithms, so abandoned request bodies can still retain their incoming messages. This directly undermines the new leak regression's intended behavior.

Review comment:

- [P1] Break the remaining stream-to-state ownership cycle — /home/runner/work/_temp/agent-jobs/cppgw-asan-core-20261117/checkout/cpp/gateway/src/runtime/http_body.cpp:148-149
  When an incoming body is abandoned without being consumed or cancelled, `StreamState` still owns the `pull` and `cancel` algorithms created above, and both closures strongly capture `state`; `state` in turn owns `incoming`. Resetting only `state->controller` therefore leaves the entire request body alive indefinitely, and `clearAlgorithms()` is never reached because the stream remains readable. Capture state weakly or otherwise detach the algorithms when the body is abandoned.
The ownership-cycle fix does not break the strong captures held by the readable stream's source algorithms, so abandoned request bodies can still retain their incoming messages. This directly undermines the new leak regression's intended behavior.

Review comment:

- [P1] Break the remaining stream-to-state ownership cycle — /home/runner/work/_temp/agent-jobs/cppgw-asan-core-20261117/checkout/cpp/gateway/src/runtime/http_body.cpp:148-149
  When an incoming body is abandoned without being consumed or cancelled, `StreamState` still owns the `pull` and `cancel` algorithms created above, and both closures strongly capture `state`; `state` in turn owns `incoming`. Resetting only `state->controller` therefore leaves the entire request body alive indefinitely, and `clearAlgorithms()` is never reached because the stream remains readable. Capture state weakly or otherwise detach the algorithms when the body is abandoned.
```

Independent assessment of that unaccepted claim: the fix removes the strong edge
from state back to the stream; the state keeps only a weak stream pointer after
synchronous startup. When external stream owners and the startup reaction release,
the stream destructor releases its algorithms, then state and incoming are freed.
It does not need `clearAlgorithms()` before its destructor. The exact abandonment
case fails before the fix and passes afterwards with an expired incoming weak
pointer, including under ASan/LSan. All ten body cases pass in both profiles, and
the full HTTP differential leak falls from 76,401 to 5,928 bytes. Weakening source
algorithm captures as suggested would lose body state when only the stream is kept;
no such source change was made.

## Publication and remaining work

Publication was attempted with:

```sh
GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false git push --set-upstream origin test/cppgw-asan-core-20261117
```

It failed with exit 128:

```text
error: unable to read askpass response from '/bin/false'
fatal: could not read Username for 'https://github.com': terminal prompts disabled
```

`gh auth status` also reports no authenticated host. No credential was printed,
written, refreshed or replaced. A fresh read-only fetch of
`development:refs/remotes/origin/development` succeeds; it is still
`002fe15fa926674c97f0169355d24929bf529710`. The implementation is not its ancestor.
No push, PR, ancestry-preserving development merge, hosted CI receipt or final
`development` checkout handoff is claimed. Preserve the clean owned task branch
for the enclosing authenticated runner publisher / port integration owner. Its
next action is publication and integration through the normal PR workflow; the
pre-existing port/production-build commits are not accepted or merged by this task.

Remaining qualification: the HTTP fuzz owner must fix the excluded
`src/runtime/http.cpp` state cycle and rerun the twelve failed leak binaries.
A supported ARM64 host must run the five permission and eighteen native-startup
cases; this x64 runner correctly fails their platform guards. No owned lifetime
regression remains failing. The local review's model identity remains unverified.
No hosted workflow was dispatched; no production/Mac service, credential source,
live database or external inference provider was activated or accessed by tests.

The documentation follow-up changes only this report. Every tested C++ and helper
input remains identical to the implementation commit. Final gates use the pinned
Prettier 3.9.6 from the frozen lint lock installed outside the checkout, check the
whole task diff for whitespace errors, and reconcile all 849 current input hashes:

```sh
bun --no-env-file --no-install /tmp/cppgw-asan-core-20261117/lint/node_modules/prettier/bin/prettier.cjs --check cpp/gateway/docs/findings/cppgw-asan-core-20261117.md
git diff --check f4d548f204dd253b830e5c96e37ac6624b7fbd27^ HEAD
```

The size gate already passed (851 files checked, no grandfathered entries).
Changed C++ file sizes are 152 and 384 source/helper lines, 78 header lines and
686 / 307 test lines, all below their applicable caps. No C++/helper input changes
after the committed-source sweeps; documentation-only gates do not rerun native
compilation, unrelated TypeScript suites or hosted CI.

## Final handoff summary

Built 460 distinct C++20 translation units and all 118 core-shard binaries per
profile with Clang 21.1.8. Exact full commands:

```sh
python3 cpp/gateway/tests/helpers/core_sanitizers.py plain --require-clean
python3 cpp/gateway/tests/helpers/core_sanitizers.py asan --require-clean
```

Plain: **116/118 binaries passed**. ASan/UBSan: **104/118 binaries passed**. Each
ran **1,315 cases: 1,292 passed, 23 failed**. The 23 failures are ARM64-only guards;
twelve sanitizer binaries also fail on real, unsuppressed outside-owned HTTP
state leaks. There are no build failures, skips, timeouts, ASan access errors or
UBSan diagnostics.

Fixed `src/runtime/http_body.cpp` controller ownership and
`tests/helpers/loopback_http.{hpp,cpp}` terminal listener ownership. Added one
body and three RawClient regressions to their existing test files; all four pass
in both profiles and fail against the original implementations. Added the isolated
`tests/helpers/core_sanitizers.py` runner and this findings report. Original Bun
values and nine body event traces are unchanged; no assertion was weakened.

Implementation: `f4d548f204dd253b830e5c96e37ac6624b7fbd27`. Complete commands,
per-binary counts, all source changes, negative controls and receipt hashes are
above. A green full shard requires the excluded HTTP owner's fix and ARM64
qualification. GitHub authentication blocks publication and development integration;
the committed task branch is preserved for the enclosing runner publisher.
