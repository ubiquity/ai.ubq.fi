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
  flags="-isystem $boost"
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

The driver runs each binary in independent network, mount and PID namespaces with a private tmpfs, an empty home, a new temporary directory and a credential-free environment. The command selects every test without a shard filter. Release and sanitizer builds ran as independent processes. The final source and test commit is `dfef31b22cc971cbfa897bd18e030dbbd8b21b1e`, tree `586f80c663892b2fdaa58356c3f934cf5904fe10`. Superseded runs contributed only cached objects. A preliminary focused compile overlapped an edit and consumed inconsistent input. It was replaced by a run from the committed source.

## Verification in progress

Focused isolated release and forced-SIGCHLD ASan/UBSan runs passed 15 of 15 process cases. The forced backend uses `-DBOOST_PROCESS_V2_DISABLE_PIDFD_OPEN` on Linux. Both full Linux ARM64 profiles are running on the frozen source and test commit.

The two process implementation objects and the process test cross-compiled as Mach-O ARM64 with Clang 21.1.8, libc++, the macOS 15.5 SDK and Boost 1.92.0. This compile check exposed the SDK's signal-set macros and verified the correction. Darwin runtime execution requires a Mac.
