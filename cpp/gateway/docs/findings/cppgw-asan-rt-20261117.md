# Runtime sanitizer findings — 2026-11-17

Runtime validation passes: **36 binaries / 452 cases in each profile**, zero
failures, timeouts, sanitizer reports, or suppressions. The original 34 binaries
contain 446 cases; two new lifetime suites add six cases. Commit-bound final
receipts and publication status are recorded after the source checkpoint.

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
