# Clang/libc++ portability findings, 2026-11-17

## Contract v1

Objective: compile every `src/rt`, remaining `src`, and `tests` C++ translation unit with recent Clang,
libc++, C++20 and `-Wall -Wextra -Wshadow -Wconversion -Werror`; link and execute every `tests/rt` binary.
Audit Linux dependencies and document exact macOS prerequisites, commands and remaining limits.

Invariants: preserve the TypeScript/Bun 1.4.0 oracle, all assertions, coroutine lifetime rules and module
structure. Only mechanical compile-portability changes and clearly correct guarded platform branches.
No deployment, live credentials, production state, provider traffic, test weakening or unrelated formatting.

Owned: `cpp/gateway/{include,src,tests}` for portability, this findings file and `docs/MACOS.md`.
Excluded: `src/rt/{json,url,regex*}.cpp` and their headers, all other repository paths, build tools and CI.
Failures in excluded paths are reported with commands and first diagnostics instead of edited.

Matrix: Linux x86_64 Clang/libc++ object compilation and runtime execution are required; Linux libstdc++
is a diagnostic comparison only; Apple Silicon/Apple SDK compilation and execution remain unverified.
Linux confinement remains real and unsupported platforms fail closed, matching the TypeScript callers.

Acceptance: full source/test object census, all runtime binaries and real case counts, changed-document
Prettier check, `git diff --check`, exact source/tree identity, committed and pushed task branch.
No unrelated TypeScript, Rust, deployment or whole-repository verification gates are selected.

Long operations: four compile workers, artifacts and full logs under `/tmp/cppgw-libcxx-20261117`,
per-translation-unit commands/status and periodic progress. Expect 10–30 minutes per complete phase;
on failure retain diagnostics, fix owned root causes, and rebuild affected inputs. Stop only with exact
terminal results or documented ownership/platform blockers; never count an unbuilt unit as passing.

## Discovery checkpoint

- Starting commit: `09d44bd5` (`agent-job`); initial checkout clean.
- Read `PORTING.md`, `docs/PORT-NOTES.md`, `docs/TESTING.md`, `docs/BUILD.md`, `docs/OPEN-STAGES.md`
  and the existing macOS draft. Production-host restrictions are overridden by this task's disposable-runner authorization.
- Runner: Ubuntu 26.04.1 x86_64, four CPUs, Bun 1.4.0. Clang 20/21/22 and GCC 14/15 are installed;
  libc++ and Boost development dependencies still require preparation.
- `tools/links.py` resolves its gateway root relative to its own file; normal source-list resolution is
  usable directly. Its cached symbol-closure mode depends on the old host tools and is unnecessary here.
- `gh auth status` reports no authenticated GitHub host. Delivery capability will be checked without
  exposing or persisting environment credentials.

## Results and changes

Local implementation and verification are complete. Publishing and integration remain blocked by
the runner's missing GitHub write authentication; Mac execution remains unverified.

- Installed libc++/libc++abi 22.1.2, Boost 1.90 headers and compression development dependencies.
  `libboost-system1.90-dev` does not exist in this distribution; Boost.System is header-only here.
- Actual census: 44 runtime + 327 other production sources; 459 test/support sources, comprising
  437 standalone test binaries and 22 support translation units. There are 34 runtime test binaries.
- `python3 /tmp/cppgw-libcxx-20261117/compile.py rt`: initial 41 passed / 3 failed; after mechanical
  comparison/capacity/character-conversion fixes and guarded loader/environment changes, 44 passed / 0 failed.
- First full modules pass: 314 passed / 13 failed in 377.747 seconds. Diagnostics and subsequent fixes
  concern shadowed private guards, signed lengths, an explicit numeric condition, vexing parses, and
  intentionally unused internal variant tags/ADL serializers. No warning flags or assertions weakened.
- Bun oracle command (empty HOME and environment, no dotenv or automatic installs):
  `env -i PATH=/home/runner/.bun/bin:/usr/bin:/bin HOME=/tmp/cppgw-libcxx-20261117/oracle-home bun --no-env-file --no-install -e 'console.log(JSON.stringify({platform:process.platform,arch:process.arch,version:Bun.version}));'`
  returned `{"platform":"linux","arch":"x64","version":"1.4.0"}`. The original process fixture's
  unconditional `arm64` assertion is host-specific; it now selects the actual target architecture.
- Runtime linkage uses Boost.JSON and Boost.Container 1.90 rebuilt with Clang/libc++, avoiding the
  distribution's libstdc++ C++ ABI. Container source tag `boost-1.90.0`, commit
  `29323399657194e8968656ab85b47139ca722ec2`; JSON uses the installed matching `boost/json/src.hpp`.
- The existing native SQLite fixture requires exactly 3.54.0. Built the official
  `https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip` under `/tmp` instead of changing any
  versioned expectations. Archive SHA-256:
  `68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d`.
- Apple XNU's public `bsd/sys/syslimits.h` lacks `HOST_NAME_MAX`; `bsd/sys/param.h` defines
  `MAXHOSTNAMELEN` as 256. Added the guarded header/buffer spelling in `rt::os::hostname`.
  This is a source-header audit, not an Apple SDK compile receipt.
- Full module reconciliation passes: 327 passed / 0 failed.
- Final reconciliation passes all **830 translation units**: **44/44 runtime**, **327/327 other source**,
  **459/459 test/support**. Runtime linkage: **34/34 passed**. Final runtime execution: **34/34 binaries**,
  **446 run / 446 passed / 0 failed**, zero skips and timeouts. These are 34 actual runtime files, not 33.
- Additional checks: legacy loader profile **44/44 runtime compilations**; the existing
  `UOS_GATEWAY_TEST_POSIX_CHILD_WATCHER` branch compiles, links and passes **10/10 process cases**
  in a private Linux network namespace. Those 10 cases overlap the primary count and are not added to it.
- `python3 cpp/gateway/tools/links.py --check`: all 437 test manifests resolve, zero skips/errors.

## Outside-owned findings and delivery blockers

1. The existing build interface cannot configure directly on this runner:
   `cmake -S cpp/gateway -B /tmp/cppgw-libcxx-20261117/cmake-probe -G Ninja` reports:

   ```text
   CMake Error at CMakeLists.txt:3 (message):
     Configure through cpp/gateway/tools/build.py so resource bounds and SDK
     identity are recorded
   ```

   `cmake/Profile.cmake` also hard-codes `/usr/lib/libboost_{json,container}.a`, a host tool root
   and mold/ELF flags. The driver/test supervisor uses `/proc`, Linux namespaces and host paths.
   These tools are outside owned paths and unchanged. The task-authorized direct compiler driver
   bypasses them only on this disposable runner; `docs/MACOS.md` supplies an external build recipe.

2. `runtime/startup.cpp` (outside `src/`, not owned) rejects artifacts that are not Linux ARM64 ELF
   at line 145, requires `linux-arm64.node` at line 360, and unconditionally applies Linux
   permissions at line 320. A Mac gateway launcher would refuse startup even after source objects
   compile. This is a source audit, not an attempted service launch. A Darwin Mach-O/dependency
   manifest and real Seatbelt launcher remain the runtime owner's work; no unrestricted fallback.
3. Delivery preflight:
   `git push --dry-run origin HEAD:refs/heads/build/cppgw-libcxx-portability-20261117` fails with
   `fatal: could not read Username for 'https://github.com': No such device or address`.
   `gh auth status` likewise has no authenticated host. No token was printed or written.
   The completed commit must be published by the enclosing job's authorized push mechanism;
   this runner cannot create/merge a PR or prove `origin/development` integration without GitHub write authentication.

## Exact build and execution commands

All commands ran from the repository root. Output root: `/tmp/cppgw-libcxx-20261117`; four compiler
workers, `-O0 -g0`, no sanitizers. No build directory, binary, corpus, archive or dependency install was
placed inside the checkout. No TypeScript application, Rust, hosted CI or deployment gate was run.

Dependency preparation:

```sh
sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends libc++-22-dev libc++abi-22-dev libboost-json1.90-dev libboost-container1.90-dev libssl-dev libsqlite3-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev
git clone --depth 1 --branch boost-1.90.0 https://github.com/boostorg/container.git /tmp/cppgw-libcxx-20261117/boost-container
curl --fail --silent --show-error --location https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip -o /tmp/cppgw-libcxx-20261117/sqlite-3540000.zip
unzip -q /tmp/cppgw-libcxx-20261117/sqlite-3540000.zip -d /tmp/cppgw-libcxx-20261117
clang-22 -O2 -fPIC -DSQLITE_THREADSAFE=1 -DSQLITE_DQS=3 -c /tmp/cppgw-libcxx-20261117/sqlite-amalgamation-3540000/sqlite3.c -o /tmp/cppgw-libcxx-20261117/deps/sqlite3.o
python3 /tmp/cppgw-libcxx-20261117/boost-build.py
```

`boost-build.py` compiles a one-line `#include <boost/json/src.hpp>` translation unit and each
`boost-container/src/*.cpp` with
`clang++-22 -std=c++20 -stdlib=libc++ -O2 -DBOOST_CONTAINER_STATIC_LINK -DBOOST_CONTAINER_NO_LIB -c SOURCE -o OBJECT`.
It compiles `boost-container/src/alloc_lib.c` with `clang-22 -O2 -c SOURCE -o OBJECT`, then runs
`ar rcs /tmp/cppgw-libcxx-20261117/deps/libboost_libcxx.a OBJECTS...`. All eight dependency objects
passed. The distribution's Boost C++ binaries were not linked.

The temporary compiler driver discovers `src/rt/*.cpp`, all other `src/**/*.cpp`, then all
`tests/**/*.cpp`, uses four workers and compiles each unit with the following exact argv template:

```sh
clang++-22 -std=c++20 -stdlib=libc++ -O0 -g0 -Wall -Wextra -Wshadow -Wconversion -Werror \
  -DBOOST_JSON_NO_LIB -DBOOST_CONTAINER_NO_LIB -DBOOST_SYSTEM_NO_LIB -DUOS_GATEWAY_NATIVE_LIBRARIES \
  -I "$UOS_GATEWAY_ROOT/include" -I "$UOS_GATEWAY_ROOT" -pthread -ferror-limit=4 \
  -MMD -MF "$UOS_OUTPUT/obj/${unit%.cpp}.d" -c "$UOS_GATEWAY_ROOT/$unit" -o "$UOS_OUTPUT/obj/${unit%.cpp}.o"
```

Here `UOS_GATEWAY_ROOT` is the absolute checkout path plus `/cpp/gateway`, `UOS_OUTPUT` is the output
root above, and `unit` is a gateway-relative source path. Each object has `.log`, `.d` and `.json`
receipts containing the exact expanded argv, status, elapsed time and first-party dependency SHA-256s.
Successful objects are reused only when the command and every consumed input hash still match.
No source-bearing compile is silently skipped or counted from an invalidated object.
[MACOS.md](../MACOS.md) contains a self-contained equivalent object driver for reproduction.

```sh
python3 /tmp/cppgw-libcxx-20261117/compile.py rt
python3 /tmp/cppgw-libcxx-20261117/compile.py modules
python3 /tmp/cppgw-libcxx-20261117/compile.py tests
python3 /tmp/cppgw-libcxx-20261117/link-rt.py
python3 /tmp/cppgw-libcxx-20261117/compile-legacy.py rt
```

The legacy driver omits only `-DUOS_GATEWAY_NATIVE_LIBRARIES` and writes a separate `legacy/obj`
tree. The runtime link driver uses four workers and this argv template for each runtime test:

```sh
clang++-22 -stdlib=libc++ -pthread "$UOS_OUTPUT/obj/tests/rt/${test}.o" \
  "$UOS_OUTPUT/obj/src/rt/"*.o "$UOS_OUTPUT/deps/libboost_libcxx.a" "$UOS_OUTPUT/deps/sqlite3.o" \
  -lssl -lcrypto -lz -lbrotlidec -lzstd -licuuc -licui18n -ldl -o "$UOS_OUTPUT/bin/$test"
```

`ldd` confirms `libc++.so.1` and `libc++abi.so.1`. System ICU also imports libstdc++ internally;
only its C API crosses into first-party code. First-party C++ and both Boost components use libc++.
The native SQLite object is 3.54.0; the distribution's older SQLite is not used by the runtime run.

Execution commands:

```sh
sudo -n unshare --net -- /usr/bin/python3 /tmp/cppgw-libcxx-20261117/run-rt.py 1001 1001
sudo -n unshare --mount --net -- /usr/bin/python3 /tmp/cppgw-libcxx-20261117/run-tmpfs.py
```

The first command records the ordinary-filesystem baseline. The second mounts a private 512 MiB
tmpfs on `/tmp` and binds the original artifact directory back into it, checking its inode identity.
It checks the Bun timestamp oracle and invokes the same runner. The runner brings up loopback,
proves external-route denial with `192.0.2.1:443` returning `ENETUNREACH` (101), drops each test to
UID/GID 1001 with no supplementary groups, uses a new short `/tmp/ux.*` HOME/TMPDIR/cwd per binary,
and passes only PATH, HOME, TMPDIR, LC_ALL and TZ. Each test has its own process group and a
280-second timeout. The final run used no timeout or skip. No credential environment is inherited.

The POSIX child-watcher control recompiles only `src/rt/process_loop.cpp` with the same primary flags
plus `-DUOS_GATEWAY_TEST_POSIX_CHILD_WATCHER`, then relinks `process_test` replacing that one runtime
object. Its isolated execution was:

```sh
sudo -n unshare --net -- /bin/sh -c 'ip link set lo up && cd /tmp/cppgw-libcxx-20261117/posix-child/w && exec setpriv --reuid=1001 --regid=1001 --clear-groups env -i PATH=/usr/bin:/bin HOME=/tmp/cppgw-libcxx-20261117/posix-child/h TMPDIR=/tmp/cppgw-libcxx-20261117/posix-child/t LC_ALL=C TZ=UTC /tmp/cppgw-libcxx-20261117/posix-child/process_test'
```

## Initial failures and oracle evidence

- Initial object passes: runtime **41/44**, modules **314/327**, test/support **448/459**.
  All compiler failures were fixed within owned paths. First-pass logs retain the original diagnostics;
  no warning group was disabled. Further passes exposed additional occurrences of the same vexing
  parse or shadowing after the four-error diagnostic limit; final counts include their fixes.
- Initial runtime baseline: **33/34 binaries passed**, **446 run / 444 passed / 2 failed**.
  Both failing cases were in `rt/fs_test.cpp`. One asserted ARM64's `O_NOFOLLOW == 32768`; the
  x86_64 implementation correctly reported 131072. The other expected enormous timestamps on a
  filesystem that clamps them. No libc++ ordering, numeric, locale, JSON, regex or sort mismatch
  was found by the runtime suite.
- Oracle command (fresh HOME, no dotenv/auto-install), expanded script:

  ```sh
  env -i PATH=/home/runner/.bun/bin:/usr/bin:/bin HOME=/tmp/cppgw-libcxx-20261117/oracle-home \
    bun --no-env-file --no-install -e '
  import * as fs from "node:fs";
  const file="/tmp/cppgw-libcxx-20261117/oracle-times";
  fs.writeFileSync(file,"x");
  console.log(JSON.stringify({platform:process.platform,arch:process.arch,O_NOFOLLOW:fs.constants.O_NOFOLLOW}));
  for (const pair of [[new Date(8.64e15),new Date(-8.64e15)],[9.2e18,1],[1e19,1e300]]) {
    fs.utimesSync(file,pair[0],pair[1]);
    const s=fs.statSync(file);
    console.log(JSON.stringify({atimeSeconds:s.atimeMs/1000,mtimeSeconds:s.mtimeMs/1000}));
  }
  fs.unlinkSync(file);'
  ```

  Exact output on the ordinary runner filesystem (`stat -f -c '%T' /tmp`: `ext2/ext3`):

  ```text
  {"platform":"linux","arch":"x64","O_NOFOLLOW":131072}
  {"atimeSeconds":15032385535,"mtimeSeconds":-2147483648}
  {"atimeSeconds":15032385535,"mtimeSeconds":1}
  {"atimeSeconds":15032385535,"mtimeSeconds":15032385535}
  ```

  C++ returned the same clamped raw timestamps. On the private tmpfs the Bun Date-pair probe returned
  `{"atimeSeconds":8640000000000,"mtimeSeconds":-8640000000000}`; every original timestamp assertion
  then passed unchanged. The only numeric expectation added is the Bun-proven Linux x86_64 flag;
  the original ARM64 branch remains intact.

- An initial tmpfs launcher preflight ran **zero tests**: `mount` canonicalized the retained directory's
  `/proc/self/fd` source into the newly empty mount, hiding the runner. It failed with
  `FileNotFoundError: .../run-rt.py`. The corrected bind uses `--no-canonicalize` and checks inode and
  runner-file identity before testing. The failed preflight log remains `run-rt-tmpfs-preflight.log`.
- Final primary runtime result: **34 binaries passed, 446 cases passed, zero failures/skips/timeouts**.
  Final numeric, ordered-container, regex, JSON, complete-Unicode IDNA, TLS, transport, WebSocket,
  subprocess and SQLite groups all passed; this does not assert untested Darwin behavior.

## Source changes and evidence

All paths below are relative to `cpp/gateway`. There are 22 changed production sources and 19 changed
C++ test/support sources. No header, module path, public API, assertion case or wire value was removed.
Local identifiers changed only where required by Clang's shadow diagnostic. `[[maybe_unused]]` marks
specific intentional variant tags, optional ADL serializers or registration side effects; it does not
suppress a warning group. Every row is covered by the final strict object gate. Runtime rows and runtime
fixtures are additionally covered by the 446-case run, with guarded Darwin branches remaining unexecuted.

| Path                                                        | Minimal change and original diagnostic/audit evidence                                                                                                                                    |
| ----------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/auth/passkeys.cpp`                                     | Make nonnegative UTF-16 lengths explicit at unsigned CBOR/index boundaries; `-Wsign-conversion` at original lines 604/826.                                                               |
| `src/authority/authority_rpc.cpp`                           | Name the private handle/account/proof guards distinctly from method parameters and locals; `-Wshadow` at 308/326/357 and sibling methods. Guard definitions and wire keys are unchanged. |
| `src/authority/durable_store.cpp`                           | Rename the local encoded-state string to avoid the private `text` validator; `-Wshadow` at 190.                                                                                          |
| `src/authority/native_authority_contracts.cpp`              | Mark six intentional, translation-unit-local `value_from` ADL overloads `[[maybe_unused]]`; `-Wunneeded-internal-declaration` at 7/11/92/103/113/124. Keep all codecs.                   |
| `src/cache/kv_migration_projection.cpp`                     | Explicitly convert nonnegative string length to the unsigned index type; `-Wsign-conversion` at 110.                                                                                     |
| `src/codex/auth_refresh.cpp`                                | Use braces for two named RequestInfo objects; qualified parameter-declarator / `-Wvexing-parse` at 213/562. Coroutine argument lifetimes remain named.                                   |
| `src/codex/banked_reset_claim.cpp`                          | Retain four internal variant `kind` tags with `[[maybe_unused]]`; `-Wunused-const-variable`.                                                                                             |
| `src/codex/banked_reset_pool.cpp`                           | Same for eight internal variant tags; `-Wunused-const-variable`.                                                                                                                         |
| `src/codex/banked_reset_submission.cpp`                     | Same for five internal variant tags; `-Wunused-const-variable`.                                                                                                                          |
| `src/deepseek/chat_projection.cpp`                          | Compare the finite unreadable-part counter explicitly with zero; `-Wfloat-conversion` at 304.                                                                                            |
| `src/kernel/quota_reserve.cpp`                              | Retain four internal variant tags with `[[maybe_unused]]`; `-Wunused-const-variable`.                                                                                                    |
| `src/provider/anthropic_responses.cpp`                      | Retain two internal variant tags with `[[maybe_unused]]`; `-Wunused-const-variable`.                                                                                                     |
| `src/provider/lithos.cpp`                                   | Retain two internal variant tags with `[[maybe_unused]]`; `-Wunused-const-variable`.                                                                                                     |
| `src/rt/async.cpp`                                          | Remove the redundant internal `operator!=`; C++20 rewrites it through the unchanged `operator==`. Original `-Wunneeded-internal-declaration` at 207.                                     |
| `src/rt/env.cpp`                                            | Guard Darwin's `_NSGetEnviron()` accessor instead of a direct `environ` reference; platform API audit. Linux environment semantics unchanged.                                            |
| `src/rt/fetch_transport_codec.cpp`                          | Guard legacy Brotli/Zstd dylib names on Apple; `.so.1` was ELF-specific. Native direct bindings unchanged.                                                                               |
| `src/rt/fetch_transport_net.cpp`                            | Initialize both slot arrays from the existing capacity member; original `-Wunused-private-field` on `capacity_`. Same stored capacity, layout and slot counts.                           |
| `src/rt/gzip.cpp`                                           | Guard `libz.1.dylib` as Apple's legacy default; Linux retains `libz.so.1` and native direct binding behavior.                                                                            |
| `src/rt/process.cpp`                                        | Guard `<sys/param.h>` and `MAXHOSTNAMELEN` on Apple; public Apple XNU headers lack Linux's `HOST_NAME_MAX`.                                                                              |
| `src/rt/process_loop.cpp`                                   | Guard `_NSGetEnviron()` for inherited spawn environments on Apple. Existing SIGCHLD/close-on-exec branches retained and POSIX control passed.                                            |
| `src/rt/sqlite.cpp`                                         | Guard `libsqlite3.dylib` for Apple fallback loading and corresponding error text; Linux text and native direct bindings unchanged.                                                       |
| `src/rt/tls.cpp`                                            | Explicitly cast string bytes to unsigned for the existing hostname range check; `-Wsign-conversion` at 71.                                                                               |
| `tests/anthropic_responses_test.cpp`                        | Use braces for three named Response objects; qualified parameter-declarator / `-Wvexing-parse` at 332/354/365.                                                                           |
| `tests/codex_banked_reset_daily_capacity_incident_test.cpp` | Rename two local candidate vectors to avoid an imported `pool` fixture; `-Wshadow` at 97/135.                                                                                            |
| `tests/config_differential_test.cpp`                        | Apple self-reexecution uses `process::execPath()`; Linux keeps SDK-loader-aware `/proc/self/cmdline`.                                                                                    |
| `tests/jev_codex_smoke_test.cpp`                            | Guard existing GCC-only diagnostic pragmas from Clang; `-Wunknown-warning-option` on `-Wsubobject-linkage`.                                                                              |
| `tests/jev_compaction_smoke_test.cpp`                       | Same compiler guard; real implementation include and every assertion remain.                                                                                                             |
| `tests/kv_durable_interop_test.cpp`                         | Guard Apple self-reexecution as above; Linux loader arguments preserved.                                                                                                                 |
| `tests/kv_durable_suite_test.cpp`                           | Promote bytes explicitly to unsigned before base64 shifts; `-Wsign-conversion` at 204/208/213.                                                                                           |
| `tests/local_admin_auth_test.cpp`                           | Guard Apple self-reexecution as above; Linux loader arguments preserved.                                                                                                                 |
| `tests/native_kernel_conformance_test.cpp`                  | Spell three intentional failure assertions as `false && "message"`; `-Wstring-conversion` on `!"message"`. They still always fail on the same path and retain the messages.              |
| `tests/production_smoke/serve_failure_gate.cpp`             | Guard Apple socket creation plus checked `fcntl(FD_CLOEXEC)`; Linux retains `SOCK_CLOEXEC`. Close the descriptor on flag failure.                                                        |
| `tests/rt/fs_test.cpp`                                      | Use native stat timestamp field accessors on Apple/Linux; add only Bun-proven x86_64 `O_NOFOLLOW == 131072`, retaining ARM64's literal. Original timestamp expectations unchanged.       |
| `tests/rt/kv_sqlite_test.cpp`                               | Guard Apple self-reexecution as above; Linux loader arguments preserved.                                                                                                                 |
| `tests/rt/kv_test.cpp`                                      | Same unsigned-before-shift fix in the original base64 helper; `-Wsign-conversion` at 94/98/103.                                                                                          |
| `tests/rt/number_test.cpp`                                  | Make the byte conversion explicit in the existing FNV/BigInt case; `-Wsign-conversion` at 2356. Recorded hash remains unchanged.                                                         |
| `tests/rt/process_test.cpp`                                 | Assert target platform/architecture rather than always Linux ARM64; add independent dyld executable-path expectation for Apple. Linux x86_64 values confirmed under Bun.                 |
| `tests/rt/websocket_test.cpp`                               | Explicit unsigned-byte conversion in the existing Latin-1 wire fixture; `-Wsign-conversion` at 335.                                                                                      |
| `tests/serve_test.cpp`                                      | Type the retained no-argument entrypoint function pointer explicitly; original `auto` initializer cannot select between current overloads. No startup invoked.                           |
| `tests/sqlite_readonly_test.cpp`                            | Mark the side-effect-only Darwin test registration variable `[[maybe_unused]]`; `-Wunused-const-variable` at 209. Registration is preserved.                                             |
| `tests/storage_bun_kv_test.cpp`                             | Guard Apple self-reexecution as above; Linux loader arguments preserved.                                                                                                                 |

`docs/MACOS.md` was rewritten with exact Apple prerequisites, ABI-matching dependency preparation,
feature probe, complete object and runtime-link commands, all added platform switches and remaining
Mac acceptance. This findings file records the contract, commands, original failures and final evidence.
No source or header belonging to the JSON/URL/regex owner was edited; all of those runtime units compile
and their runtime test groups pass in the final Linux profiles.

## Final per-binary runtime counts

| Runtime binary            | Run | Passed | Failed |
| ------------------------- | --: | -----: | -----: |
| `abort_test`              |  13 |     13 |      0 |
| `async_test`              |  32 |     32 |      0 |
| `base64_test`             |   6 |      6 |      0 |
| `clock_test`              |  11 |     11 |      0 |
| `console_test`            |   9 |      9 |      0 |
| `crypto_test`             |  18 |     18 |      0 |
| `date_test`               |   6 |      6 |      0 |
| `env_test`                |   6 |      6 |      0 |
| `error_test`              |   4 |      4 |      0 |
| `fetch_test`              |   9 |      9 |      0 |
| `fetch_transport_test`    |  33 |     33 |      0 |
| `fs_test`                 |  10 |     10 |      0 |
| `gzip_test`               |  10 |     10 |      0 |
| `harness_test`            |   3 |      3 |      0 |
| `headers_test`            |   4 |      4 |      0 |
| `http_test`               |   6 |      6 |      0 |
| `json_test`               |  24 |     24 |      0 |
| `kv_js_test`              |   8 |      8 |      0 |
| `kv_jsc_deserialize_test` |   7 |      7 |      0 |
| `kv_sqlite_test`          |  17 |     17 |      0 |
| `kv_test`                 |  53 |     53 |      0 |
| `nullable_test`           |   6 |      6 |      0 |
| `number_test`             |  21 |     21 |      0 |
| `ordered_test`            |   9 |      9 |      0 |
| `process_test`            |  10 |     10 |      0 |
| `record_test`             |   6 |      6 |      0 |
| `regex_test`              |  16 |     16 |      0 |
| `sqlite_test`             |  29 |     29 |      0 |
| `stream_test`             |  10 |     10 |      0 |
| `string_test`             |  19 |     19 |      0 |
| `text_codec_test`         |   6 |      6 |      0 |
| `url_idna_test`           |   2 |      2 |      0 |
| `url_test`                |   6 |      6 |      0 |
| `websocket_test`          |  17 |     17 |      0 |

The separate POSIX child-watcher control passes 10/10 overlapping process cases.

## Source identity and artifact evidence

- Compiler: Ubuntu clang version 22.1.2 (1ubuntu1); binary SHA-256
  `12cdaaad24ccbc8e42a2262e7513acaf6a15046061f4b3212391d4d9b9fd7688`.
- All 830 primary object receipts and 1359 unique consumed first-party inputs
  were reconciled against their current SHA-256 values after the final C++ edit.
- Sorted consumed-input manifest SHA-256: `009f2894036a7ba723f3307c3077d7c89b227740d28c196f62a1ce25ebd6a48a`.
- Exact expanded commands, dependency hashes, object logs, dependency archive inputs and run logs
  remain under `/tmp/cppgw-libcxx-20261117`. The repository report retains the essential commands,
  original diagnostics, oracle observations and terminal counts; disposable-runner artifacts are not published binaries.

## Remaining work and handoff

- No remaining owned-source compile failure or primary runtime test failure. JSON/URL/regex exclusions
  required no owner fix. No libc++-specific runtime behavior difference was observed in the executed cases.
- Darwin compilation/execution, loader resolution, Apple filesystem/signal/errno behavior, actual kqueue,
  dependency packaging and a real Seatbelt launcher/test boundary remain unverified. The existing Linux
  startup/package and supervision blockers above must be handled by their owners.
- Linux-only production-smoke Python readers (`tests/production_smoke/{network,process,driver}.py`)
  also use `/proc`/namespaces; changing their runtime isolation is outside compile-portability scope.
- No production gateway, live database, credential source, external inference provider, Mac or Pi was accessed.
- Push/PR/ancestry-preserving development integration must be performed by the enclosing authenticated job.
  No hosted CI run was dispatched. The checkout remains on its owned `agent-job` branch for that handoff.
- Local review and final commit/tree receipts are recorded in the documentation follow-up below.

## Final commit, checks and review receipt

Implementation commit: `27ddd088c3b0a2e5ede5698a75a13b8e4ea28435`.
Tree: `f93b36dc44ed81e03394132e45f2891b4732d257`.
Parent/base: `09d44bd5b94fd6fb6d44c9c1bc9c3ebe15a48494`.
The documentation follow-up changes only `docs/MACOS.md` and this report; every consumed C++ input
remains identical to the implementation commit. Its Mac recipe explicitly selects the matching
Homebrew libc++ headers as well as libraries, instead of relying on the compiler's default SDK search.

On the committed implementation tree, the three compiler-driver gates above reconciled **44/44**,
**327/327**, and **459/459** valid objects, zero failures. These were content-checked cache reuses;
the earlier full compilation and runtime execution receipts remain bound to exactly those inputs.
No source edit followed the final successful runtime run.

Changed-path verification maps the 41 C++ paths in the source table to the warning-fatal object gate
and 34 runtime binaries. The two changed Markdown paths use the locked Prettier 3.9.6 and whitespace gate.
Quality dependencies were prepared outside the checkout, with both repository locks present and unchanged:

```sh
mkdir -p /tmp/cppgw-libcxx-20261117/lint
cp tools/lint/package.json tools/lint/bun.lock /tmp/cppgw-libcxx-20261117/lint/
bun --no-env-file install --frozen-lockfile --ignore-scripts --cwd /tmp/cppgw-libcxx-20261117/lint
bun --no-env-file --no-install /tmp/cppgw-libcxx-20261117/lint/node_modules/prettier/bin/prettier.cjs --check cpp/gateway/docs/MACOS.md cpp/gateway/docs/findings/cppgw-libcxx-portability-20261117.md
git diff --check
```

Both changed documents pass formatting; whitespace checks pass. No C++ formatter or unrelated
TypeScript lint/check/test aggregate was run. A Python cache generated while importing the Links
resolver was removed before staging; no generated artifact remains in the checkout.

Actual local review command, Codex CLI 0.169.0:

```sh
codex -c 'review_model="codex-auto-review"' review --commit 27ddd088c3b0a2e5ede5698a75a13b8e4ea28435
```

The CLI exited 0 after 69.762 seconds, without timeout or credential redaction. However, its banner
reported `model: ubiquity/deepseek-flash`, `provider: uos`, rather than confirming the requested
`codex-auto-review`. **Model identity is unverified: reject this receipt for official review ingestion.**
No finding was ingested, no failed/identity-mismatched review was marked complete, and review status
was not used as a delivery gate. The exact transcript remains `/tmp/cppgw-libcxx-20261117/review.log`.
Its SHA-256 is `716e3d443c17cd0117edbdb88d82f0e353fb9ea9fa779921ced041cfa611c432`.

Preserved original final review text, as unaccepted evidence:

```text
The portability changes contain a platform-specific filesystem assertion that is incorrect on macOS and will fail the test suite there.

Review comment:

- [P1] Use the Darwin value for O_NOFOLLOW — /home/runner/work/_temp/agent-jobs/cppgw-libcxx-portability-20261117/checkout/cpp/gateway/tests/rt/fs_test.cpp:532-535
  On macOS this falls into the `else` branch and expects the Linux ARM64 value `32768`, but Darwin defines `O_NOFOLLOW` as `256`. Since this test is now explicitly made compilable for Apple, the filesystem suite will fail on every macOS run; add a Darwin-specific expectation rather than grouping it with Linux ARM64.
```

The report already leaves Mac filesystem expectations unqualified; no Mac Bun oracle is available.
The new Linux x86_64 expectation is proven above. Existing Darwin signal/errno/open-flag expectations
need a real Mac oracle rather than an unobserved replacement in this compile-only job.

Final publication attempt:

```sh
git push origin HEAD:refs/heads/build/cppgw-libcxx-portability-20261117
```

It exited 128 with `fatal: could not read Username for 'https://github.com': No such device or address`.
A fresh fetch confirms `origin/development` is `002fe15fa926674c97f0169355d24929bf529710`;
`git merge-base --is-ancestor 27ddd088c3b0a2e5ede5698a75a13b8e4ea28435 origin/development` exits 1.
Therefore no push, PR, development merge or final development checkout handoff is claimed. Preserve
this clean owned `agent-job` branch for the enclosing job's authenticated publication and integration.

## Final handoff summary

Clang/libc++ 22.1.2 passes all 830 source/test compilations and all 34 runtime links;
34 runtime binaries pass 446 cases. Legacy-loader compilation passes 44/44 and the POSIX child-watcher
control passes 10/10 overlapping cases. The two initial filesystem failures are explained by Bun-proven
host constants and filesystem timestamp range, with no timestamp expectation weakened.

The source table records every mechanical compiler/guarded platform change; MACOS.md contains the
prerequisites, full build commands and platform switches. Mac execution and the out-of-scope Linux
launcher/build-tool boundaries remain open. Changes are committed locally; GitHub authentication
blocks publication/integration. The local review receipt is rejected for unverified model identity.
