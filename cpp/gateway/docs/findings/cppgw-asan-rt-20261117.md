# Runtime sanitizer findings — 2026-11-17

Runtime validation passes: **36 binaries / 452 cases in each profile**, zero
failures, timeouts, sanitizer reports, or suppressions. The original 34 binaries
contain 446 cases; two new lifetime suites add six cases. The exact committed-source receipts and delivery limits follow below.

Contract v1: build every `tests/rt/*_test.cpp` with all `src/rt/*.cpp`, retain the Bun
1.4.0 behavioral oracle and warning policy, run ASan/UBSan and plain `-O2`, and fix
root causes only in the assigned runtime sources/headers and runtime tests.
`json.cpp`, `url.cpp`, `regex*.cpp`, and their public headers are read-only.
No production activation, credentials, external inference, TypeScript changes,
build-tool changes, or unrelated port fixes are in scope. Stop when all runnable
tests have both receipts and every observed failure is fixed or documented with
its ownership boundary. A documented ownership-cycle limitation may receive only
a narrowly justified LSan suppression.

Starting commit: `09d44bd5` on `agent-job`; it contains the existing port and
production-build work beyond `origin/development` (`002fe15f`). Preserve those
pre-existing commits without treating them as accepted task work.

Host: Ubuntu 26.04.1 x86_64, Clang 21.1.8, GCC 14.3.0, Bun 1.4.0. Choose Clang
for sanitizer stack traces. Builds, binaries, temporary homes, working directories,
and logs are kept outside the checkout under `/tmp`. Test environments use an
explicit non-credential allowlist, fresh HOME/TMPDIR/cwd, and no dotenv loading.

## Findings

| Test                                                    | Tool                                  | Cause                                                                                                                                                                          | Fix or disposition                                                                                                                                                                         |
| ------------------------------------------------------- | ------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Discovery                                               | —                                     | 34 original runtime test files, 44 runtime translation units                                                                                                                   | Two additional lifetime suites add six cases. Both complete 36-binary sweeps pass all 452 cases                                                                                            |
| All binaries, `async.cpp:207`                           | Clang 21 `-Werror`                    | Unused internal `operator!=` fails `-Wunneeded-internal-declaration`                                                                                                           | Remove redundant overload; C++20 rewrites `!=` from the retained `operator==`                                                                                                              |
| All binaries, `fetch_transport_internal.hpp:242`        | Clang 21 `-Werror`                    | Unread pool `capacity_` duplicates the two slot vectors' sizes                                                                                                                 | Remove the redundant field/initializer; both vectors still receive the original capacity                                                                                                   |
| All binaries, `tls.cpp:71`                              | Clang 21 `-Werror`                    | TLS hostname validation implicitly converts string `char` to unsigned byte                                                                                                     | Make the byte conversion explicit, preserving the original range checks                                                                                                                    |
| `kv_test.cpp:94,98,103`                                 | Clang 21 `-Werror`                    | Base64 test helper shifts promoted signed integers before assigning unsigned chunks                                                                                            | Promote bytes explicitly to unsigned before shifting; assertions and outputs unchanged                                                                                                     |
| `number_test.cpp:2356`                                  | Clang 21 `-Werror`                    | Literal-ASCII test loop implicitly converts `char` to unsigned byte                                                                                                            | Iterate the literal as `char`; all three values and assertions are unchanged                                                                                                               |
| `websocket_test.cpp:335`                                | Clang 21 `-Werror`                    | Header fixture implicitly converts string `char` to unsigned byte                                                                                                              | Make the Latin-1 byte conversion explicit; expectations unchanged                                                                                                                          |
| `fetch_transport_test`, aborting a response body        | ASan heap-use-after-free              | `makeResponse` cancellation resets the last connection owner while `Connection::fill` is suspended; its socket completion reads freed `open_` at `fetch_transport_net.cpp:147` | `fill` now synchronously acquires shared ownership around the private read coroutine. Focused sanitizer rerun: 33 transport + 9 fetch + 2 lifetime cases pass                              |
| `fs_test`                                               | Assertions                            | Original Linux/AArch64 `O_NOFOLLOW` literal and production filesystem's extreme timestamp range differ on this x86_64 runner                                                   | Bun confirms 131072 on x86_64; retain 32768 for AArch64. Fixtures now honor TMPDIR. Run on tmpfs, where Bun preserves every original extreme timestamp; no timestamp expectation changed   |
| `process_test`                                          | Assertion                             | Test assumes `process.arch == arm64`, runner correctly reports `x64`                                                                                                           | Bun confirms `x64` on x86_64; preserve the original `arm64` expectation on AArch64                                                                                                         |
| `sqlite_test`                                           | Assertions                            | Native test values are pinned to SQLite 3.54.0, installed distribution library is 3.46.1                                                                                       | Build the official 3.54.0 amalgamation in `/tmp` and select its header/library explicitly; no SQLite expectation changed                                                                   |
| `websocket_test`, four cases in the original full sweep | LSan, 473,220 bytes / 125 allocations | Closing or open sockets keep callbacks capturing their WebSocket after the loop discards IO completions; no close event runs to break the cycles                               | Weak per-loop tracking and teardown release callbacks without emitting abandoned events. All 17 integration + 4 lifetime cases pass under sanitizers without suppressions                  |
| `websocket_test`, isolated connection-failure case      | LSan, 4,560 bytes / 44 allocations    | An overlong Unix path throws while a separate TCP connection is pending: its retry function strongly captures itself, and unpublished handshakes keep their WebSocket          | Retry function captures itself weakly; constructor exceptions abort/release the unpublished request. The new constructor/connecting teardown regressions pass                              |
| `run_checks.py`                                         | Local review / receipt audit          | Dirty working-tree builds were labeled with the base HEAD's tree                                                                                                               | Label dirty receipts with null commit/tree and separate HEAD metadata; `--require-clean` requires a committed tree and verifies identity and all 126 runtime input hashes stayed unchanged |

## Reproduction and acceptance

Changed paths are the runtime sources/headers, five existing runtime tests, two new
lifetime test files, the runtime check runner, and this report. The selected gates
are the 36 standalone runtime binaries in both configurations, the required
warning-as-error C++ type/compile checks, Python syntax, changed-document Prettier,
`git diff --check`, and the size ratchet (851 files checked; zero grandfathered).
No TypeScript application source or import changed, so application TypeScript,
ESLint, and unrelated test suites are outside this change's gate surface.

The initial unsuppressed sanitizer sweep ran all 34 binaries: 29 passed, three had
host/dependency assertion failures, the fetch transport stopped on the UAF, and
WebSocket stopped on LSan. The initial plain sweep passed 31/34 binaries (the same
three host/dependency failures). All were resolved without skipping a binary or
changing a timestamp, codec, network, or SQLite expected value.

The source-matching complete rerun passed **36/36 binaries, 452/452 cases** under
ASan/UBSan and again under plain `-O2`. No UBSan diagnostic occurred. No documented
cycle limit required a suppression, so no `lsan.supp` was added.

```sh
python3 cpp/gateway/tests/rt/run_checks.py --profile both --jobs 4 \
  --sqlite-prefix /tmp/cppgw-sqlite-3.54.0 --fs-scratch-root /dev/shm
```

After committing, use the identical command with `--require-clean` to bind the
acceptance receipts to that exact commit and tree. The runner refuses in-checkout
build output, checks every Links closure with the existing resolver, compiles each
of the 44 runtime sources once per profile and links every object into each test.
No host-specific wrapper, preset or repository CMake file was edited. Its CMake
configure/build commands and exact compiler argv are retained in
`/tmp/cppgw-asan-rt-20261117/{asan,plain}/build.log` and `compile_commands.json`.
Each invocation retains its own `runs/<id>/{logs,results.json,provenance.json}`;
`results.json` in the profile directory is the latest run. Each process has a
180-second process-group timeout, a credential-free environment allowlist, and a
fresh HOME/TMPDIR/cwd. Scratch is under `/tmp`, except `fs_test` under `/dev/shm`.

Dependencies installed on this runner:

```sh
sudo apt-get update -qq
sudo apt-get install -y -qq libboost-json-dev libboost-container-dev libbrotli-dev
```

The other development packages were already installed: OpenSSL 3.5.5, SQLite
3.46.1, zlib 1.3.1, ICU 78.2 and Zstd 1.5.7. The added packages supply Boost 1.90
and Brotli 1.2.0. Both profiles use Ubuntu Clang 21.1.8 with libstdc++ 15. GCC
14.3.0 was present but was not used for the reported tests.

The distribution SQLite is insufficient for the already-versioned native
expectations. The exact independent test library was prepared as follows:

```sh
curl --fail --silent --show-error --location \
  https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip \
  --output /tmp/sqlite-amalgamation-3540000.zip
python3 - <<'PY'
from pathlib import Path
import hashlib, shutil, zipfile
archive = Path('/tmp/sqlite-amalgamation-3540000.zip')
assert hashlib.sha256(archive.read_bytes()).hexdigest() == '68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d'
with zipfile.ZipFile(archive) as source:
    source.extractall('/tmp/cppgw-sqlite-src')
prefix = Path('/tmp/cppgw-sqlite-3.54.0')
for directory in ['include', 'lib']:
    (prefix / directory).mkdir(parents=True, exist_ok=True)
shutil.copy('/tmp/cppgw-sqlite-src/sqlite-amalgamation-3540000/sqlite3.h', prefix / 'include/sqlite3.h')
PY
clang -O2 -fPIC -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_COLUMN_METADATA=1 \
  -DSQLITE_DQS=3 -shared -Wl,-soname,libsqlite3.so \
  -o /tmp/cppgw-sqlite-3.54.0/lib/libsqlite3.so \
  /tmp/cppgw-sqlite-src/sqlite-amalgamation-3540000/sqlite3.c -ldl -pthread -lm
```

`sqlite3_libversion()` returned `3.54.0`; the resulting library SHA-256 is
`43fe49e65bc25cfc1cad9084c88ce77a884057087aafc117a42028f6f1aa2d28`.
The runner selects that library and its matching header explicitly. This is an
offline test dependency build, not application startup behavior.

Sanitizer flags: `-std=c++20 -Wall -Wextra -Wshadow -Wconversion -Werror
-DBOOST_JSON_NO_LIB -DBOOST_CONTAINER_NO_LIB -DBOOST_SYSTEM_NO_LIB -O1 -g
-fsanitize=address,undefined -fno-omit-frame-pointer
-fno-sanitize-recover=undefined`, with the two gateway include directories.
Each sanitizer execution sets
`ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1`
and `UBSAN_OPTIONS=print_stacktrace=1`. Plain verification uses `-O2` without
sanitizers. Both profiles additionally define `UOS_GATEWAY_NATIVE_LIBRARIES`, use
`-pthread`, and include `cpp/gateway/include` and `cpp/gateway`. Both link
Boost.JSON, Boost.Container, OpenSSL (`ssl`, `crypto`), pthread, dl, the selected
SQLite, zlib, Brotli decoder, Zstd, and ICU (`icuuc`). Every runtime translation
unit is linked directly into every binary.

Long-operation budget: build with at most four concurrent compilers; expect the
initial two-profile compile to take 10–25 minutes. Preserve compiler logs and
incremental objects, inspect progress at most once per minute, and stop to diagnose
the first failed compile rather than retry an unchanged command. Each test gets a
180-second process-group timeout. Expected runtime is seconds per binary; record
timeouts without suppressing or skipping the test. Keep output to state changes
and compact summaries; retain full reports in `/tmp`.

## Exact committed-source receipts

Both final sweeps passed **36 binaries / 452 cases**, with zero failed cases,
timeouts, ASan/UBSan/LSan diagnostics or suppressions, on this clean source commit:

- Commit: `5a4b09f61655c7c37f64ccce06c3b7183c332fc1`.
- Tree: `e77e7466dc9f4dd2df613ada0ec5f5e0c25a34af`.
- All 126 runtime input hashes stayed unchanged through each build and run.
- Both receipts have `worktree_clean`, `inputs_unchanged`, and
  `identity_unchanged` set to true. Only this report changes after that checkpoint.

Exact acceptance command (Clang 21.1.8, both profiles):

```sh
python3 cpp/gateway/tests/rt/run_checks.py --profile both --jobs 4 --require-clean \
  --sqlite-prefix /tmp/cppgw-sqlite-3.54.0 --fs-scratch-root /dev/shm
```

The regular `-O2` build is Valgrind-free. No test was omitted. The IDNA suite also
checked its 2,228,224 recorded code-point/context results. Tests for the read-only
JSON, URL and regex sources pass; none of those sources or public headers changed.

- `asan` run: `/tmp/cppgw-asan-rt-20261117/asan/runs/1794912161636910879`.
  `results.json` SHA-256: `2b00ea6c0d164c4970b08a4258cfd7ddc7ff80864e9e0d4f1b3f709d5d61aa48`.
  `provenance.json` SHA-256: `06c39160041f653870fdad5739661cc5e126409867a5b1cfcb4c38fa2954f644`.
- `plain` run: `/tmp/cppgw-asan-rt-20261117/plain/runs/1794912238825300289`.
  `results.json` SHA-256: `caa816dac004e44e2ba4122f1c5559c338843cd98c44f55b8441ccfe1b1fd644`.
  `provenance.json` SHA-256: `1f6831a31603ae2a779cbcafaf97e32d1eb6f0aa1c8a1ba42e4f8c66c15f6070`.

| Binary                    | Cases | ASan/UBSan | Plain `-O2` |
| ------------------------- | ----: | ---------- | ----------- |
| `abort_test`              |    13 | PASS       | PASS        |
| `async_test`              |    32 | PASS       | PASS        |
| `base64_test`             |     6 | PASS       | PASS        |
| `clock_test`              |    11 | PASS       | PASS        |
| `console_test`            |     9 | PASS       | PASS        |
| `crypto_test`             |    18 | PASS       | PASS        |
| `date_test`               |     6 | PASS       | PASS        |
| `env_test`                |     6 | PASS       | PASS        |
| `error_test`              |     4 | PASS       | PASS        |
| `fetch_connection_test`   |     2 | PASS       | PASS        |
| `fetch_test`              |     9 | PASS       | PASS        |
| `fetch_transport_test`    |    33 | PASS       | PASS        |
| `fs_test`                 |    10 | PASS       | PASS        |
| `gzip_test`               |    10 | PASS       | PASS        |
| `harness_test`            |     3 | PASS       | PASS        |
| `headers_test`            |     4 | PASS       | PASS        |
| `http_test`               |     6 | PASS       | PASS        |
| `json_test`               |    24 | PASS       | PASS        |
| `kv_js_test`              |     8 | PASS       | PASS        |
| `kv_jsc_deserialize_test` |     7 | PASS       | PASS        |
| `kv_sqlite_test`          |    17 | PASS       | PASS        |
| `kv_test`                 |    53 | PASS       | PASS        |
| `nullable_test`           |     6 | PASS       | PASS        |
| `number_test`             |    21 | PASS       | PASS        |
| `ordered_test`            |     9 | PASS       | PASS        |
| `process_test`            |    10 | PASS       | PASS        |
| `record_test`             |     6 | PASS       | PASS        |
| `regex_test`              |    16 | PASS       | PASS        |
| `sqlite_test`             |    29 | PASS       | PASS        |
| `stream_test`             |    10 | PASS       | PASS        |
| `string_test`             |    19 | PASS       | PASS        |
| `text_codec_test`         |     6 | PASS       | PASS        |
| `url_idna_test`           |     2 | PASS       | PASS        |
| `url_test`                |     6 | PASS       | PASS        |
| `websocket_lifetime_test` |     4 | PASS       | PASS        |
| `websocket_test`          |    17 | PASS       | PASS        |

## Bun host observations

Bun 1.4.0 on this x86_64 Linux host reports `process.arch === "x64"` and
`fs.constants.O_NOFOLLOW === 131072`. The original AArch64 expectations (`arm64`, 32768) remain in their architecture branch. This corrects proven host-specific
expectations; it does not change application behavior.

The runner's ordinary filesystem clamps extreme timestamps just as Bun does.
Tmpfs preserves the original values. Recorded seconds (atime, mtime):

| Input                           | Ordinary filesystem under Bun | Tmpfs under Bun / retained test expectation |
| ------------------------------- | ----------------------------- | ------------------------------------------- |
| `Date(8.64e15), Date(-8.64e15)` | `15032385535, -2147483648`    | `8640000000000, -8640000000000`             |
| `9.2e18, 1`                     | `15032385535, 1`              | `9200000000000000000, 1`                    |
| `1e19, 1e300`                   | `15032385535, 15032385535`    | `9223372036854775807, 9223372036854775807`  |

The fixture now honors TMPDIR and the runner selects a fresh tmpfs scratch
for `fs_test`, keeping every original timestamp expectation. Reproduce the Bun
observations without credentials or dotenv:

```sh
python3 - <<'PYTHON'
import shutil, subprocess, tempfile
bun = shutil.which('bun')
program = '''import fs from "node:fs"; import os from "node:os";
const file = os.tmpdir() + "/times"; fs.writeFileSync(file, "");
console.log(JSON.stringify({bun:Bun.version, arch:process.arch, O_NOFOLLOW:fs.constants.O_NOFOLLOW}));
for (const pair of [[new Date(8.64e15),new Date(-8.64e15)],[9.2e18,1],[1e19,1e300]]) {
  fs.utimesSync(file,pair[0],pair[1]); const s=fs.statSync(file,{bigint:true});
  console.log(String(s.atimeNs)+" "+String(s.mtimeNs));
}'''
for parent in ['/tmp', '/dev/shm']:
    with tempfile.TemporaryDirectory(prefix='rt-oracle-', dir=parent) as scratch:
        subprocess.run([bun, '--no-env-file', '--no-install', '-e', program],
                       cwd=scratch, env={'PATH':'/usr/bin:/bin','HOME':scratch,'TMPDIR':scratch}, check=True)
PYTHON
```

## Source changes and regression evidence

Paths below are relative to `cpp/gateway`.

| Changed files                                                                          | Reason and evidence                                                                                                                                                                                                                                                                      |
| -------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/rt/async.cpp`                                                                     | Remove redundant C++20 `!=`; Clang's fatal unused-declaration diagnostic is gone and all 32 async cases pass                                                                                                                                                                             |
| `src/rt/fetch_transport_internal.hpp`, `src/rt/fetch_transport_net.cpp`                | Acquire a connection owner when `fill()` is called and keep it through cancellation completion; remove duplicate unused pool capacity storage. The original ASan UAF and both new lifetime cases are clean; all 33 transport cases pass                                                  |
| `src/rt/tls.cpp`                                                                       | Explicit unsigned-byte conversion in hostname validation; preserves checks and passes the strict warning build plus fetch/WebSocket TLS cases                                                                                                                                            |
| `src/rt/websocket.cpp`, `src/rt/websocket_internal.hpp`, `src/rt/websocket_socket.cpp` | Track WebSockets weakly per IO context and release socket/receiver/listener owners during teardown without emitting abandoned events; clean up a constructor exception's unpublished request. Original 473,220-byte LSan report is gone; all 17 integration and four lifetime cases pass |
| `src/rt/websocket_client.cpp`                                                          | Retry function retains itself through the pending IO handler rather than a self-capture cycle; pending-connect and synchronous-failure regressions are clean                                                                                                                             |
| `tests/rt/fetch_connection_test.cpp`                                                   | Two regressions verify ownership starts at the call and ends after deferred or canceled IO, including weak-reference checks in plain builds                                                                                                                                              |
| `tests/rt/websocket_lifetime_test.cpp`                                                 | Four regressions cover open cycles, discarded close completion, pending connection and synchronous Unix-endpoint failure; teardown must not emit close                                                                                                                                   |
| `tests/rt/fs_test.cpp`, `tests/rt/process_test.cpp`                                    | Preserve Bun's architecture-dependent constants and use the assigned scratch filesystem; Bun evidence above proves why the AArch64-only assumptions failed                                                                                                                               |
| `tests/rt/kv_test.cpp`, `tests/rt/number_test.cpp`, `tests/rt/websocket_test.cpp`      | Explicit unsigned promotion for base64 shifts and Latin-1 header bytes, and an ASCII-typed hash loop; fix Clang conversion diagnostics without changing recorded outputs                                                                                                                 |
| `tests/rt/run_checks.py`                                                               | Reproducible external builds, complete Links closures, clean environments, process-group timeouts, retained per-run logs, source fingerprints and clean-commit acceptance receipts. It built and ran every binary in both profiles                                                       |
| `docs/findings/cppgw-asan-rt-20261117.md`                                              | This report, including original failures, exact commands, dependency hashes, final results and delivery limits                                                                                                                                                                           |

Additional checks passed:

```sh
bun --no-env-file run size:check
python3 cpp/gateway/tools/links.py --check \
  cpp/gateway/tests/rt/fetch_connection_test.cpp \
  cpp/gateway/tests/rt/websocket_lifetime_test.cpp
python3 -c 'import ast,pathlib; ast.parse(pathlib.Path("cpp/gateway/tests/rt/run_checks.py").read_text())'
bun --no-env-file run format:check cpp/gateway/docs/findings/cppgw-asan-rt-20261117.md
git diff --check
```

## Review and delivery limits

Actual local review mode completed with:

```sh
timeout 180s codex review -c 'review_model="codex-auto-review"' --uncommitted
```

Its HEAD and base were both `09d44bd5b94fd6fb6d44c9c1bc9c3ebe15a48494`,
with the then-uncommitted runtime changes. The CLI announced
`model: ubiquity/deepseek-flash`, `provider: uos`, despite the requested
`review_model`. Preserve this identity discrepancy; no official review-backlog
receipt or exact reviewer-model match is claimed. The exact log is
`/tmp/cppgw-asan-rt-review.log`, SHA-256
`58a43be611f75b33b4b0ff03c33c215ebbcc2c91bd725411e0ae26de35b77aeb`.
No findings were ingested into an official backlog. The returned P2 was fixed
and the later clean-commit sweeps above replaced the earlier dirty receipts.
Original finding text (the CLI printed the same finding twice):

> [P2] Record the tree that the checks actually compile — /home/runner/work/_temp/agent-jobs/cppgw-asan-rt-20261117/checkout/cpp/gateway/tests/rt/run_checks.py:115-117
>
> When this runner is invoked on the current dirty checkout, it compiles the working-tree sources but records `HEAD` and `HEAD^{tree}` as the provenance. The resulting receipt therefore identifies the base commit rather than the tested changes, making it impossible to bind a pass to the actual compiled tree; record the working-tree content/tree (or fail unless the checkout is committed). This conflicts with [AGENTS.md:183](AGENTS.md#L183).

Manual publication was attempted after both committed-source gates passed:

```sh
GIT_TERMINAL_PROMPT=0 GIT_ASKPASS=/bin/false \
  git push --set-upstream origin test/cppgw-asan-rt-20261117
```

It failed with exit 128: `fatal: could not read Username for 'https://github.com': terminal prompts disabled`.
GitHub CLI also returned exit 4 asking for `GH_TOKEN`. No credential was printed,
written, replaced or refreshed. The task remains on the clean owned branch
`test/cppgw-asan-rt-20261117` for the runner's promised publication on exit.
Independent push verification, PR creation, merge and a `development` handoff
could not be completed from this unauthenticated checkout. The parent port owner /
runner publisher owns that next step. The base includes two pre-existing port
commits outside `development`, including production-build work marked in progress;
this task does not accept or merge that unrelated work. No hosted workflow was
triggered manually and no production or Mac service was activated.

No new defects were found outside the owned paths. The 34 original runtime
binaries and the two new regression binaries were all built and tested. The
larger gateway suite and other compiler/platform/deployment qualifications remain
with their existing owners.

Final result: Clang 21 ASan/UBSan and plain `-O2` both pass 36 binaries / 452 cases.
Fixed the pending fetch read UAF, WebSocket teardown cycles, retry self-reference
and unpublished-handshake cleanup; six new lifetime cases pass. Native host
expectations are backed by Bun 1.4.0. Exact reproduction commands, every changed
source, initial failures and final receipts are above. Publication/integration is
pending the runner publisher; there are no remaining runtime test failures or
outside-owned-path defects to report.
