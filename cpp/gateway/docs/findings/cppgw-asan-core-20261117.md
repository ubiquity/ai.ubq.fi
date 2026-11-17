# Core module AddressSanitizer and UBSan findings — 2026-11-17

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
its input-matching flag is false. It is discovery evidence only and will be
superseded by a complete clean-commit sweep. All sanitizer logs below are retained
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
described above. The ASan/UBSan profile is now compiling every unit with the
required instrumentation; no sanitizer result is claimed yet.

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
plain and ASan/UBSan commands above will run once on the clean implementation
commit. Record both terminal receipts, every residual diagnostic, full commit/tree
IDs and input hashes before handoff. A documentation-only follow-up may append
the final receipts without changing tested C++ or helper inputs.

Local review uses actual `codex review --commit <implementation-sha>` with
`review_model="codex-auto-review"`, a 180-second operation budget and a retained
transcript. Verify the announced model identity before accepting any official
review evidence. Review latency/findings are not a gate; deterministic evidence,
the ownership boundary and explicit platform failures remain the acceptance facts.
No hosted workflow or production deployment is authorized or triggered.
