# Native macOS Apple Silicon build

The November 18, 2026 requalification compiles the native gateway with Apple Clang and libc++.
It builds simdjson 5.2.0 and simdutf 9.4.0 from the vendored sources and uses Homebrew ICU 78.3 for
IDNA, case mapping and collation.

The existing `-O0 -g0` profile compiled all **373 production translation units** and linked
the gateway and two offline KV utilities as arm64 Mach-O executables. The complete test run
passed **442/442 binaries and 5,034/5,034 cases**.

See [the current findings](findings/cppgw-macos-requal-20261118.md) for exact commands, receipt
identities and diagnostics. The [previous qualification](findings/cppgw-macos-native-20261117.md)
covers the earlier platform port and the Darwin fallback loaders.

## Verified host and dependency selection

| Component         | Native job                                                            |
| ----------------- | --------------------------------------------------------------------- |
| Host              | macOS 26.7.2, build 25G332, arm64; Darwin 25.6.0                      |
| Compiler          | Apple Clang 21.0.0, clang-2100.1.1.101, Xcode 26.6                    |
| C++ library       | Apple macOS 26.5 SDK, libc++, C++20                                   |
| Boost             | Homebrew 1.92.0_1, JSON and Container, libc++ ABI                     |
| OpenSSL           | Homebrew openssl@3 3.5.9                                              |
| SQLite            | Separate official 3.54.0 dylib, matching the versioned native fixture |
| Other C libraries | zlib 1.4, ICU 78.3, Brotli 1.2.1, Zstd 1.5.7                          |
| Tools             | Homebrew 7.0.11, CMake 4.4.5, Ninja 1.13.2, Python 3.15.2             |
| Oracle            | Bun 1.4.0, revision 34cbb9a40b4bd1bd767d134a7065e66c2432a676          |

Homebrew SQLite 3.54.1 is installed but the qualified fixture run selects the separate **3.54.0**
header and dylib explicitly. The system SQLite is not selected. Bun 1.4.0 on this Mac reports
SQLite 3.51.0; the C++ native-library fixture already has separate native-version
expectations. Do not change its recorded 3.54.0 expectation to make another dependency pass.

The driver explicitly selects Homebrew `icu4c@78` (formula revision 78.3_1 on this runner). ICU 79.1
was already installed and failed the exhaustive IDNA data gate in the previous qualification.
The shared Unicode casing, collation and IDNA fixtures passed with the same results as
Linux ICU 78.3. [Linux ICU verification](findings/cppgw-std-icu-20261117.md#verification)

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

Run the commands from the repository root with a clean committed checkout.

```sh
bash cpp/gateway/tools/macos-build.sh --build-root "$UOS_MAC_BUILD/build" \
  --sqlite-prefix "$UOS_MAC_BUILD/sqlite-3.54.0" --jobs 3 --suite rt --require-clean
bash cpp/gateway/tools/macos-build.sh --build-root "$UOS_MAC_BUILD/build" \
  --sqlite-prefix "$UOS_MAC_BUILD/sqlite-3.54.0" --jobs 3 --suite all --require-clean
```

The script compiles the production sources once and builds the module archive. The three
production executables share that archive and the startup object. Darwin's whole-archive
link keeps the module registrars. Runtime tests execute before the other tests. Each test links
all 44 runtime objects and the ordered `Links` source list from `tools/links.py`, which keeps
the local stand-ins. A cached object requires matching compiler arguments and SHA-256s of its
tracked dependency closure.

The port's compiler flags include the following options.

```text
-std=c++20 -stdlib=libc++ -O0 -g0 -Wall -Wextra -Wshadow -Wconversion -Werror
-DBOOST_JSON_NO_LIB -DBOOST_CONTAINER_NO_LIB -DBOOST_SYSTEM_NO_LIB
-DUOS_GATEWAY_NATIVE_LIBRARIES -pthread
-I cpp/gateway/include -I cpp/gateway --system-header-prefix=third_party/
```

The driver resolves the prepared Homebrew dependencies with `brew --prefix` and `pkg-config`.
Installed library headers are system includes, so the port keeps fatal warnings when Apple Clang
parses ICU's headers. SQLite links directly to the prepared dylib. The vendored implementations
compile separately without the port warning flags. The driver checks their registry hashes, records
both archives and links them after the objects that use them. The JSON consumer enables
`UOS_GATEWAY_SIMDJSON`. The external logs contain the expanded compiler and linker arguments.

Every binary gets empty private HOME, fresh TMPDIR/cwd, an environment allowlist with no provider
or credential variables, a new process group and a 180-second timeout. Unix paths use short `/tmp`
scratch directories, canonicalized before assigning HOME/TMPDIR/cwd so native `/private/tmp` cwd
checks agree with the wrapper. A real Seatbelt network boundary permits loopback/Unix fixtures and rejects
external networking; a failed positive/negative boundary preflight stops execution. Descendant
groups are killed after execution or timeout. No fixture reads a live database or auth directory.
The driver sets `DYLD_LIBRARY_PATH` through `env` after `sandbox-exec`, because macOS strips it
when launching the platform boundary binary.

The external build root contains the following artifacts and receipts.

- `libuos_gateway.a`, the two vendor archives, and `obj/` with per-object compiler logs and input hashes.
- `runs/<id>/{uos-gateway,uos-kv-convert,uos-kv-proof}` and their link logs.
- `runs/<id>/provenance.json` with toolchain, dependency, input and executable identities.
- `runs/<id>/{production,runtime,gateway}-compile.json` with per-source build outcomes.
- `runs/<id>/results.json` and per-binary logs with harness counts and failures.

Dirty development runs have null acceptance commit/tree, with HEAD retained separately. A final
receipt must have a clean committed tree, unchanged identity and unchanged source inputs. Build
failures, missing harness summaries, empty selections and timeouts are failures, never passes.
Use `--suite none` for the production build alone, `--filter substring[,substring]` for a focused
rerun and `--case substring` for a single harness group; those are subset receipts.

The fallback bindings can be checked separately, using the same prepared dependencies:

```sh
bash cpp/gateway/tools/macos-build.sh --build-root "$UOS_MAC_BUILD/build" \
  --sqlite-prefix "$UOS_MAC_BUILD/sqlite-3.54.0" --suite rt \
  --jobs 3 --filter gzip,sqlite,fetch_transport --legacy-loaders --require-clean
```

This recompiles only `gzip.cpp`, `sqlite.cpp` and `fetch_transport_codec.cpp` without the direct-library
define, while keeping the native-version test fixtures and ICU path. It exercises `libz.1.dylib`,
`libsqlite3.dylib`, `libbrotlidec.1.dylib` and `libzstd.1.dylib` through Darwin's loader. A subsequent
normal invocation restores the three native objects before creating the primary archive:

```sh
bash cpp/gateway/tools/macos-build.sh --build-root "$UOS_MAC_BUILD/build" \
  --sqlite-prefix "$UOS_MAC_BUILD/sqlite-3.54.0" --jobs 3 --suite none --require-clean
```

## Native results

| Gate                       | Result                                                                             |
| -------------------------- | ---------------------------------------------------------------------------------- |
| Production compile         | 373/373 passed (369 runtime/module sources, shared startup and three entry points) |
| Vendor compile             | simdjson and simdutf both built and archived                                       |
| Static application archive | 369 members                                                                        |
| Mach-O executable links    | 3/3 passed                                                                         |
| Test and support compile   | 464/464 passed                                                                     |
| Runtime phase              | 36/36 binaries, 458/458 cases                                                      |
| Application phase          | 406/406 binaries, 4,576/4,576 cases                                                |
| Whole suite                | 442/442 binaries, 5,034/5,034 cases                                                |

The cold standalone runtime command took 652.31 s. The full suite took 1,206.90 s after
reusing verified production and runtime test objects. The clean receipt binds commit
`b05ecda8d3727b56b791a363ec344ba2f1093230` and tree
`c1c4aa854fb7488c05aa80d7b589bc776fc717f1`. The findings record the output hashes.

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
  refuses direct configuration. The production launcher still requires a Linux ARM64 runtime
  closure, ELF native artifacts and Linux confinement. The linked gateway reached the runtime closure
  check with a private synthetic manifest and exited 1 with `runtime closure platform is unsupported`.
  Idle gateway RSS remains unmeasured.
- Packaging, production same-binary acceptance, Mac service activation, Linux/Pi regression execution,
  and other sanitizer/fuzz/performance profiles remain separate work.
