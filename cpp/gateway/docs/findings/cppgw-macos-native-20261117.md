# Native Apple Silicon findings, 2026-11-17

## Contract v1

Objective: compile every `cpp/gateway/src/**/*.cpp` into an external static archive with the strict
C++20 port flags, run every runtime test first, then all feasible standalone gateway tests on Darwin.
Provide `tools/macos-build.sh` and verified build/test instructions in `docs/MACOS.md`.

Invariants: Bun 1.4.0 and the TypeScript remain the behavioral oracle; preserve every assertion,
module boundary, coroutine lifetime rule and Linux behavior. Source edits are restricted to guarded
Darwin branches or behavior-preserving include/standard-library fixes. Material changes require a
new contract version and invalidate affected evidence.

Owned paths: Darwin portability changes in `include/`, `src/`, `tests/`, plus `docs/MACOS.md`,
`tools/macos-build.sh` and this report, all under `cpp/gateway`. All other paths are read-only.
No service activation, deployment, production data, credential mutation, provider spend or hosted
workflow execution. Non-owned defects are reported with commands and first diagnostics.

Matrix: Apple Silicon / Darwin / Apple Clang and libc++ preferred, Homebrew LLVM only if required;
native C libraries and Boost; offline fixtures and ephemeral loopback/Unix sockets only. Linux-only
confinement must continue failing closed on Darwin, as its TypeScript API does.

Acceptance: complete production compilation and archive; real per-binary and per-case test counts;
Bun-on-this-Mac evidence before platform-conditional expected values; exact commands and dependency
versions; changed-document formatting, shell syntax and `git diff --check`; commit/push attempt and
an exact commit/tree receipt. No unrelated TypeScript, Rust or full repository gates are selected.

Unknowns: Apple SDK floating charconv support, Darwin headers and loader names, SQLite-version
fixtures, child watcher races, native signals and open flags, APFS timestamp range, kqueue transport,
TLS roots, and Linux-host-specific fixtures. Resolve these with compilation, tests and Bun probes.

Long operations: at most four compile workers, 10–30 minutes expected for the initial source/test
build, external per-command logs and periodic progress. Each test has a bounded process-group
timeout, empty HOME, fresh TMPDIR/cwd and an allowlisted credential-free environment. Retain
incremental objects after failures; diagnose rather than repeat unchanged commands. Stop with
passing receipts or exact unresolved failures and ownership boundaries, never inferred passes.

## Discovery

- Starting clean commit: `141cb4d482a3b0d8624ce0411cbd3b93e45873e6`, branch `agent-job`.
- Host: macOS 26.7.2 (25G332), arm64; Apple Clang 21.0.0 (clang-2100.1.1.101),
  target `arm64-apple-darwin25.6.0`; Homebrew 7.0.13; Bun 1.4.0.
- Read the port rules, port notes, testing/build docs, ASan and libc++ findings and macOS draft.
  The user explicitly overrides production-host compile restrictions for this disposable runner.
- Dependencies already present: OpenSSL 3.5.9, SQLite 3.54.1, ICU 79.1, Brotli 1.2.1,
  Zstd 1.5.7, CMake 4.4.5, Ninja 1.13.2. Boost and the selected ICU 78.3 were prepared separately as recorded below.

## Progress

- Homebrew preparation: `HOMEBREW_NO_AUTO_UPDATE=1 brew install boost pkgconf zlib` installed
  Boost 1.92.0_1 and zlib 1.4. Apple Clang passes the strict floating `from_chars`/`to_chars` probe.
- Initial full production pass: **368/369 compiled**, one failure:
  `src/rt/process_loop.cpp:459:22: error: 'posix_spawn_file_actions_addchdir_np' is deprecated:
first deprecated in macOS 26.0`. Darwin deployment targets 26+ now use the POSIX replacement;
  earlier Darwin and Linux retain their existing spelling. All other production units compiled.
- Prepared the official SQLite 3.54.0 amalgamation externally; archive SHA-256
  `68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d`.
  Homebrew 3.54.1 remains installed and untouched. Versioned fixture expectations remain intact.
- `sandbox-exec` preflight accepts loopback TCP and denies `192.0.2.1:443` with `EPERM` (1).
  The runner uses it for every binary, with an environment allowlist and private HOME/TMPDIR/cwd.
- Bun 1.4.0 on this Mac reports `darwin`, `arm64`, `O_NOFOLLOW=256`, `O_CREAT=512`,
  `O_EXCL=2048`, `O_TRUNC=1024`, `O_APPEND=8`, and errno values ENAMETOOLONG=63,
  ELOOP=62, ENOTSUP=45, ENOTEMPTY=66. Linux fixture numbers remain in the other branch.
- Bun evaluated all **317** path expressions in `rt/fs_test.cpp`; **41** differ after
  `chdir('/tmp')` because cwd is `/private/tmp`. Only those expected values gained Darwin literals.
- Bun's Darwin timestamps saturate total signed 64-bit nanoseconds, unlike Linux's seconds.
  Raw `stat` seconds and Bun bigint nanoseconds prove the endpoints are
  `9223372036.854775807` and `-9223372037.145224192`. The Darwin conversion is now clamped to
  those exact endpoints; ordinary fractional timestamp results match the original tests.
- Bun's `process` listener emits `('SIGUSR2',31)` and `('SIGUSR1',30)`. Its subprocess API
  retains a Linux-name quirk: `.kill('SIGUSR1')` exits 138, `.kill('SIGUSR2')` exits 140;
  native `sh -c 'kill -USR1 $$'` exits 158 and `signalCode` is `SIGPWR`. Darwin spawn uses
  a separate Linux-name table; native process listeners retain Darwin constants.
- Darwin-only concurrent-child reaping and closed-child-stdin EPIPE checks pass, including
  64 immediate exits and `waitpid(..., WNOHANG) == -1 / ECHILD` for every child.
- Exact oracle programs/output are external under `/tmp/cppgw-macos-native-20261117/oracle*`.
  Each ran through `bun --no-env-file --no-install` with an allowlisted environment and private home/tmp.
- Publication preflight: `GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/usr/bin/false git push --dry-run origin
HEAD:refs/heads/build/cppgw-macos-native-20261117` exits 128: `fatal: could not read Username for
'https://github.com': terminal prompts disabled`. No credential was printed or saved.

The next production pass compiled **369/369** sources and built the archive. Its exploratory runtime
sweep compiled/linked **36/36** binaries, ran **455 cases: 450 passed / 5 failed**, and passed 31 binaries.
No skip or timeout occurred. Source edits during discovery make this a diagnostic run rather than a
clean-commit acceptance receipt. Exact run: `native/runs/1794914517262831000` under the external root.

| Initial failing case | Evidence and disposition                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| -------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Empty-target symlink | C++ succeeds; Bun also succeeds and `readlinkSync` returns empty string. Darwin expected value is conditional.                                                                                                                                                                                                                                                                                                                                                                                                                              |
| gzip byte identity   | zlib writes Darwin OS byte 19, Linux writes 3. Bun on this Mac independently inflated all nine captured buffers to the original input hashes and recorded their exact compressed hashes; sizes and all round-trip checks remain unchanged. Bun's own gzip/CompressionStream header also uses 19.                                                                                                                                                                                                                                            |
| SQLite lock timing   | Isolated C++ read takes 0 ms; write resolves after 6205 ms once a six-second lock is released. Bun's `bun:sqlite` control with `busy_timeout=5000` resolves after 6251 ms with a six-second lock, but rejects after 7816 ms with a ten-second lock. Darwin sleep coalescing invalidates the Linux timing fact. The Darwin six-second result is conditional and an additional ten-second case retains timeout/rejection coverage; Linux assertions remain unchanged. This lower-level Bun control does not claim native-addon qualification. |
| Exhaustive IDNA      | ICU 79.1 accepts new codepoints and changes block hashes. Bun on this Mac matches **1088/1088 original blocks / 2,228,224 codepoint-context records** exactly. Installed `icu4c@78` 78.3_1 and selected its headers/libraries explicitly; no Unicode expectation changed.                                                                                                                                                                                                                                                                   |
| WebSocket refusal    | C++ errno is -61; the actual pinned ws package loaded by the repository HTTP loader under Bun emits `ECONNREFUSED`, errno -61 and close 1006. Linux -111 remains intact.                                                                                                                                                                                                                                                                                                                                                                    |

The other runtime groups pass, including TCP/Unix `SO_NOSIGPIPE`, half-close/EPIPE, signals, pipes,
child reaping, TLS certificate fixtures, durable KV schema/revisions/expiry/CAS and crash recovery.
The second runtime sweep passes **36/36 binaries, 456/456 cases, zero failures/skips/timeouts** against
ICU 78.3, including all 2,228,224 IDNA records. Runtime source identity is unchanged during its phase;
the full discovery run still has source edits before that phase and is not a clean-commit receipt.
`fs.cpp` instrumented with `-fsanitize=undefined -fno-sanitize-recover=undefined` additionally passes
all ten filesystem cases without a diagnostic, including the normalized INT64_MIN endpoint.

The 403 other standalone targets are being compiled. Apple Clang 21 rejects two same-name lambda
init-captures of structured bindings in `codex_supervisor_secret_test.cpp:106` under `-Wshadow`.
A guarded Darwin capture-name change preserves all 30 dynamically registered cases and the Linux code.
Execution of that correction and the full larger suite remains pending.

The fixture driver now canonicalizes its scratch paths before setting HOME/TMPDIR, so native Darwin
cwd comparisons use the same `/private/tmp` spelling. Build outputs, receipts and archives stay external.
The complete gateway build compiled **423/424** test/support sources, with the one guarded capture
fix pending recompilation. Initial execution was intentionally interrupted after 218 completed targets: 205 passed / 13 non-passing; case counts [2691, 2640, 51].
The broad failures share a wrapper spelling bug: native cwd is `/private/tmp/...`, while the earlier
wrapper supplied `/tmp/...` as TMPDIR, triggering `Gateway assets require the wrapper task workdir`
and `T2 public assets require the wrapper task workdir`. The driver now canonicalizes HOME/TMPDIR/cwd.
No asset test or guard was changed. The interrupted process group was killed and its scratch cleaned;
these partial counts are diagnostic only. The complete rerun uses the corrected committed driver.

## Source changes

All paths are under `cpp/gateway`; Linux production branches remain unchanged except a standard include.

| Path                                     | Reason and evidence                                                                                                                                                                                                                                                                                                                                                  |
| ---------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/rt/fs.cpp`                          | Darwin total-nanosecond saturation matches Bun; use a wide intermediate for normalized INT64_MIN Stats arithmetic. All ten filesystem cases pass with UBSan. Add the direct `<algorithm>` declaration for `std::clamp`.                                                                                                                                              |
| `src/rt/process_loop.cpp`                | macOS 26 deployment targets use nondeprecated POSIX addchdir; Darwin subprocess names preserve Bun's Linux-table quirk separately from native signals. Twelve process cases pass, including named kill, concurrent reaping and EPIPE.                                                                                                                                |
| `tests/rt/fs_test.cpp`                   | 41 Bun-proven cwd path literals, native open flags/errno values, empty symlink success and exact extreme timestamp endpoints become conditional. Linux literals and every assertion remain.                                                                                                                                                                          |
| `tests/rt/process_test.cpp`              | Guard the native hostname buffer and recorded Darwin signals/missing `/etc/hostname`; add concurrent waitpid/ECHILD, named-kill and closed-stdin checks.                                                                                                                                                                                                             |
| `tests/rt/fetch_connection_test.cpp`     | Darwin-only TCP/Unix getsockopt(SO_NOSIGPIPE), FIN/half-close and EPIPE regression; all three cases pass.                                                                                                                                                                                                                                                            |
| `tests/rt/gzip_test.cpp`                 | Darwin gzip OS byte 19 and all nine exact buffer hashes, measured by Bun's independent decompressor; preserve byte counts, original Linux hashes and both round-trip assertions.                                                                                                                                                                                     |
| `tests/rt/kv_sqlite_test.cpp`            | Record the Bun-observed Darwin six-second-lock outcome and add the ten-second rejection control; retain original Linux timeout/error assertions.                                                                                                                                                                                                                     |
| `tests/rt/websocket_test.cpp`            | Actual pinned ws under the repository HTTP loader reports Darwin ECONNREFUSED errno -61, Linux remains -111.                                                                                                                                                                                                                                                         |
| `tests/codex_supervisor_secret_test.cpp` | Guarded capture-name fix for Apple Clang 21's two `-Wshadow` diagnostics on init-captured structured bindings. No case removed or changed.                                                                                                                                                                                                                           |
| `tools/macos-build.sh`                   | External compile-once archive/test driver, exact Links closures, frozen dependency receipts, private credential-free canonical scratch, Seatbelt network preflight, group timeouts, cache hash checks and source identity. Fresh archive publication avoids stale duplicate basename members; the Darwin symbol-table member is excluded from the 369-source census. |
| `docs/MACOS.md`, this report             | Exact prerequisites, reproducible commands, measured host semantics, pass/fail counts and remaining boundaries.                                                                                                                                                                                                                                                      |

## Outside-owned boundaries

- `cmake -S cpp/gateway -B /tmp/cppgw-macos-native-20261117/cmake-probe -G Ninja` exits 1:
  `CMake Error at CMakeLists.txt:3 (message): Configure through cpp/gateway/tools/build.py so resource
bounds and SDK identity are recorded`. Existing presets/drivers remain tied to Linux host paths.
- `rg -n 'Linux ARM64 ELF|applyLinuxPermissions|linux-arm64.node' cpp/gateway/runtime/startup.cpp`
  finds the ELF refusal at line 145, Linux confinement at 325 and Linux native artifact checks at
  365–366. This runtime/release-owned launcher cannot start a Mach-O Mac candidate. It was not activated
  or changed. A production Mach-O/Seatbelt package is separate owner work.

## Local review evidence

Actual review mode: `codex -c 'review_model="codex-auto-review"' review --commit
1bcfc4e59f561cd0bccb2f8fb175384b7df559d7`, base `141cb4d482a3b0d8624ce0411cbd3b93e45873e6`.
CLI 0.169.0 exited 0 in 138.156 seconds, but announced `model: ubiquity/deepseek-flash`, provider `uos`.
The requested reviewer identity is unverified: reject this receipt for official ingestion, ingest
nothing and do not mark an official review complete. Review status does not gate delivery.
The exact credential-redacted transcript remains `/tmp/cppgw-macos-native-20261117/review-1bcfc4e5.log`,
SHA-256 `7c6fa9189088c8d12cc7e69dffb6515fc94e0a2cdfa8091b744e00b69a44a517`.
The original finding was emitted twice; both occurrences remain in that transcript:

> [P1] Include the header that declares std::clamp — /Users/runner/work/_temp/agent-jobs/cppgw-macos-native-20261117/checkout/cpp/gateway/src/rt/fs.cpp:287-288
>
> On the new Apple path this calls `std::clamp`, but `fs.cpp` and its included headers do not include `<algorithm>`. Apple Clang/libc++ does not guarantee this declaration through the current transitive includes, so the native macOS build can fail with `std::clamp` not found; add the direct standard-library include.

The native build had already compiled successfully through transitive declarations. The direct include
is nevertheless the correct standard-library dependency and is added as normal implementation work;
this does not salvage the identity-mismatched official review receipt.

## Continuation contract v2, 2026-11-17

The continuation starts at clean commit `1f095000` on a new Apple Silicon runner. Preserve all v1
changes and evidence. The objective is now to finish the interrupted qualification: rebuild with
the existing driver, verify its already-committed canonical scratch paths and capture-name fix,
execute the whole application suite after the runtime suite, and resolve real Darwin failures.
The v1 non-goals, owned/prohibited paths, Linux invariants and credential/network isolation remain.
No deployment, service activation, provider calls or hosted workflow dispatch is authorized.

Acceptance commands are the driver with `--suite all --require-clean`, selected Bun 1.4.0 oracle
probes for any new host-dependent expectation, a focused `--legacy-loaders` run, changed-document
formatting, `bash -n`, Python AST parsing and `git diff --check`. Unknowns are the unexecuted gateway
targets, fallback loaders and any new-runner dependency drift. The stop condition is full real counts
and exact receipts, or an evidenced outside-owned blocker with the unfinished work preserved.

Long operations: external build root, at most three compile workers on this three-core/7 GiB host,
20–40 minutes expected for a cold build plus the complete serial suite, per-object logs and incremental
hash-checked reuse. Test timeout remains 180 seconds per process group. One persistent execution
handle per run supplies progress; failures trigger diagnosis and focused reruns, not blind rebuilds.
Commit after meaningful steps so another interruption preserves the completed work.

Fresh host observation: macOS 26.7.2 (25G332), arm64, Apple Clang 21.0.0
(`clang-2100.1.1.101`), Homebrew 7.0.11. The previous job's temporary artifacts are absent.
Boost, zlib and ICU 78 must be prepared again; the existing remaining Homebrew versions match
the earlier report. The canonical `Path(scratch_name).resolve()` and guarded capture correction
are already in the starting commit; do not reapply them.

## Continuation discovery and SQLite correction

Prepared Boost 1.92.0_1, zlib **1.3.2** and ICU 78.3_1 with
`HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_ANALYTICS=1 brew install boost zlib icu4c@78`.
The other installed versions match v1. Prepared the same SHA-verified SQLite 3.54.0 dylib using
the commands in MACOS.md, under `/tmp/cppgw-macos-native-20261117b/sqlite-3.54.0`.
All downloads, frozen lint-tool installs, probes, objects and logs remain external.

Started the complete sweep with:

```sh
cpp/gateway/tools/macos-build.sh --build-root /tmp/cppgw-macos-native-20261117b/native \
  --sqlite-prefix /tmp/cppgw-macos-native-20261117b/sqlite-3.54.0 --jobs 3 --suite all --require-clean
```

Run `native/runs/1794925481996443000` started at clean commit
`566c0ccefe9551128bd15d97265a416802b26710`, tree
`dd436ee1028249dbe6acd86def2c15d268acfd3c`. All **369/369 production sources** and **36/36 runtime
test sources** compile; the archive contains 369 members. Initial runtime execution is **34/36 binaries
passing, 456 cases / 449 passed / 7 failed**, with no timeout. Gateway compilation continues.
There are now **405 application targets** and 441 total, including additions merged after v1.
The source corrections below invalidate this initial sweep as final acceptance; retain it as diagnosis.

Fresh Bun 1.4.0 probes confirm canonical cwd/TMPDIR and the asset-wrapper parent check, Darwin flags
and signal constants, 121 default versus two system TLS roots, and the exact Linux-confinement refusal.
Scripts and complete output are `/tmp/cppgw-macos-native-20261117b/oracle-*.{mjs,log}`.

The inherited `src/rt/sqlite_darwin.hpp` URI/readonly-SHM adapter was added after v1's successful
runtime phase and was absent from its source-change table. Bun on this Mac validates its URI and
missing-SHM behavior, but the adapter alone exposed an incomplete portability contract:

| Bun-on-Darwin scenario                                                              | Exact observed result                                                     |
| ----------------------------------------------------------------------------------- | ------------------------------------------------------------------------- |
| `new Database('file:urirel.db?mode=ro')`, absent file                               | `SQLITE_CANTOPEN`, errno 14, `unable to open database file`               |
| Existing `file://.../ro.db?immutable=1`, readonly/strict, no explicit URI flag      | Opens successfully, result 1                                              |
| Close a writable WAL database, default persistence                                  | Both `-wal` and `-shm` remain; readonly query returns `{a:7}`             |
| Explicit `fileControl(SQLITE_FCNTL_PERSIST_WAL, 0)` after accessing WAL, then close | Both sidecars disappear; readonly query throws the same `SQLITE_CANTOPEN` |
| Missing sidecars, immutable readonly connection to stopped checkpointed file        | Returns `{a:7}`; write rejects with `SQLITE_READONLY`                     |

Correction: `src/rt/sqlite.cpp` sets `SQLITE_FCNTL_PERSIST_WAL=1` only on Darwin, matching Bun's
default and preserving explicit disable calls. This fixes the lifecycle cause of six durable-KV
inspection failures without changing their readonly helper, schema/value assertions or expected values.
`tests/rt/sqlite_test.cpp` gains the two exact Bun-proven Darwin URI expectations, retaining Linux
literals, plus a Darwin-only persistence/readback/explicit-disable regression. The source change is
guarded; Linux production preprocessing is unchanged. New runtime total will be 457 cases.
Compilation/execution of this correction is pending the next driver invocation.

Early gates pass: shell syntax, embedded Python AST, both documents' initial Prettier checks,
`bun --no-env-file --no-install run size:check` (851 files; this ratchet covers TypeScript), and
`git diff --check`. No TypeScript source, test or dependency lock changed.

Publication preflight again exits 128: `fatal: could not read Username for 'https://github.com':
terminal prompts disabled`. Public fetch succeeds; `origin/development` is
`002fe15fa926674c97f0169355d24929bf529710`, and the continuation tip is not its ancestor.
No authenticated push, PR or development handoff is claimed.

## Complete diagnostic sweep and resolved engine matrix

The first complete sweep finished all **441 targets**: runtime **34/36** binaries, **449/456** cases;
application **395/405** binaries, **4,539/4,562 reported cases**. Three application processes aborted
without harness summaries; their partial output is preserved and is not included in the case totals.
All **832 translation units** compiled: 369 production, 36 runtime tests and 427 application/support.
There were no build/link failures, skips or timeouts. The canonical wrapper paths eliminated all
previous `/tmp` spelling failures. Ten application targets failed: Claude capacity analytics, accounts,
header meter, usage meters/retry, durable interop, Linux permissions, Responses HTTP conformance,
SQLite readonly and storage Bun KV. The nine storage-related targets share readonly-WAL failures;
the other target expects Linux confinement on Darwin. This run has changed inputs/identity and is
diagnostic, never final acceptance.

The complete Bun-on-this-Mac engine matrix is now observed:

- Bun `bun:sqlite` retains sidecars by default, honors explicit persistence disabling, rejects schema
  access to drained WAL without SHM, and leaves **no new sidecar** after that failed readonly access.
  Construction itself succeeds; schema-free `PRAGMA busy_timeout=5000` returns `{timeout:5000}`;
  comment-only SQL throws `Statement has finalized`. Rejecting every readonly prepare would be wrong.
- The root-lock-verified published `@deno/kv@0.14.0` Darwin ARM64 addon, run under Bun with explicit
  JSC serialization on a fresh private database, removes both sidecars on close. Its lifecycle control
  does not qualify the separately patched release artifact, native budgets or packaging. Both npm
  archives were verified against their existing SHA-512 entries in `bun.lock`; no lock was changed.
- Bun's immutable readonly connection reads the drained snapshot without changing it and reports
  `PRAGMA journal_mode` as `delete`; both physical database header bytes remain 2 (WAL format).
- Running the actual TypeScript `inspectSqliteReadOnly` on this Mac returns `bun-jsc-v1` without
  changing bytes, refuses each newly introduced sidecar, and rejects a changed source timestamp.

Final correction strategy preserves those distinct contracts. Darwin `rt::sqlite` defaults to persistent
WAL and uses a small SQLite VFS copy for readonly connections: its real `xOpen` removes CREATE only
for WAL opens. SQLite still decides when SQL accesses the schema; a failed readonly query cannot leave
an empty WAL that would poison the TypeScript fallback. Existing readonly-SHM restrictions remain.
The VFS and its C declarations/symbol bindings are Darwin-only and support both direct and dlopen paths.
`rt::SqliteKv` explicitly disables persistence on Darwin, matching the native SDK's drained-close policy.

The two runtime/interop test inspectors use immutable URIs only for private sidecar-free stopped
snapshots, retaining ordinary reads for live WAL. The original no-WAL, schema, queue, bytes, CAS,
revision, expiry and crash-recovery assertions stay intact. The Darwin immutable journal-mode assertion
uses Bun's `delete` and also checks the physical WAL header bytes. The readonly application test has
its exact Darwin URI-success branch alongside the untouched Linux refusal branch.

Focused verification of the final VFS approach passes **3 binaries / 55 cases**: SQLite 30, durable
SQLite KV 18, readonly inspection 7. The new runtime regression also checks schema-free pragmas,
comment-only SQL, deferred schema failure and absence of sidecars. Earlier focused diagnostic rounds
are preserved under `focused-sqlite-round{1,2,3}`; the verifier and logs are external.

All five `permissions_linux_test` cases retain their Linux assertions and gain guarded Darwin
expectations for the exact TypeScript early refusal. Twelve Bun source probes cover default, absent
and malformed artifacts, support queries, apply, relative paths and immutable grants; every result is
`Linux confinement requires Linux ARM64`. The Darwin child test asserts refusal, never fake success.

Changed-surface gates for the next committed-tree run: strict native compilation plus all runtime and
application tests for `src/rt/{sqlite.cpp,sqlite_darwin.hpp,kv_sqlite.cpp}`; the exact changed standalone
tests for SQLite, KV, readonly snapshots, durable interop and permissions; document Prettier, shell and
embedded-Python syntax and `git diff --check`. No TypeScript type/import surface changed.

## Corrected-source complete sweep checkpoint

Run `native/runs/1794928218517772000` completed on clean commit
`c039f1bba7868f2e7d2b79bc3cf2af82211ed5ba`, tree `70ae08bded9401f0236c4c45fb15985164136f6d`,
with both identity/input stability checks true. Strict compilation again passes all 832 units.
Runtime: **36/36 binaries, 457/457 cases**. Application: **404/405 binaries, 4,571/4,572 cases**.
No abort, build/link failure, skip or timeout remains. All nine previously failing storage/permission
targets other than interop now pass, including full Responses HTTP conformance and storage Bun KV.

The one remaining interop failure was an implementation mistake in this continuation's fixture
adaptation: an ambiguous patch selected the writable recorded-row setup block instead of the later
readonly queue inspector. Corrected that placement, restoring writable setup and applying the
Darwin snapshot URI only to actual readonly inspections. No assertion or expected value changed.
The complete committed-tree driver run is repeated after this test-source correction.

Production Linux-source comparison: `/usr/bin/unifdef -t -U__APPLE__` produces byte-identical output
for the starting `1f095000` and corrected `src/rt/{sqlite.cpp,sqlite_darwin.hpp,kv_sqlite.cpp}`.
The default unifdef C lexer rejected existing C++ digit separators; text mode resolves the same
platform directives without misreading those literals. This is a source-equivalence check, not Linux execution.

Actual local review mode completed in 79.968 seconds, exit 0:

```sh
codex -c 'review_model="codex-auto-review"' -c 'sandbox_mode="read-only"' review --base 1f095000
```

Reviewed head/tree are the `c039f1bb` pair above; exact base is
`1f0950006c0e8510d6efa20151825c0787ee1a6b`. The CLI banner reports
`model: ubiquity/deepseek-flash`, `provider: uos`; reject this model-identity-mismatched receipt
for official ingestion, ingest nothing, and do not mark official review complete. The returned text
contains no finding. Exact credential-redacted transcript: `/tmp/cppgw-macos-native-20261117b/review.log`,
SHA-256 `81fc366bdb794bf94e8021e34ad0a9a1bc75facebdfef40a482497c80edf6424`.
This review does not cover the later fixture placement correction and does not gate delivery.

## Complete passing native receipt

The final full primary run is `native/runs/1794929084815645000`, using the exact `--suite all`
command above. It exits **0**, with no source edits during compilation or execution:

- Commit: `c6cd29f9e9743e37c4eca1a0313194fd3279ba9e`.
- Tree: `8599a3a0bc292a66a664832f01ca1404d2192e3d`.
- Clean checkout, unchanged input hashes and unchanged commit/tree identity: all true.
- Production: **369/369 compiled** and 369-member static archive.
- Tests/support: **463/463 compiled** (36 runtime plus 427 application/support).
- Runtime: **36/36 binaries, 457/457 cases passed**.
- Application: **405/405 binaries, 4,572/4,572 cases passed**.
- Total: **441/441 binaries, 5,029/5,029 cases passed**; zero failures, aborts, skips or timeouts.
- Archive SHA-256: `d9bf7a5ee258372de222994df788bc0a01746f3ab676268f0da3918b12c91d9f`.
- `results.json` SHA-256: `bc6ed250e624ede717ec929f9fba0adc17e27f786a72f7e41aed998a673dae9f`.
- `provenance.json` SHA-256: `09cea71e9bb03188f17ff50d4d24859c1374fd15066a25dd1563584e27ca2db6`.

Every application target ran, including the three cluster targets. The earlier guarded capture
correction and canonical wrapper paths are verified without reapplying their existing changes.
The final suite covers native child exit/kill/pipes/reaping, signals, TCP/Unix HTTP and WebSockets,
SO_NOSIGPIPE/half-close/EPIPE, local TLS certificates, all 2,228,224 IDNA records, exact gzip bytes,
filesystem precision, durable SQLite state and actual handler delivery/settlement fixtures.

Every continuation source change (all under `cpp/gateway`; v1 changes remain documented above):

| Path                                | Change, reason and evidence                                                                                                                                                             |
| ----------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/rt/sqlite.cpp`                 | Darwin persistence default, readonly VFS selection and guarded direct/dlopen VFS bindings; Bun proves lifecycle, URI and SQL timing; 30 SQLite cases plus the entire suite pass.        |
| `src/rt/sqlite_darwin.hpp`          | Retain inherited URI/readonly-SHM adapter; add readonly WAL VFS and scoped C-header macro handling. Failed schema access leaves no sidecar, while schema-free SQL keeps Bun behavior.   |
| `src/rt/kv_sqlite.cpp`              | Darwin native-KV connection explicitly disables Bun-style WAL persistence, matching the checksum-verified SDK lifecycle control; original drain/reopen/CAS/queue/crash assertions pass. |
| `tests/rt/sqlite_test.cpp`          | Two exact Bun-on-Darwin URI expectations and one persistence/readonly-SQL regression; all Linux values remain.                                                                          |
| `tests/rt/kv_sqlite_test.cpp`       | Darwin stopped-snapshot readonly URI, immutable connection's Bun-proven `delete` mode plus physical WAL-header assertions; no original schema/value/expiry/queue assertion removed.     |
| `tests/kv_durable_interop_test.cpp` | Darwin immutable URI only for four readonly snapshot inspections with no sidecars; writable setup and live WAL reads retain ordinary paths. All seven cases pass.                       |
| `tests/sqlite_readonly_test.cpp`    | Darwin URI success/codec/no-sidecar assertions alongside the original Linux error branch. All seven cases, including both immutable fences, pass.                                       |
| `tests/permissions_linux_test.cpp`  | Five guarded Darwin refusal variants, including forked-child refusal; twelve actual TypeScript probes prove the exact unsupported-platform error. All Linux assertions remain.          |

`docs/MACOS.md` now records verified versions, complete counts and reproducible external build/test
commands. `tools/macos-build.sh` is unchanged in this continuation and is qualified by the full sweep.
Darwin fallback loader syntax checks pass for all three loader sources; execution is the remaining
qualification step before restoring the primary native archive. Documentation-only changes after the
primary receipt do not alter its consumed source inputs.

## Fallback loaders and restored primary archive

Both final commands exit 0 on clean documentation checkpoint
`bc8fc4f60423614c38c480d58e33213f68a701eb`, tree `bea2f5bdfb7d7672cb3b016ed9182ae4d168d7c2`:

```sh
cpp/gateway/tools/macos-build.sh --build-root /tmp/cppgw-macos-native-20261117b/native \
  --sqlite-prefix /tmp/cppgw-macos-native-20261117b/sqlite-3.54.0 --jobs 3 --suite rt \
  --filter gzip,sqlite,fetch_transport --legacy-loaders --require-clean
cpp/gateway/tools/macos-build.sh --build-root /tmp/cppgw-macos-native-20261117b/native \
  --sqlite-prefix /tmp/cppgw-macos-native-20261117b/sqlite-3.54.0 --jobs 3 --suite none --require-clean
```

Fallback run `native/runs/1794930031969413000`: **4/4 binaries, 91/91 cases**, zero failures,
skips, aborts or timeouts; input and identity stability true. This executes Darwin's `libz.1.dylib`,
`libsqlite3.dylib`, `libbrotlidec.1.dylib` and `libzstd.1.dylib` paths, including the readonly VFS
bindings through dlsym. Results SHA-256:
`0196e0ce7c4a373a2845ea04ba9a89b489060032c39a04c5811c624641522716`;
provenance SHA-256: `5fa20aeb5d5f652db2091dc9a5cd9af83d4b28d92fdcfc270e34c397b076b93e`.

Restore run `native/runs/1794930155908005000`: all **369/369 production objects** use the native
define again; a fresh 369-member archive is published externally. Tests are explicitly unselected
only for this archive restoration. Input/identity stability true; provenance SHA-256
`f5226c7df15fe9180ae6395d23f4ea826ffd927a466b0c60a55f7430906aa821`.
Current primary archive: **680,701,232 bytes**, SHA-256
`43c670d08b260f091529c6b23dcbb7d40be193d75e35354bf607d25caef2315e`.
The archive hash differs after the documented recompile/archive cycle; the full primary test receipt
retains its own artifact hash and all source inputs are identical. No test binary remains in the build root.

Final changed-document formatting, shell syntax, embedded Python AST and whitespace checks pass.
`git diff --exit-code c6cd29f9 -- cpp/gateway/src cpp/gateway/include cpp/gateway/tests cpp/gateway/tools`
confirms all changes after the full passing receipt are documentation only. No unrelated TypeScript,
Rust, Linux-only fixture, hosted CI, deployment or production probe was run.

Fresh outside-owned checks reproduce the two existing boundaries:

```text
cmake -S cpp/gateway -B /tmp/cppgw-macos-native-20261117b/cmake-probe -G Ninja
CMake Error at CMakeLists.txt:3 (message):
  Configure through cpp/gateway/tools/build.py so resource bounds and SDK
  identity are recorded
```

`runtime/startup.cpp:145` still refuses non-Linux-ARM64 ELF artifacts, line 326 applies Linux
confinement, and lines 361–362/402–403 require `native/permissions/linux-arm64.node`.
These are runtime/release-owner work, unchanged and never activated. Arbitrary macOS keychain trust
equivalence, older Mac/SDK/Intel hosts, patched native-addon acceptance and Linux/Pi/sanitizer/performance
profiles remain their separate qualification surfaces.

Publication command:

```sh
GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/usr/bin/false \
  git push origin HEAD:refs/heads/build/cppgw-macos-native-20261117b
```

It exits 128: `fatal: could not read Username for 'https://github.com': terminal prompts disabled`.
`gh auth status --hostname github.com` also reports no authenticated host. No credentials were printed,
saved, replaced or refreshed. Public development fetch works, but the task commits are outside
`origin/development`; no authenticated PR, merge or development checkout handoff is claimed.
Leave the clean owned `agent-job` checkout for the enclosing runner publisher / parent port owner,
who owns publication and normal ancestry-preserving integration. Local review remains unaccepted
for its reported model-identity mismatch and is not a delivery gate.

## Final handoff

Built all **369 production sources** into a native Apple Silicon C++20/libc++ archive with Apple
Clang 21 and the exact commands above. All **832 source/test/support units compile**; the whole
suite passes **441 binaries / 5,029 cases**, plus **4 fallback-loader binaries / 91 overlapping cases**.
Every continuation source change and its Bun evidence is listed in the per-file table; the inherited
Darwin changes are preserved and verified. No test failure, skip or timeout remains. Build artifacts
and oracle/review logs are outside the checkout. Source/build/test work is finished; GitHub write
authentication blocks publication/integration, owned by the enclosing publisher. The Linux-specific
CMake/release launcher and Mac service/packaging/trust qualification remain outside this task.
