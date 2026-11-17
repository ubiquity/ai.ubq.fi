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
The complete gateway test build is in progress; final receipts follow below.

## Final handoff

Pending exact commands, pass/fail counts, source changes, unresolved work and outside-owned defects.
