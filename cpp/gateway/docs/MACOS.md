# macOS build readiness

The 2026-11-17 portability job uses Linux x86_64, Clang 22.1.2, libc++/libc++abi 22.1.2,
Boost 1.90, and C++20. It compiles the complete source and test inventory and links the runtime
suite against Boost rebuilt with libc++. The exact commands, counts, failures and source identity
are recorded in [the job findings](findings/cppgw-libcxx-portability-20261117.md).

The completed Linux gate passes **830/830 translation units**, **34/34 runtime links**, and
**446/446 runtime cases**. All 44 runtime sources also compile without the native-library define.
The POSIX child-watcher control passes 10 process cases on Linux; this is not Darwin execution.

No Apple SDK, Apple Silicon machine, Darwin binary or Mac service was exercised. Linux libc++
acceptance checks the language and standard-library boundary; it does not check Darwin headers,
linker behavior, dependency install names, filesystem semantics, kqueue or Seatbelt.

## Prerequisites on an Apple Silicon development machine

Use a prepared development machine, with Xcode Command Line Tools, Python 3, CMake, Ninja,
Boost with JSON and Container, OpenSSL 3, SQLite, zlib, ICU, Brotli and Zstd. All C++ dependencies
must use the selected libc++ ABI. The current native SQLite fixture records **3.54.0** explicitly;
a different SQLite version is a separate qualification result, not permission to change its expectations.
The Linux job used ICU 78.2. Keep downloads, dependency builds and outputs outside the checkout.
These preparation commands are not application startup steps:

```sh
xcode-select --install
brew install llvm cmake ninja python pkgconf openssl@3 sqlite zlib icu4c brotli zstd
```

Select Homebrew LLVM **22 or newer**, including its matching libc++ headers and libraries. The
verified compiler/library pair is 22.1.2. Apple Clang is an alternative only after its actual SDK/libc++
passes the feature probe and complete object gate below. Older SDK libraries can lack floating-point
`std::from_chars`/`std::to_chars`; a C++20 compiler version alone does not establish library support.
No alternate numeric parser or formatting fallback was needed with libc++ 22.

Run from the repository root:

```sh
export LLVM_PREFIX="$(brew --prefix llvm)"
export PATH="$LLVM_PREFIX/bin:$PATH"
export CXX="$LLVM_PREFIX/bin/clang++"
export SDKROOT="$(xcrun --show-sdk-path)"
export UOS_LIBCXX_BUILD="$(mktemp -d /tmp/uos-libcxx.XXXXXX)"
export PKG_CONFIG_PATH="$(brew --prefix openssl@3)/lib/pkgconfig:$(brew --prefix sqlite)/lib/pkgconfig:$(brew --prefix zlib)/lib/pkgconfig:$(brew --prefix icu4c)/lib/pkgconfig:$(brew --prefix brotli)/lib/pkgconfig:$(brew --prefix zstd)/lib/pkgconfig"
export UOS_LIBCXX_LINK="-L$LLVM_PREFIX/lib/c++ -Wl,-rpath,$LLVM_PREFIX/lib/c++"
uname -msr
xcrun --show-sdk-path
"$CXX" --version
pkg-config --modversion openssl sqlite3 zlib libbrotlidec libzstd icu-uc icu-i18n
```

Build the same Boost release used by the Linux proof with the selected compiler. This avoids
accidentally linking a libstdc++ archive or an independently configured C++ dependency. Verify the
release archive against Boost's published checksum before extracting it.

```sh
curl -fL https://archives.boost.io/release/1.90.0/source/boost_1_90_0.tar.bz2 -o "$UOS_LIBCXX_BUILD/boost.tar.bz2"
tar -xjf "$UOS_LIBCXX_BUILD/boost.tar.bz2" -C "$UOS_LIBCXX_BUILD"
export BOOST_ROOT="$UOS_LIBCXX_BUILD/boost_1_90_0"
(
  cd "$BOOST_ROOT"
  ./bootstrap.sh --with-toolset=clang --with-libraries=json,container
  ./b2 -j4 --layout=system toolset=clang cxxstd=20 variant=release link=static threading=multi \
    cxxflags="-stdlib=libc++ -nostdinc++ -isystem $LLVM_PREFIX/include/c++/v1" \
    linkflags="-stdlib=libc++ $UOS_LIBCXX_LINK" stage
)
```

Probe actual floating-point parsing and formatting, including linkage to the selected library:

```sh
cat > "$UOS_LIBCXX_BUILD/charconv.cpp" <<'CPP'
#include <charconv>
#include <system_error>
int main() {
  const char text[] = "1.25";
  double value = 0;
  const auto parsed = std::from_chars(text, text + 4, value, std::chars_format::general);
  char output[64];
  const auto formatted = std::to_chars(output, output + 64, value, std::chars_format::general);
  return parsed.ec != std::errc{} || parsed.ptr != text + 4 || value != 1.25 || formatted.ec != std::errc{};
}
CPP
"$CXX" -std=c++20 -stdlib=libc++ -nostdinc++ -isystem "$LLVM_PREFIX/include/c++/v1" \
  -Wall -Wextra -Wshadow -Wconversion -Werror \
  "$UOS_LIBCXX_BUILD/charconv.cpp" -L"$LLVM_PREFIX/lib/c++" -Wl,-rpath,"$LLVM_PREFIX/lib/c++" \
  -o "$UOS_LIBCXX_BUILD/charconv"
"$UOS_LIBCXX_BUILD/charconv"
```

A failure requires a supported compiler/library pair or a separately proven numeric compatibility
change. Do not use locale-sensitive parsing or weaken the Bun-recorded numeric cases.

## Complete object build

The existing `tools/unit.sh`, `tools/env.sh`, `tools/build.py` and CMake profiles contain Linux/host
paths and supervision. They are not a Darwin build interface. On a disposable development machine,
this direct recipe compiles every runtime source first, then every remaining source, then every test
and support source, using four workers. It deliberately continues collecting failures in each phase.
Every failed translation unit retains its exact command and diagnostics under the external output root.

```sh
python3 - <<'PY'
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import json
import os
import shlex
import subprocess

root = Path.cwd().resolve()
gateway = root / "cpp/gateway"
build = Path(os.environ["UOS_LIBCXX_BUILD"])
packages = ["openssl", "sqlite3", "zlib", "libbrotlidec", "libzstd", "icu-uc", "icu-i18n"]
flags = [os.environ["CXX"], "-std=c++20", "-stdlib=libc++", "-O0", "-g0",
         "-nostdinc++", "-isystem", str(Path(os.environ["LLVM_PREFIX"]) / "include/c++/v1"),
         "-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Werror",
         "-DBOOST_JSON_NO_LIB", "-DBOOST_CONTAINER_NO_LIB", "-DBOOST_SYSTEM_NO_LIB",
         "-DUOS_GATEWAY_NATIVE_LIBRARIES", "-pthread",
         "-I", str(gateway / "include"), "-I", str(gateway),
         "-isystem", os.environ["BOOST_ROOT"]]
flags += shlex.split(subprocess.check_output(["pkg-config", "--cflags", *packages], text=True))

def compile_one(source):
    obj = build / "obj" / source.relative_to(gateway).with_suffix(".o")
    obj.parent.mkdir(parents=True, exist_ok=True)
    command = [*flags, "-c", str(source), "-o", str(obj)]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    obj.with_suffix(".log").write_text(shlex.join(command) + "\n" + result.stdout)
    return {"source": str(source.relative_to(gateway)), "status": result.returncode, "command": command}

runtime = sorted((gateway / "src/rt").glob("*.cpp"))
modules = [p for p in sorted((gateway / "src").rglob("*.cpp")) if p not in runtime]
tests = sorted((gateway / "tests").rglob("*.cpp"))
results = []
for name, sources in [("runtime", runtime), ("modules", modules), ("tests", tests)]:
    with ThreadPoolExecutor(max_workers=4) as pool:
        phase = list(pool.map(compile_one, sources))
    results.extend(phase)
    print(name, "total", len(phase), "passed", sum(r["status"] == 0 for r in phase),
          "failed", sum(r["status"] != 0 for r in phase), flush=True)
(build / "compile-results.json").write_text(json.dumps(results, indent=2) + "\n")
raise SystemExit(any(r["status"] for r in results))
PY
```

The job's starting inventory is 44 runtime sources, 327 other sources and 459 test/support
translation units, including 437 standalone test targets and 34 runtime targets. Discover the actual
inventory each time; a successful subset is not a complete receipt.

## Link the runtime tests

After the object gate passes, link each runtime test against all runtime objects and the matching
Boost archives. The C libraries can be shared; their C interfaces do not exchange C++ standard-library
objects. The direct-library define enables ICU IDNA and native SQLite/compression bindings.

```sh
python3 - <<'PY'
from pathlib import Path
import os
import shlex
import subprocess

build = Path(os.environ["UOS_LIBCXX_BUILD"])
boost = Path(os.environ["BOOST_ROOT"]) / "stage/lib"
packages = ["openssl", "sqlite3", "zlib", "libbrotlidec", "libzstd", "icu-uc", "icu-i18n"]
libraries = shlex.split(subprocess.check_output(["pkg-config", "--libs", *packages], text=True))
cpp_library = shlex.split(os.environ["UOS_LIBCXX_LINK"])
rt = [str(p) for p in sorted((build / "obj/src/rt").glob("*.o"))]
(build / "bin").mkdir(exist_ok=True)
for source in sorted(Path("cpp/gateway/tests/rt").glob("*_test.cpp")):
    binary = build / "bin" / source.stem
    command = [os.environ["CXX"], "-stdlib=libc++", "-pthread", *cpp_library,
               str(build / "obj/tests/rt" / source.with_suffix(".o").name), *rt,
               str(boost / "libboost_json.a"), str(boost / "libboost_container.a"),
               *libraries, "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run(["otool", "-L", str(binary)], check=True)
PY
```

Darwin does not need Linux's `-ldl`. Inspect `otool -L`, compiler include search paths and dependency
hashes to establish the actual libc++ and dylib closure. The native path does not use the fallback
`dlopen` names; legacy fallback loading additionally requires the selected dylibs to be discoverable
by dyld. Never load two SQLite libraries against the same database.

Execute tests only through a qualified Darwin fixture boundary with private HOME/TMPDIR/cwd,
no provider/auth environment, loopback-only networking and bounded child cleanup. That boundary
is still missing from the existing Linux test tools. Keep Unix-socket scratch paths short: Darwin's
`sun_path` is smaller than Linux's; Asio checks the native endpoint size. Use a private directory
under `/tmp`, not a deeply nested build path. Do not present these compile/link recipes as test-run,
production launcher or service activation acceptance.

The Linux filesystem suite's enormous timestamp cases require a filesystem that preserves their
recorded range. This job used a private tmpfs mount after Bun and C++ both demonstrated timestamp
clamping on the runner's ordinary filesystem. No timestamp expectation was changed. A Mac filesystem
needs its own Bun comparison. The Linux x86_64 `O_NOFOLLOW` fixture now records Bun's `131072`;
the earlier ARM64 `32768` expectation is retained for that original target.

## Platform switches added by this job

| File relative to `cpp/gateway`                                                                                           | Guarded change                                                                                                                             |
| ------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------ |
| `src/rt/env.cpp`                                                                                                         | Apple uses `<crt_externs.h>` and `*_NSGetEnviron()`; Linux retains `environ`.                                                              |
| `src/rt/process_loop.cpp`                                                                                                | Same Apple environment accessor for inherited `posix_spawn` environments.                                                                  |
| `src/rt/process.cpp`                                                                                                     | Apple uses `<sys/param.h>` / `MAXHOSTNAMELEN`; Linux retains `HOST_NAME_MAX`.                                                              |
| `src/rt/gzip.cpp`                                                                                                        | Apple fallback `libz.1.dylib`; Linux `libz.so.1`.                                                                                          |
| `src/rt/sqlite.cpp`                                                                                                      | Apple fallback `libsqlite3.dylib`; Linux `libsqlite3.so.0`, including loader error text.                                                   |
| `src/rt/fetch_transport_codec.cpp`                                                                                       | Apple fallback `libbrotlidec.1.dylib` / `libzstd.1.dylib`; Linux retains `.so.1` names.                                                    |
| `tests/rt/fs_test.cpp`                                                                                                   | Native `st_*timespec` on Apple and `st_*tim` on Linux for the same timestamp assertions; Bun-proven Linux x86_64 `O_NOFOLLOW` expectation. |
| `tests/rt/process_test.cpp`                                                                                              | Target-specific platform/architecture assertions; Apple executable-path expectation uses dyld, Linux retains `/proc/self/exe`.             |
| `tests/{config_differential,local_admin_auth,storage_bun_kv,kv_durable_interop}_test.cpp`, `tests/rt/kv_sqlite_test.cpp` | Apple self-reexecution uses `process::execPath()`; Linux retains SDK-loader-aware `/proc/self/cmdline` parsing.                            |
| `tests/production_smoke/serve_failure_gate.cpp`                                                                          | Apple uses `socket` plus checked `fcntl(FD_CLOEXEC)`; Linux retains `SOCK_CLOEXEC`.                                                        |
| `tests/jev_codex_smoke_test.cpp`, `tests/jev_compaction_smoke_test.cpp`                                                  | Existing GCC `-Wsubobject-linkage` pragmas are restricted to GCC, excluding Clang.                                                         |

The final findings also enumerate compiler-only edits discovered by the full test-object pass.
No blanket warning exclusions, alternate expectations, new runtime
framework or module restructuring are part of this work.

## Existing platform boundaries and remaining work

- `runtime/permissions_linux.cpp` already guards its Linux ARM64 kernel headers and enforcement.
  Its API refuses other hosts with `Linux confinement requires Linux ARM64`, matching
  `src/runtime/permissions-linux.ts`. The TypeScript launcher's Darwin branch uses Seatbelt and does
  not call this API. No permissive stub or change to that refusal was added.
- Existing Darwin branches cover `_NSGetExecutablePath`/`realpath`, native signal constants,
  `arc4random_buf`, filesystem `st_*timespec`, and a SIGCHLD-driven child watcher with `pipe`/`fcntl`.
  Linux retains `getrandom`, `pidfd_open` and `pipe2`; no polling fallback was introduced here.
- HTTP, TLS and WebSocket sockets use Asio. No first-party production `epoll`, `eventfd`, `signalfd`,
  `timerfd`, `MSG_NOSIGNAL`, `SOCK_CLOEXEC` or `accept4` dependency was found. Linux libc++ execution
  still exercises epoll through Asio; it cannot prove the Darwin kqueue implementation.
- Actual Apple headers/linkage, dyld fallback resolution, OpenSSL trust roots, Unix sockets,
  half-closes, cancellation, child-exit races, signal values and error strings need Mac execution.
  Some process/filesystem expectations were recorded on Linux and require a Bun-on-Mac oracle;
  passing compilation does not make those values platform-independent.
- CMake dependency paths, ELF/mold flags, package manifests, launcher confinement and Linux-only
  build/test supervisors are outside this job's owned paths. A Mac package and same-binary acceptance
  remain separate work. Run every original test under a real Darwin boundary before accepting a Mac build.
- Raspberry Pi OS/glibc, native dependency closure and runtime memory qualification remain unknown.
  Linux x86_64/libc++ results provide no ARM64 Linux or Pi binary/runtime acceptance.
