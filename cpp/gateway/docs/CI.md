# Portable gateway CI build

`tools/ci-build.sh` builds the C++20 port on a disposable Linux runner independently of the production SDK and CMake profiles. It compiles every canonical `src/**/*.cpp` once and uses `tools/links.py` for each test's exact object list. Every `src/rt/*.cpp` object is linked into every test. Dot-prefixed staging files are excluded. Use `bash cpp/gateway/tools/ci-build.sh --list` for the current test inventory.

## Ubuntu prerequisites

On Ubuntu 24.04, prepare Clang 21 or newer from apt.llvm.org or use GCC 14 or newer. Build Boost 1.92.0 JSON and Container and ICU 78.3 from their upstream sources. The distribution's Boost and ICU are older than those used by the ARM64 fixtures. For the other dependencies:

```sh
sudo apt-get update
sudo apt-get install -y ninja-build pkg-config libssl-dev zlib1g-dev libbrotli-dev libzstd-dev util-linux
# Optional faster linker, detected automatically on Linux:
sudo apt-get install -y mold
```

The current native SQLite differential suite expects SQLite 3.54.0, including double-quoted-string support. Distribution SQLite 3.46.1 can compile the port but does not meet those existing expected values. This runner prepared the published release as follows (all downloads/build outputs stay outside the checkout):

```sh
mkdir -p /tmp/cppgw-sqlite
curl --fail --location https://www.sqlite.org/2026/sqlite-autoconf-3540000.tar.gz \
  -o /tmp/cppgw-sqlite/sqlite-autoconf-3540000.tar.gz
printf '%s  %s\n' 134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed \
  /tmp/cppgw-sqlite/sqlite-autoconf-3540000.tar.gz | sha256sum --check
tar -xzf /tmp/cppgw-sqlite/sqlite-autoconf-3540000.tar.gz -C /tmp/cppgw-sqlite
cd /tmp/cppgw-sqlite/sqlite-autoconf-3540000
./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
make -j1
sudo make install
sudo ldconfig
pkg-config --modversion sqlite3
```

Library preparation is a separate prerequisite. The GitHub suite workflow prepares the same release and configure flags with `make -j4`. The driver takes `sqlite3.h` and `libsqlite3.so` from `UOS_CI_SQLITE_PREFIX` (default `/usr/local`) and adds that library directory to the test binaries' ELF RPATH. Use a prefix outside `/tmp`, which the test mount namespace hides. A probe built with the same flags checks the headers, prints `sqlite3_libversion()` from inside the fixture boundary and rejects a version mismatch before running the suite.

Use GCC 14 or newer or Clang 21 or newer, with Boost 1.86 or newer (1.92 recommended) and ICU 78.3 for the Unicode fixtures. GCC 12 is refused because of its known coroutine miscompile. Dependencies are linked through their installed headers and libraries with `UOS_GATEWAY_NATIVE_LIBRARIES=1`. SQLite uses the explicit prefix above, and `pkg-config` resolves flags for OpenSSL, zlib, Brotli, Zstd and ICU. Boost.JSON, Boost.Container and pthread are also required. For a staged ICU install, point `PKG_CONFIG_PATH` at its metadata and set its library RPATH through `UOS_CI_LDFLAGS`. This lets fixture subprocesses resolve the same library with a clean environment.

The driver validates the vendored simdjson, simdutf, llhttp, Ada, QCBOR and QuickJS-ng libregexp sources using `cmake/vendors.json` and `tools/vendor_sources.py`. It compiles the C sources with the matching C compiler, using GNU C11 for QuickJS-ng, and builds the other vendor sources with the C++ compiler. Each archive links into the test closures that use it. Vendor compilation uses the selected optimization and sanitizer profile and omits the first-party warning set. With Clang, `--system-header-prefix=third_party/` covers tree-relative vendor includes while Ninja tracks those headers as explicit inputs.

Tests require real Linux network, mount and PID namespaces. The driver first tries unprivileged user namespaces. If those are restricted, it tries passwordless `sudo -n` for namespace setup and drops back to the invoking uid/gid before running the test. Each test gets its own loopback-only network namespace, private 512 MiB tmpfs at `/tmp`, empty `HOME=/tmp/h`, `TMPDIR=/tmp/t` and working directory `/tmp/w`. The helper proves loopback communication and an outside-route denial before executing the binary. A credential-free environment allowlist also excludes provider settings, `CODEX_HOME`, synchronized authentication locations and unknown future credential variables. Registry entries cannot override the home/temp/path isolation or introduce credential/provider variables. The tmpfs fixture preserves the full Linux timestamp range used by the Bun-recorded filesystem tests; an ext4-backed runner directory clamps those times. Unsupported isolation fails closed.

The current runtime and fixtures still contain ARM64-only application contracts. A portable compilation does not mean production startup or confinement is supported on x86_64. See the findings linked below. Darwin execution needs a separately qualified OS fixture boundary and matching-platform runtime/tests; this driver currently refuses Darwin execution rather than claim an unrestricted pass.

## Commands

Run from any working directory; paths below assume the repository root:

```sh
CC=gcc-14 CXX=g++-14 UOS_CI_JOBS=4 bash cpp/gateway/tools/ci-build.sh --config debug
CC=gcc-14 CXX=g++-14 UOS_CI_JOBS=4 bash cpp/gateway/tools/ci-build.sh --config release
CC=clang-21 CXX=clang++-21 UOS_CI_JOBS=4 bash cpp/gateway/tools/ci-build.sh --config asan --shard 1/4
bash cpp/gateway/tools/ci-build.sh --list
bash cpp/gateway/tools/ci-build.sh --config debug rt/process
bash cpp/gateway/tools/ci-build.sh --config release --shard 1/4
```

`CC` defaults to `cc` and `CXX` to `g++`. Select compilers from the same toolchain for sanitizer runs. `UOS_CI_JOBS` defaults to the available CPU count (`nproc` equivalent). `UOS_CI_BUILD_DIR` defaults to `/tmp/uos-gateway-build` and must be outside the repository. The driver separates compiler/flag identities and configurations, locks each output tree, and lets Ninja track transitive header dependencies. A filter is a substring of the test path. `--shard i/n` takes every nth element of the sorted filtered list, starting with one-based index i. Empty selections and invalid arguments fail.

| Configuration | Additional compiler flags                                     |
| ------------- | ------------------------------------------------------------- |
| debug         | `-O0`                                                         |
| release       | `-O2`                                                         |
| asan          | `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer` |

All configurations retain `-std=c++20 -Wall -Wextra -Wshadow -Wconversion -Werror` and the Boost JSON/Container/System no-autolink definitions. Release and sanitizer configurations additionally define `BOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING` to use standard coroutine frame allocation. This avoids GCC 15/Boost.Asio 1.90 optimized allocator diagnostics without disabling any warning. Debug exercises Asio's recycling mode. The `asan` configuration instruments compilation and linkage with AddressSanitizer and UndefinedBehaviorSanitizer, enables LeakSanitizer with `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`, and halts on UBSan errors. `UOS_CI_CFLAGS` and `UOS_CI_CXXFLAGS` add language-specific compile flags. `UOS_CI_LDFLAGS` can add library paths. `UOS_CI_TIMEOUT` defaults to 300 seconds per binary. Links are bounded at 900 seconds. Timeouts and interruption kill the owned process group, including namespace descendants. At most `UOS_CI_JOBS` test binaries exist at once. The driver deletes binaries and scratch directories after execution and retains reusable objects and logs.

The final table lists every selected test, `PASS`, `FAIL`, `BUILD_FAIL` or `TIMEOUT`, and the harness's run/passed/failed case counts. A crash or missing summary cannot pass. Compilation continues after individual failures to collect complete diagnostics; any failed production compilation, test build, run or timeout makes the command exit nonzero. A crashed binary without a flushed harness summary has zero reported cases, not an invented count.

Each `$UOS_CI_BUILD_DIR/<compiler>-<identity>/<config>/` output tree contains `compile.log`, per-object compiler logs, `logs/<test>/{build,run}.log`, incremental `results.tsv` and a final machine-readable `receipt.json` with the Git commit/tree, compiler/flags/input identity, counts and wall times. The receipt records which tests linked successfully, including ones that later failed or timed out. Input hashes are checked again at the end; an edited input invalidates acceptance and makes the command fail while preserving the observed counts. Previous logs and receipts move to `history/` on a rerun. Original failures remain available.

Driver control tests use the actual selected compiler and actual namespaces:

```sh
CXX=g++ python3 -B cpp/gateway/cmake/ci/test_driver.py
CXX=clang++ python3 -B cpp/gateway/cmake/ci/test_driver.py
CXX=clang++ UOS_CI_SELFTEST_CONFIG=asan python3 -B cpp/gateway/cmake/ci/test_driver.py
```

These seven controls cover successful parallel environment isolation, failure counts, missing summaries, a timeout with descendants, link failure, sharding, and exclusion of unpublished dot-prefixed staging sources. They are separate from the gateway suite.

## Compiler cache

Install `ccache` to reuse compiled objects across fresh build directories. Set `CCACHE_DIR` to share one cache between local checkouts. The driver, CI fuzz builder and packagers use `ccache` from `PATH` by default. Set `UOS_CI_COMPILER_LAUNCHER` to a launcher command, or set `UOS_CI_COMPILER_LAUNCHER=''` to disable the launcher. The portable Linux packager resolves the command inside its build image and stores the local cache in BuildKit.

The driver records the launcher and per-invocation cache hits and misses in `receipt.json`. Ccache checks compiler contents, arguments and source and header inputs within a configuration. Repository-relative dependency paths let outputs move between checkouts. Inside each test namespace, a read-only bind of the current checkout gives fixture paths derived from `__FILE__` a stable location. [ARM64 measurements](findings/cppgw-build-cache-20261119.md)

CI scopes each cache to the runner image, configuration, installed toolchain package versions and vendored sources. The save key adds the run ID and restores use the same prefix across runs. All jobs can restore that configuration's cache, and shard 1 for each suite or the first fuzz target in sorted order saves it. Each cache is capped at 1.9 GB. The published Linux package workflow forces a fresh compile with the launcher disabled.

## Runner measurements

Measured on 2026-11-17: Ubuntu 26.04.1, x86_64, 4 CPUs, approximately 15 GiB RAM; GCC 15.2.0 (debug), GCC 14.3.0 (release), Clang 21.1.8, Boost 1.90, OpenSSL 3.5.5, SQLite 3.54.0 (prepared from source; distribution version was 3.46.1), ICU 78.2, zlib 1.3.1, Brotli 1.2.0, Zstd 1.5.7 and mold 2.40.4.

| Configuration / gate                | Object build wall time | Link/test wall time | Outcome                                                                       |
| ----------------------------------- | ---------------------- | ------------------- | ----------------------------------------------------------------------------- |
| GCC debug cold v3 (historical)      | 1,429.959 s            | 288.155 s           | 432 PASS / 4 FAIL / 1 BUILD_FAIL; later inventory edit invalidated acceptance |
| GCC debug final-tree rerun          | Pending                | Pending             | Required after v7 shared-runtime portability fixes                            |
| GCC release, complete suite         | In progress            | Pending             | Final GCC 14 run pending after v7 runtime fixes                               |
| GCC v4 driver controls              | Included in total      | 6.653 s total       | 7/7 passed                                                                    |
| Clang ASan/UBSan v4 driver controls | Included in total      | 9.047 s total       | 7/7 passed                                                                    |
| Full Clang / full ASan/UBSan suites | Not run                | Not run             | No full-suite acceptance claimed                                              |
| arm64 / macOS                       | Not run                | Not run             | Proposed CI only                                                              |

GCC 15 optimized compilation also reports unresolved libstdc++ variant-copy and direct awaited structured-binding diagnostics. This runner uses the permitted GCC 14.3 compiler for the complete release run; it passed strict comparison probes for the same failing modules. The GCC 15 optimized attempts remain diagnostic evidence, not successful release receipts.

Exact commands, original failures, source changes, limitations and terminal counts are maintained in [the runner findings](findings/cppgw-ci-build-x86-20261117.md). An incremental build must not be described as a cold-build measurement.

## GitHub Actions

The C++ gateway workflow runs on pushes to `claude/cpp-gateway-port`, `pool-merge/**` and `agent-jobs/**`, on pull requests targeting `claude/cpp-gateway-port`, and on manual dispatch. A newer run for the same ref cancels the older run. All jobs run on `ubuntu-26.04-arm` with read-only repository access and checkout credential persistence disabled. [Workflow](../../../.github/workflows/cpp-gateway-ci.yml)

| Check              | Jobs                           | Coverage                                                             |
| ------------------ | ------------------------------ | -------------------------------------------------------------------- |
| Release suite      | Four round-robin shards        | Every registered test with Clang `--config release`                  |
| Sanitizer suite    | Four round-robin shards        | The same tests with Clang `--config asan` and ASan, UBSan and LSan   |
| Library robustness | One job per registered harness | Five minutes per harness, or 40 minutes on pushes to the port branch |

Each suite shard has a 240-minute job limit and four workers. It builds the production objects and vendors, then links and runs its share of the sorted test inventory. The `ci-<config>-shard-<i>` artifact contains the job summary and any `receipt.json`, `results.tsv`, and driver, compiler, namespace, link and run logs. The summary counts successful links, passed and failed targets, and reported cases. It lists unsuccessful targets, including build failures and timeouts.

Both configurations split the stage 2a inventory into 111, 111, 111 and 110 tests. All four Release shards and a focused sanitizer selection passed on Linux ARM64. An earlier ASan shard built and ran 111 binaries on four x86_64 cores. The two build segments and the test run accounted for about 62 minutes, with 107 passes and four fixture failures before suite repair. [Stage 2a results](findings/cppgw-integrate-s2a-20261118.md) · [Earlier sanitizer receipt](findings/cppgw-ci-sanitizers-20261118.md#asan-shard-1-receipt)

Release shard 1 also seals the gateway from its existing Release objects and checks session memory on a fresh production-shaped SQLite fixture. It runs 20 sessions with the native Codex CLI when the image supplies it, and uses the same programmed-reply workload with the scripted client otherwise. It also runs 20 scripted rounds on eight concurrent connections and checks free arena bytes and idle anonymous RSS above allocator live bytes. The job saves both memory receipts with the package identity and sampled values. [Memory harness](../bench/codex_real/README.md)

The robustness matrix uses all entries in `fuzz/targets.json`. The discovery job publishes the selected targets. Each harness job has a 120-minute limit and runs libFuzzer with four workers in a private Linux network namespace, using a fresh home and temporary directory and a credential-free environment. The `fuzz-<target>` artifact contains its build and campaign logs, summary, and any finding inputs. Slow `regex` inputs and the empty crash artifact from the known ARM64 libFuzzer fork-parent exit leak stay in the evidence and are counted separately. Genuine findings and incomplete campaigns fail the job.

The same command runs the five-minute JSON harness on a disposable Linux host with passwordless sudo. With staged dependencies, set `CPLUS_INCLUDE_PATH` for the headers and pass the matching library directories through `LDFLAGS` inside the clean environment. Include an RPATH or set `LD_LIBRARY_PATH` for staged shared libraries.

```sh
fuzz_out=$(mktemp -d)
mkdir -p "$fuzz_out/home" "$fuzz_out/tmp"
sudo -n unshare --net -- setpriv --reuid="$(id -u)" --regid="$(id -g)" --clear-groups --no-new-privs \
  env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$fuzz_out/home" TMPDIR="$fuzz_out/tmp" \
  python3 -B cpp/gateway/fuzz/ci.py json --seconds 300 --jobs 4 --out "$fuzz_out"
```

The x86_64 qualification records local counts and diagnostics. Matching ARM64 results come from the workflow runs. [Release qualification](findings/cppgw-ci-workflow-20261118.md) · [Sanitizer and robustness qualification](findings/cppgw-ci-sanitizers-20261118.md)
