# Native macOS Apple Silicon build

This port now compiles natively on an Apple Silicon Mac. The 2026-11-17 job builds all **369 production
translation units** (325 modules and 44 runtime sources) with Apple Clang and libc++, with every port
warning fatal, and archives them as `libuos_gateway.a`. The complete suite passes **441/441 binaries
and 5,029/5,029 cases**: 36 runtime binaries / 457 cases, then 405 application binaries / 4,572 cases.
All 463 test/support translation units also compile. There are no failed, skipped or timed-out targets.

This is an offline development qualification. Production packaging, the Mac service, live database
conversion, credentials, upstream inference and deployment are separate acceptance surfaces.
See [the exact job findings](findings/cppgw-macos-native-20261117.md) for source changes and failures.

## Verified host and dependency selection

| Component         | Native job                                                                 |
| ----------------- | -------------------------------------------------------------------------- |
| Host              | macOS 26.7.2, build 25G332, arm64; Darwin 25.6.0                           |
| Compiler          | Apple Clang 21.0.0, clang-2100.1.1.101, Xcode 26.6                         |
| C++ library       | Apple SDK libc++, C++20; floating `from_chars` and `to_chars` probe passes |
| Boost             | Homebrew 1.92.0_1, JSON and Container, libc++ ABI                          |
| OpenSSL           | Homebrew openssl@3 3.5.9                                                   |
| SQLite            | Separate official 3.54.0 dylib, matching the versioned native fixture      |
| Other C libraries | zlib 1.3.2, ICU 78.3, Brotli 1.2.1, Zstd 1.5.7                             |
| Tools             | Homebrew 7.0.11, CMake 4.4.5, Ninja 1.13.2, Python 3.15.2                  |
| Oracle            | Bun 1.4.0, revision 34cbb9a40b4bd1bd767d134a7065e66c2432a676               |

Homebrew SQLite 3.54.1 is installed but the qualified fixture run selects the separate **3.54.0**
header and dylib explicitly. The system SQLite is not selected. Bun 1.4.0 on this Mac reports its
SQLite as 3.51.0; the C++ native-library fixture already has separate native-version
expectations. Do not change its recorded 3.54.0 expectation to make another dependency pass.

ICU 79.1 failed the exhaustive IDNA data gate; Bun on this Mac matches all 1088 original scalar
blocks. The driver selects Homebrew `icu4c@78` explicitly, retaining every Unicode expectation.

No Homebrew LLVM was required. Older Apple compilers/SDKs, Intel Macs and older deployment targets
are unqualified. Keep the existing Linux GCC coroutine rules even when Apple Clang compiles them.

## Prepare a clean development machine

With Xcode Command Line Tools already available, install dependencies explicitly before building:

```sh
HOMEBREW_NO_AUTO_UPDATE=1 brew install boost openssl@3 sqlite zlib icu4c@78 brotli zstd cmake ninja python pkgconf
xcrun clang++ --version
sw_vers
bun --version # must be 1.4.0 for oracle checks
```

The build driver never installs or downloads dependencies. Bun was already pinned on the job runner.
No application TypeScript compilation or TypeScript tests are needed for this C++ qualification.
Keep all dependency preparation, object files, archives, executables and logs outside the checkout.

Prepare the exact versioned SQLite dependency, checking the original archive before extraction:

```sh
export UOS_MAC_BUILD="$(mktemp -d /tmp/cppgw-macos.XXXXXX)"
curl --fail --silent --show-error --location \
  https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip -o "$UOS_MAC_BUILD/sqlite.zip"
python3 - <<'PYTHON'
from pathlib import Path
import hashlib
import os
import shutil
import subprocess
import zipfile
root = Path(os.environ["UOS_MAC_BUILD"])
archive = root / "sqlite.zip"
assert hashlib.sha256(archive.read_bytes()).hexdigest() == "68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d"
with zipfile.ZipFile(archive) as source:
    source.extractall(root)
prefix = root / "sqlite-3.54.0"
(prefix / "include").mkdir(parents=True)
(prefix / "lib").mkdir()
shutil.copy(root / "sqlite-amalgamation-3540000/sqlite3.h", prefix / "include/sqlite3.h")
subprocess.run(["xcrun", "clang", "-O2", "-dynamiclib", "-DSQLITE_THREADSAFE=1", "-DSQLITE_DQS=3",
                "-DSQLITE_ENABLE_COLUMN_METADATA=1", "-Wl,-install_name," + str(prefix / "lib/libsqlite3.dylib"),
                str(root / "sqlite-amalgamation-3540000/sqlite3.c"), "-o", str(prefix / "lib/libsqlite3.dylib")], check=True)
PYTHON
```

## Reproduce the build and tests

Run from the repository root with a clean committed checkout:

```sh
cpp/gateway/tools/macos-build.sh --build-root "$UOS_MAC_BUILD/build" \
  --sqlite-prefix "$UOS_MAC_BUILD/sqlite-3.54.0" --jobs 3 --suite rt --require-clean
cpp/gateway/tools/macos-build.sh --build-root "$UOS_MAC_BUILD/build" \
  --sqlite-prefix "$UOS_MAC_BUILD/sqlite-3.54.0" --jobs 3 --suite all --require-clean
```

The script compiles every production source once, creates the complete archive, then compiles,
links and executes runtime tests before the other tests. Tests link all 44 runtime objects directly
and the exact ordered `Links` source list resolved by `tools/links.py`, preserving local stand-ins
and static registrars. Production and test objects are shared across binaries. A successful cache
entry requires matching compiler argv and SHA-256s of its first-party dependency closure.

The compiler flags are:

```text
-std=c++20 -stdlib=libc++ -O0 -g0 -Wall -Wextra -Wshadow -Wconversion -Werror
-DBOOST_JSON_NO_LIB -DBOOST_CONTAINER_NO_LIB -DBOOST_SYSTEM_NO_LIB
-DUOS_GATEWAY_NATIVE_LIBRARIES -pthread
-I cpp/gateway/include -I cpp/gateway
```

Homebrew include/link flags come from `brew --prefix` and `pkg-config`; SQLite's selected absolute
dylib is explicit. Links include Boost.JSON, Boost.Container, OpenSSL, SQLite, zlib, Brotli, Zstd and
ICU. Darwin uses no `-ldl`. Exact expanded compiler/link argv and failures are retained in logs.
The driver bypasses host-specific Linux wrappers/presets only for an authorized disposable host.

Every binary gets empty private HOME, fresh TMPDIR/cwd, an environment allowlist with no provider
or credential variables, a new process group and a 180-second timeout. Unix paths use short `/tmp`
scratch directories, canonicalized before assigning HOME/TMPDIR/cwd so native `/private/tmp` cwd
checks agree with the wrapper. A real Seatbelt network boundary permits loopback/Unix fixtures and rejects
external networking; a failed positive/negative boundary preflight stops execution. Descendant
groups are killed after execution or timeout. No fixture reads a live database or auth directory.
The driver sets `DYLD_LIBRARY_PATH` through `env` after `sandbox-exec`, because macOS strips it
when launching the platform boundary binary.

Artifacts and receipts are under the external build root:

- `libuos_gateway.a`, `obj/`, per-object compiler logs and consumed-input hash receipts;
- `runs/<id>/provenance.json`, compiler, SDK, dependencies/dylib hashes, commit/tree and input hashes;
- `runs/<id>/{production,runtime,gateway}-compile.json`, exact build outcomes;
- `runs/<id>/results.json` and per-binary logs, with actual case counts and failure/timeout statuses.

Dirty development runs have null acceptance commit/tree, with HEAD retained separately. A final
receipt must have a clean committed tree, unchanged identity and unchanged source inputs. Build
failures, missing harness summaries, empty selections and timeouts are failures, never passes.
Use `--suite none` for the production archive only, `--filter substring[,substring]` for a focused
rerun and `--case substring` for a single harness group; those are subset receipts.

The fallback bindings can be checked separately, using the same prepared dependencies:

```sh
cpp/gateway/tools/macos-build.sh --build-root "$UOS_MAC_BUILD/build" \
  --sqlite-prefix "$UOS_MAC_BUILD/sqlite-3.54.0" --suite rt \
  --jobs 3 --filter gzip,sqlite,fetch_transport --legacy-loaders --require-clean
```

This recompiles only `gzip.cpp`, `sqlite.cpp` and `fetch_transport_codec.cpp` without the direct-library
define, while keeping the native-version test fixtures and ICU path. It exercises `libz.1.dylib`,
`libsqlite3.dylib`, `libbrotlidec.1.dylib` and `libzstd.1.dylib` through Darwin's loader. A subsequent
normal invocation restores the three native objects before creating the primary archive:

```sh
cpp/gateway/tools/macos-build.sh --build-root "$UOS_MAC_BUILD/build" \
  --sqlite-prefix "$UOS_MAC_BUILD/sqlite-3.54.0" --jobs 3 --suite none --require-clean
```

## Native results

| Gate                    | Result                              |
| ----------------------- | ----------------------------------- |
| Full production compile | 369/369 passed                      |
| Static archive          | 369 members, built                  |
| Test/support compile    | 463/463 passed                      |
| Runtime tests           | 36/36 binaries, 457/457 cases       |
| Application tests       | 405/405 binaries, 4,572/4,572 cases |
| Whole suite             | 441/441 binaries, 5,029/5,029 cases |
| Darwin fallback loaders | Execution receipt pending           |

The complete primary receipt binds clean commit `c6cd29f9e9743e37c4eca1a0313194fd3279ba9e` and
tree `8599a3a0bc292a66a664832f01ca1404d2192e3d`, with unchanged identity and every input hash.
The findings file records the exact run, artifact/receipt hashes and subsequent documentation changes.

## Proven Darwin behavior and remaining boundaries

- `process.platform` is `darwin`, architecture `arm64`; `/tmp` resolves to `/private/tmp` in cwd and
  realpaths. All 317 path expressions were evaluated under Bun; 41 host-dependent literals differ.
- Native signals are Darwin constants for `process` listeners. Bun 1.4.0's subprocess signal table
  still uses Linux names: preserve that observable quirk separately from native listener numbers.
- Open flags and errno numbers differ. Expectations contain the exact Bun-observed Darwin values,
  retaining the prior Linux literals. Fractional `utimes` precision agrees; extreme Darwin values
  saturate signed 64-bit total nanoseconds, with exact minimum/maximum endpoint checks.
- Asio 1.92 enables `SO_NOSIGPIPE` both when creating and accepting Darwin sockets. A dedicated native
  TCP/Unix check verifies the actual option, half-close behavior and `EPIPE` delivery without SIGPIPE.
- Bun's Darwin SQLite binding retains WAL sidecars by default and enables URI names; the native KV
  addon uses bundled SQLite and drains/removes sidecars on close. The port preserves both policies.
  Readonly SQLite connections prevent creation of a missing WAL through the underlying VFS while
  retaining SQLite's normal SQL timing. Stopped, sidecar-free snapshots use Bun's immutable URI form;
  live WAL inspections retain the ordinary connection. Schema, typed rows, revisions, expiry, queue,
  CAS, crash recovery and the real readonly timestamp/sidecar fences pass. Bun probes prove the
  platform expectations; the published SDK lifecycle control does not qualify the patched Mac artifact.
- TLS uses Homebrew OpenSSL default verification paths plus the existing public PEM fallback files;
  it does not import user keychain trust settings. Bun's default/bundled list contains 121 roots on
  this runner; `getCACertificates('system')` returns a different two-entry list. Local TLS fixture
  behavior is qualified separately; arbitrary keychain trust equivalence is unproved.
- `permissions_linux` correctly refuses Darwin with `Linux confinement requires Linux ARM64`, exactly
  as the TypeScript does. Do not replace real confinement with a permissive emulation.
- Existing CMake presets/tools remain Linux-host-specific. `cmake -S cpp/gateway -B <external> -G Ninja`
  refuses direct configuration. `runtime/startup.cpp` still requires Linux ARM64 ELF, Linux native
  artifacts and Linux confinement; a Mach-O/Seatbelt production launcher remains outside this task.
- Packaging, production same-binary acceptance, Mac service activation, Linux/Pi regression execution,
  and other sanitizer/fuzz/performance profiles remain separate work.
