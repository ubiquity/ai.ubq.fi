# Portable gateway CI build

`tools/ci-build.sh` builds the C++20 port on a disposable Linux runner without the production SDK, its absolute paths, `tools/build.py`, or the production CMake presets. It compiles every canonical `src/**/*.cpp` once and uses `tools/links.py` for each test's exact object list. Every `src/rt/*.cpp` object is linked into every test. Dot-prefixed staging files are excluded. This checkout has 325 module sources, 44 runtime sources and 437 standalone tests; older notes count 436 tests.

## Ubuntu prerequisites

On Ubuntu 24.04 or newer:

```sh
sudo apt-get update
sudo apt-get install -y g++-14 clang ninja-build pkg-config libboost-json-dev libboost-container-dev \
  libssl-dev libsqlite3-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev util-linux
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

Library preparation is a separate prerequisite. The CI driver never installs dependencies at application startup or changes an expected value to accommodate an older system library. Ubuntu CI images need the same library prerequisite before the proposed build step below.

Use GCC 14 or newer, or a recent Clang, with Boost >= 1.83. GCC 12 is refused because of its known coroutine miscompile. Dependencies are linked through their installed headers and libraries with `UOS_GATEWAY_NATIVE_LIBRARIES=1`; SQLite, zlib, Brotli, Zstd and ICU use the current direct-library path. OpenSSL, Boost.JSON, Boost.Container and pthread are also required. `pkg-config` resolves the native C library include/link flags. No package is installed or downloaded by the driver.

Tests require real Linux network, mount and PID namespaces. The driver first tries unprivileged user namespaces. If those are restricted, it tries passwordless `sudo -n` for namespace setup and drops back to the invoking uid/gid before running the test. Each test gets its own loopback-only network namespace, private 512 MiB tmpfs at `/tmp`, empty `HOME=/tmp/h`, `TMPDIR=/tmp/t` and working directory `/tmp/w`. The helper proves loopback communication and an outside-route denial before executing the binary. A credential-free environment allowlist also excludes provider settings, `CODEX_HOME`, synchronized authentication locations and unknown future credential variables. Registry entries cannot override the home/temp/path isolation or introduce credential/provider variables. The tmpfs fixture preserves the full Linux timestamp range used by the Bun-recorded filesystem tests; an ext4-backed runner directory clamps those times. Unsupported isolation fails closed.

The current runtime and fixtures still contain ARM64-only application contracts. A portable compilation does not mean production startup, native development bootstrap or confinement is supported on x86_64. See the findings linked below. Darwin execution needs a separately qualified OS fixture boundary and matching-platform runtime/tests; this driver currently refuses Darwin execution rather than claim an unrestricted pass.

## Commands

Run from any working directory; paths below assume the repository root:

```sh
CXX=g++-14 UOS_CI_JOBS=4 cpp/gateway/tools/ci-build.sh --config debug
CXX=g++-14 UOS_CI_JOBS=4 cpp/gateway/tools/ci-build.sh --config release
CXX=clang++ UOS_CI_JOBS=4 cpp/gateway/tools/ci-build.sh --config asan
cpp/gateway/tools/ci-build.sh --list
cpp/gateway/tools/ci-build.sh --config debug rt/process
cpp/gateway/tools/ci-build.sh --config release --shard 1/4
```

`CXX` defaults to `g++`. `UOS_CI_JOBS` defaults to the available CPU count (`nproc` equivalent). `UOS_CI_BUILD_DIR` defaults to `/tmp/uos-gateway-build` and must be outside the repository. The driver separates compiler/flag identities and configurations, locks each output tree, and lets Ninja track transitive header dependencies. A filter is a substring of the test path. `--shard i/n` takes every nth element of the sorted filtered list, starting with one-based index i. Empty selections and invalid arguments fail.

| Configuration | Additional compiler flags                                     |
| ------------- | ------------------------------------------------------------- |
| debug         | `-O0`                                                         |
| release       | `-O2`                                                         |
| asan          | `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer` |

All configurations retain `-std=c++20 -Wall -Wextra -Wshadow -Wconversion -Werror` and the Boost JSON/Container/System no-autolink definitions. Sanitizers also apply during linkage; runtime sanitizer errors are fatal. `UOS_CI_CXXFLAGS` and `UOS_CI_LDFLAGS` can supply additional dependency include/library paths. `UOS_CI_TIMEOUT` sets the per-binary run timeout in seconds (default 300); links are bounded at 900 seconds. Timeouts and interruption kill the owned process group, including namespace descendants. At most `UOS_CI_JOBS` test binaries exist at once; binaries and scratch directories are deleted after execution while objects and logs remain reusable.

The final table lists every selected test, `PASS`, `FAIL`, `BUILD_FAIL` or `TIMEOUT`, and the harness's run/passed/failed case counts. A crash or missing summary cannot pass. Compilation continues after individual failures to collect complete diagnostics; any failed production compilation, test build, run or timeout makes the command exit nonzero. A crashed binary without a flushed harness summary has zero reported cases, not an invented count.

Each output tree contains `compile.log`, per-object compiler logs, `logs/<test>/{build,run}.log`, incremental `results.tsv` and a final machine-readable `receipt.json` with the Git commit/tree, compiler/flags/input identity, counts and wall times. Input hashes are checked again at the end; an edited input invalidates acceptance and makes the command fail while preserving the observed counts. Previous logs and receipts move to `history/` on a rerun. Original failures remain available.

Driver control tests use the actual selected compiler and actual namespaces:

```sh
CXX=g++ python3 -B cpp/gateway/cmake/ci/test_driver.py
CXX=clang++ python3 -B cpp/gateway/cmake/ci/test_driver.py
CXX=clang++ UOS_CI_SELFTEST_CONFIG=asan python3 -B cpp/gateway/cmake/ci/test_driver.py
```

These seven controls cover successful parallel environment isolation, failure counts, missing summaries, a timeout with descendants, link failure, sharding, and exclusion of unpublished dot-prefixed staging sources. They are separate from the gateway suite.

## Runner measurements

Measured on 2026-11-17: Ubuntu 26.04.1, x86_64, 4 CPUs, approximately 15 GiB RAM; GCC 15.2.0, Clang 21.1.8, Boost 1.90, OpenSSL 3.5.5, SQLite 3.54.0 (prepared from source; distribution version was 3.46.1), ICU 78.2, zlib 1.3.1, Brotli 1.2.0, Zstd 1.5.7 and mold 2.40.4. Bun 1.4.0 is the unchanged behavior oracle.

| Configuration / gate        | Object build wall time | Link/test wall time | Outcome                                    |
| --------------------------- | ---------------------- | ------------------- | ------------------------------------------ |
| GCC debug, complete suite   | In progress            | Pending             | Pending                                    |
| GCC release, complete suite | Pending                | Pending             | Pending                                    |
| GCC driver controls         | Included in total      | 9.484 s total       | 6/6 passed                                 |
| Clang driver controls       | Included in total      | 10.517 s total      | 6/6 passed                                 |
| Clang ASan driver controls  | Included in total      | 12.850 s total      | 6/6 passed                                 |
| Full ASan/UBSan suite       | Not run                | Not run             | No full-suite sanitizer acceptance claimed |
| arm64 / macOS               | Not run                | Not run             | Proposed CI only                           |

Exact commands, original failures, source changes, limitations and terminal counts are maintained in [the runner findings](findings/cppgw-ci-build-x86-20261117.md). An incremental build must not be described as a cold-build measurement.

## Proposed workflow

This is a maintainer proposal, not an installed workflow. It is manual-only to respect the repository's Actions budget. It includes the requested platform/configuration/shard matrix; Darwin remains a qualification job that will fail until its actual OS boundary and remaining runtime/fixture portability are implemented. Do not turn unsupported jobs into successful skips or make this proposal an automatic PR/push/scheduled workflow.

```yaml
name: Portable C++ gateway qualification
on:
  workflow_dispatch:
permissions:
  contents: read
jobs:
  gateway:
    strategy:
      fail-fast: false
      max-parallel: 4
      matrix:
        platform: [ubuntu-x86_64, ubuntu-arm64, macos-arm64]
        config: [debug, release, asan]
        shard: [1, 2]
        include:
          - platform: ubuntu-x86_64
            runner: ubuntu-24.04
            compiler: g++-14
          - platform: ubuntu-arm64
            runner: ubuntu-24.04-arm
            compiler: g++-14
          - platform: macos-arm64
            runner: macos-15
            compiler: clang++
    runs-on: ${{ matrix.runner }}
    timeout-minutes: 90
    env:
      CXX: ${{ matrix.compiler }}
      UOS_CI_JOBS: "4"
      UOS_CI_BUILD_DIR: ${{ runner.temp }}/uos-gateway-build
    steps:
      - uses: actions/checkout@11d5960a326750d5838078e36cf38b85af677262
      - name: Install Linux prerequisites
        if: runner.os == 'Linux'
        run: |
          sudo apt-get update
          sudo apt-get install -y g++-14 clang ninja-build pkg-config mold \
            libboost-json-dev libboost-container-dev libssl-dev libsqlite3-dev \
            zlib1g-dev libicu-dev libbrotli-dev libzstd-dev util-linux
      - name: Prepare the native SQLite prerequisite
        if: runner.os == 'Linux'
        shell: bash
        run: |
          mkdir -p "$RUNNER_TEMP/sqlite"
          curl --fail --location https://www.sqlite.org/2026/sqlite-autoconf-3540000.tar.gz \
            -o "$RUNNER_TEMP/sqlite/source.tar.gz"
          printf '%s  %s\n' 134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed \
            "$RUNNER_TEMP/sqlite/source.tar.gz" | sha256sum --check
          tar -xzf "$RUNNER_TEMP/sqlite/source.tar.gz" -C "$RUNNER_TEMP/sqlite"
          cd "$RUNNER_TEMP/sqlite/sqlite-autoconf-3540000"
          ./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
          make -j2
          sudo make install
          sudo ldconfig
      - name: Install macOS prerequisites
        if: runner.os == 'macOS'
        run: brew install ninja pkgconf boost openssl@3 sqlite zlib icu4c brotli zstd
      - name: Build and run selected shard
        shell: bash
        run: |
          if [[ "$RUNNER_OS" == macOS ]]; then
            export PKG_CONFIG_PATH="$(brew --prefix openssl@3)/lib/pkgconfig:$(brew --prefix sqlite)/lib/pkgconfig:$(brew --prefix zlib)/lib/pkgconfig:$(brew --prefix icu4c)/lib/pkgconfig"
            export UOS_CI_CXXFLAGS="-I$(brew --prefix boost)/include"
            export UOS_CI_LDFLAGS="-L$(brew --prefix boost)/lib"
          fi
          cpp/gateway/tools/ci-build.sh --config '${{ matrix.config }}' --shard '${{ matrix.shard }}/2'
      - name: Preserve exact diagnostics and results
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: cpp-${{ matrix.platform }}-${{ matrix.config }}-${{ matrix.shard }}
          path: |
            ${{ runner.temp }}/uos-gateway-build/**/receipt.json
            ${{ runner.temp }}/uos-gateway-build/**/results.tsv
            ${{ runner.temp }}/uos-gateway-build/**/*.log
```

Each shard independently compiles the production objects once, then only its selected test/helper objects. Choose the shard count deliberately: more shards reduce elapsed time but repeat production compilation and spend additional Actions minutes. No hosted workflow was dispatched for this task.
