# Boost.Process v2 migration

## Work contract v1

Replace the C++ gateway's POSIX subprocess launcher, pipe setup and exit watcher with Boost.Process v2 and Boost.Asio. Preserve the source-compatible API and the callers' argv, environment, working directory, stdio, exit, signal, and child-ownership behavior. Child setup must preserve the existing execution boundary, including signal reset and descriptor handling. Record platform-specific behavior and any intentional departure from Bun.

The affected surfaces are direct process calls in the offline KV conversion/proof code and the Codex smoke tool, plus all test binaries through their common runtime link list. Linux ARM64 is the execution target for this job. Darwin compatibility is a source and library qualification item.

Edits are limited to the owned process source, header, test, and documentation paths in the task. Provider traffic, deployment, build infrastructure edits and unrelated repairs are out of scope.

The baseline is commit `b54357a92c0f2669335d83f04047c3f7de32f925`, tree `d0aabad25d3dbe02f2f33ff519e5b77715d19d37`. The three owned production files contain 1,001 lines (165 + 610 + 226), and `process_test.cpp` contains 465.

Acceptance requires the full Linux ARM64 release and ASan/UBSan suites from `bash cpp/gateway/tools/ci-build.sh --config release` and `bash cpp/gateway/tools/ci-build.sh --config asan`. The driver's all-runtime link rule makes all 442 registered binaries affected tests. Both gates use the driver's namespace isolation and local fixtures. The job stops after the final-tree gate receipts and branch handoff are recorded, or when a prerequisite prevents further verification.

## Implementation

Boost.Process v2 launches the child, applies stdio and cwd, and waits asynchronously on the gateway's existing Boost.Asio executor. Linux uses Boost's pidfd backend. Darwin uses Boost's SIGCHLD backend and checks each registered PID independently. The Asio pipe endpoints feed the existing runtime streams and buffered stdin sink. The exit observer retains the child while it is running, even if every caller releases its handle. Closing the loop invokes the Boost process owner's termination and reap path.

The gateway still selects the executable with its explicit PATH rules, passes the original argv and environment, and resets the child signal dispositions and mask in Boost's pre-exec initializer. Empty PATH segments are skipped as before. A direct Boost executable-search probe with `PATH=::` selected an executable from cwd. The launcher disables Asio fork notifications because the child execs and never uses the inherited I/O context, which can have other threads posting to it. Both ends of data pipes are marked close-on-exec. Boost's launcher closes descriptors outside its child whitelist. Ignored streams open the existing `/dev/null` with the corresponding read or write access and without file creation.

The remaining native `kill` call supports arbitrary signals in the public API's accepted numeric range. Boost's named process operations cover only selected signals, and its `terminate` also waits. `waitpid` remains in the launch-exception cleanup because Boost can complete fork and exec before creating its process handle. A probe that denied only `pidfd_open` with seccomp observed an `EBADF` launch failure from Boost and then `waitpid(-1, ..., WNOHANG) == -1` with `ECHILD`. The ordinary wait and reaping path belongs to Boost.

| First-party file             | Before | After |
| ---------------------------- | -----: | ----: |
| `src/rt/process.cpp`         |    165 |   152 |
| `src/rt/process_loop.cpp`    |    610 |   518 |
| `include/uos/rt/process.hpp` |    226 |   225 |
| Production total             |  1,001 |   895 |
| `tests/rt/process_test.cpp`  |    465 |   537 |

The 15 process cases retain the previous coverage and add Linux execution of the concurrent-reap and EPIPE cases, signal-mask and descriptor inheritance controls, argument round trips, detached caller handle coverage, and pending-child teardown coverage. Native signal spellings and the `spawn` operation name are recorded in the runtime deviations. [Deviations from Bun](../DEVIATIONS-FROM-BUN.md)

## Dependencies and commands

Validation runs on Ubuntu 24.04 ARM64 with four vCPUs, Clang 21.1.8 from the apt.llvm.org Noble LLVM 21 repository, GCC 14.2.0, OpenSSL 3.0.13, zlib 1.3, Brotli 1.1.0, and Zstd 1.5.5. All downloaded sources, builds and logs are outside the checkout.

| Dependency archive                      | SHA-256                                                            |
| --------------------------------------- | ------------------------------------------------------------------ |
| Boost `boost_1_92_0.tar.bz2`            | `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` |
| ICU `icu4c-78.3-sources.tgz`            | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` |
| SQLite `sqlite-autoconf-3540000.tar.gz` | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` |

Boost.Process v2 requires compiled code on Linux and macOS. Its POSIX launcher calls compiled symbols including `close_all`, `get_last_error` and `do_throw_error`. Link an ABI-matched `libboost_process`. The standard Boost build also declares Boost.Filesystem as a dependency. The runner built and linked the official static archives. The portable driver accepts them through `UOS_CI_LDFLAGS`. The native CMake profiles and `tools/unit.sh` have hard-coded library lists and still need the Process archive in their build settings. Those files belong to the shared integration surface. [Boost.Process 1.92.0](https://www.boost.org/doc/libs/1_92_0/libs/process/doc/html/index.html)

```sh
scratch=/tmp/cppgw-std-process-20261118
boost="$scratch/boost_1_92_0"

# Boost source directory
./bootstrap.sh --with-toolset=gcc --with-libraries=json,container --prefix="$scratch/boost"
printf '%s\n' 'using gcc : 14 : /usr/bin/g++-14 ;' >"$scratch/user-config.jam"
./b2 --user-config="$scratch/user-config.jam" -j3 toolset=gcc-14 cxxstd=20 \
  variant=release link=static threading=multi --with-json --with-container \
  --with-process --with-filesystem stage

# SQLite source directory
./configure --prefix="$scratch/sqlite" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
make -j1
make install

# ICU source directory
./configure --prefix="$scratch/icu78" --disable-tests --disable-samples --disable-extras --disable-static
make -j3
make install
```

The portable-driver commands run from the repository root. `$runtime` is the dedicated directory under `/var/tmp` containing copies of the verified ICU and SQLite shared libraries. The runner puts that directory in the ELF RPATH because the driver mounts a fresh tmpfs at `/tmp`, and some tests exec children with a completely replaced environment.

```sh
runtime=$(cat "$scratch/runtime-path")
for config in release asan; do
  flags="-isystem $boost --system-header-prefix=third_party/simdutf/"
  if [ "$config" = asan ]; then flags="$flags -gline-tables-only"; fi
  empty_home=$(mktemp -d "$scratch/$config-home.XXXXXX")
  private_tmp=$(mktemp -d "$scratch/$config-tmp.XXXXXX")
  env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$empty_home" TMPDIR="$private_tmp" \
    LANG=C.UTF-8 CXX=/usr/bin/clang++-21 UOS_CI_JOBS=2 \
    PKG_CONFIG_PATH="$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig" \
    UOS_CI_BUILD_DIR="$scratch/ci" UOS_CI_CXXFLAGS="$flags" \
    UOS_CI_LDFLAGS="-L$boost/stage/lib -lboost_process -lboost_filesystem -Wl,--disable-new-dtags,-rpath,$runtime" \
    bash cpp/gateway/tools/ci-build.sh --config "$config"
done
```

The driver runs each binary in independent network, mount and PID namespaces with a private tmpfs, an empty home, a new temporary directory and a credential-free environment. The command selects every test without a shard filter. Release and sanitizer builds run as independent processes. The final source and test commit is `dfef31b22cc971cbfa897bd18e030dbbd8b21b1e`, tree `586f80c663892b2fdaa58356c3f934cf5904fe10`. A preliminary focused compile overlapped an edit and consumed inconsistent input. The focused runs were restarted from the committed source.

The unmodified driver classified `third_party/simdutf/simdutf.h` as a first-party header when compiling `src/rt/base64.cpp`. Clang 21 stopped on `-Werror,-Wsign-conversion` inside that header. The final commands supply Clang's system-header prefix for the vendor directory and retain the first-party warning flags. The driver used a fresh cache for the adjusted flags. The repository driver, vendor source and base64 code are outside the owned paths.

## Verification in progress

Focused isolated release and ASan/UBSan runs passed 15 of 15 process cases. A separate ASan/UBSan run forced the SIGCHLD backend with `-DBOOST_PROCESS_V2_DISABLE_PIDFD_OPEN` and passed 15 of 15. GCC 14.2.0 compiled `process_loop.cpp` with the same strict warnings at `-O2`. Both full Linux ARM64 profiles are running on the frozen C++ sources and tests.

The two process implementation objects and the process test cross-compiled as Mach-O ARM64 with Clang 21.1.8, libc++, the macOS 15.5 SDK and Boost 1.92.0. This compile check exposed the SDK's signal-set macros and verified the correction. The SDK archive `MacOSX15.5.sdk.tar.xz` had SHA-256 `c15cf0f3f17d714d1aa5a642da8e118db53d79429eb015771ba816aa7c6c1cbd`. [SDK archive](https://github.com/joseluisq/macosx-sdks/releases/tag/15.5)

```sh
for source in cpp/gateway/src/rt/process.cpp cpp/gateway/src/rt/process_loop.cpp cpp/gateway/tests/rt/process_test.cpp; do
  clang++-21 --target=arm64-apple-macos15.5 -isysroot "$scratch/MacOSX15.5.sdk" -stdlib=libc++ \
    -std=c++20 -O2 -Wall -Wextra -Wshadow -Wconversion -Werror \
    -DBOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING -Icpp/gateway/include -isystem "$boost" \
    -c "$source" -o "$scratch/darwin/$(basename "$source" .cpp).o"
done
```

A standalone launcher cross-linked for macOS with `ld64.lld-21 -arch arm64 -platform_version macos 15.5 15.5` after adding the archive built from the matching upstream Process sources. Omitting the archive produced unresolved symbols. The Linux fault-injection launcher likewise linked with `-lboost_process` and failed linkage without it. Darwin runtime execution requires a Mac.

## Helper launch timing

An isolated Clang 21.1.8 `-O2` scratch benchmark compared the original and migrated process objects with the same supporting runtime objects. Each process warmed up 20 launches, then timed 128 sequential `/usr/bin/true` launches through `spawn` and `exited()` with all stdio ignored. It alternated order over seven pairs on CPU 3 via `taskset`, while the full builds ran. The executable and launcher configurations were the same apart from the process implementation.

| Implementation | Median wall time per child | Median parent peak RSS |
| -------------- | -------------------------: | ---------------------: |
| Original       |                   1.045 ms |             13,276 KiB |
| Boost.Process  |                   1.589 ms |             13,276 KiB |

The median launch took 0.544 ms longer in this fixture. The direct application spawn callers are the offline KV tools and the Codex smoke tool.

## Continuation on a fresh ARM64 runner

The continuation started at `98d86c499b8f17fc24cf41c12eab73fdbc8e7cc0`, tree `436ea19474306ead366c973f7b9d223983ee46af`. The prior runner's build objects and terminal receipts were unavailable. Both process implementation files and `process_test.cpp` compiled on the starting tree with Clang 21.1.8, Boost 1.92.0 and `-std=c++20 -O2 -Wall -Wextra -Wshadow -Wconversion -Werror -DBOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING`. The continuation corrected the public header comment to describe native signal names. The production line count is 895.

The full runs started at `9ad53dec9385811549fe7496e0e0ae9b5075ec96`, tree `f2607603c7f8a1a9013476a9eef35f286ae022bb`. Each run selects all 442 registered test binaries and compiles all 369 production sources, including the 44 runtime objects linked into every test. `python3 -B cpp/gateway/tools/links.py --check` resolved all 442 entries. The namespaces probe passed via passwordless sudo and drops fixture execution to the runner UID. The runs have two build jobs each on the four-core host. The driver logs per-object progress, test status and terminal receipts in scratch.

The continuation used the same verified dependency archives and build settings. It staged all four Boost archives with `./b2 --user-config="$work/user-config.jam" -j2 toolset=gcc-14 cxxstd=20 variant=release link=static threading=multi --with-json --with-container --with-process --with-filesystem stage`. The ICU and SQLite install prefixes are under `/var/tmp`, which stays visible from the driver test namespaces and across a child's explicit environment replacement. The linker is mold 2.30.0.

```sh
work=/var/tmp/cppgw-std-process-resume
boost="$work/boost_1_92_0"

# Run once per profile in separate shells, with config=release and config=asan.
flags="-isystem $boost --system-header-prefix=third_party/simdutf/"
if [ "$config" = asan ]; then flags="$flags -gline-tables-only"; fi
empty_home=$(mktemp -d "$work/$config-home.XXXXXX")
private_tmp=$(mktemp -d "$work/$config-tmp.XXXXXX")
env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$empty_home" TMPDIR="$private_tmp" LANG=C.UTF-8 \
  CXX=/usr/bin/clang++-21 UOS_CI_JOBS=2 \
  PKG_CONFIG_PATH="$work/icu78/lib/pkgconfig:$work/sqlite/lib/pkgconfig" \
  UOS_CI_BUILD_DIR="$work/ci" UOS_CI_CXXFLAGS="$flags" \
  UOS_CI_LDFLAGS="-L$boost/stage/lib -lboost_process -lboost_filesystem -Wl,--disable-new-dtags,-rpath,$work/icu78/lib:$work/sqlite/lib" \
  bash cpp/gateway/tools/ci-build.sh --config "$config"
```

The same three owned translation units cross-compiled again with the strict macOS command above and the verified macOS 15.5 SDK. `file` identified all three outputs as Mach-O arm64 objects. The runner cannot execute Darwin binaries.

The Boost link control was repeated with a minimal `boost::process::v2::process` launcher for `/usr/bin/true`. The Linux linker failed on unresolved Boost.Process symbols without the compiled library, then linked with the staged `libboost_process.a` and `libboost_filesystem.a`. The resulting ELF executed successfully through `cpp/gateway/cmake/ci/isolate.py`. The Darwin control compiled `detail/last_error.cpp`, `detail/throw_error.cpp`, `posix/close_handles.cpp` and `error.cpp` from the same Boost archive for arm64 with the SDK and libc++. `ld64.lld-21 -arch arm64 -platform_version macos 15.5 15.5 -syslibroot "$work/MacOSX15.5.sdk"` with `-lSystem -lc++` reported unresolved Boost.Process symbols without the archive and produced a Mach-O arm64 executable with it.

## ICU include classification and resumed gates

The initial release build diagnosed fatal `-Wsign-conversion` warnings in ICU 78.3's installed `unicode/uset.h` and `unicode/unistr.h` through `src/rt/string.cpp`. The portable driver added the installed ICU headers as a regular `-I` include. Marking `$work/icu78/include` as a system directory fixed the strict compile of that source with all first-party warnings enabled. Both initial drivers were interrupted through their own lock-holder PIDs and exited 143. Ninja had reported 381 of 836 commands in release and 225 of 836 in ASan/UBSan. The logs are retained in scratch with SHA-256 `c79ef348b9c3e89f3da904f79b63d891d14aeee0e52ffb24a8a6db8db31ddc29` and `f9cda561b22f456ccaafd7e6ead5fd3971bc64fc5640d79b7b56044b5a312a72`, respectively. The ICU include classification belongs to the shared driver surface.

The focused release check linked the 44 runtime objects, the vendor archives and `process_test.cpp` with the corrected ICU compilation. The portable driver's namespace runner passed 15 of 15 cases in 0.767 seconds. The focused `release-process-control/receipt.json` SHA-256 is `06d0d035ea322438c9369859e22ba5c3c237ef18b6714fbead8f7cd45c3f2af6`.

The full profiles restart from a fresh build cache and run sequentially with four jobs each. The commands above use these updated settings. The driver still selects all tests with no shard filter.

```sh
flags="-isystem $boost --system-header-prefix=third_party/simdutf/ -isystem $work/icu78/include"
if [ "$config" = asan ]; then flags="$flags -gline-tables-only"; fi
# Supply these in the env -i invocation above.
UOS_CI_JOBS=4
UOS_CI_BUILD_DIR="$work/ci-system-icu"
```
