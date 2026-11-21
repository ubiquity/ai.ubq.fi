# Native gateway memory under Codex CLI traffic

## Checklist

- [x] Identify the integration base and qualify the runner, SQLite and namespace prerequisites.
- [x] Implement a loopback reproduction with a populated Bun/JSC KV database.
- [ ] Measure Release memory at startup and after 1, 10, 100 and 1,000 sessions. Isolate the catalog and completion paths.
- [ ] Profile allocation peaks and retained bytes, and measure the effect of `malloc_trim(0)`.
- [ ] Fix measured waste and repeat the same workload after each fix.
- [ ] Add the Release budget check, document small-board use, and record the focused, shard and registration gates.

## Runner

The starting tree is `8f11c1a264bcdd6bef668139bf1a59031dd962fe` on `claude/cpp-gateway-port`. The runner has four ARM64 CPUs, Ubuntu 26.04.1, Linux 7.0.0-1020-azure, glibc 2.43, Clang 21.1.8, Boost 1.90, ICU 78.2 and Heaptrack 1.5.0. The job's compiler cache is in use.

The runner used the [C++ workflow dependencies](../../../../.github/workflows/cpp-gateway-ci.yml) and built SQLite 3.54.0 under `/usr/local` using the workflow's tarball SHA-256 `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed`, `./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'` and `make -j4`. `pkg-config --modversion sqlite3` returned `3.54.0`. The runner permits fresh network, mount and PID namespaces through noninteractive sudo. Measurements use loopback mocks and an environment created from an allowlist.

## Reproduction

The [benchmark driver](../../bench/codex_memory/README.md) uses the existing package launcher harness and synthetic production-smoke auth and model fixtures. It seeds exact-version catalog chunks through the gateway, restarts on the populated database, verifies local dispatch and validates text, token usage and delivery. Its synthetic catalog repeats an 8 KiB instruction template across 535 upstream records. Production catalog bytes were unavailable.

Release objects build via `CC=clang CXX=clang++ UOS_CI_JOBS=4 bash cpp/gateway/tools/ci-build.sh --config release --shard 1/4` with a fresh empty build home and temp, the restored compiler cache and a scratch `UOS_CI_BUILD_DIR`. The package uses those Release production objects and vendor archives, plus `runtime/startup.cpp`, `entrypoints/gateway.cpp` and `src/release.cpp` built with the same flags and the selected Git SHA. The benchmark package producer seals the binary, assets and host closure from a Git archive of that SHA. A private mount overlays the Ubuntu SQLite library path with the pinned library for sealing and execution. The pinned library SHA-256 is `2fb9976405cd8f7afcc474acc90b93c087c4ced76b5302ef75dcd79279f9b238`. The host's package files are outside that mount namespace.
