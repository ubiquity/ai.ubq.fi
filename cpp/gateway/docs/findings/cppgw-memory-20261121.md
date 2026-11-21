# Native gateway memory under Codex CLI traffic

## Checklist

- [x] Identify the integration base and qualify the runner, SQLite and namespace prerequisites.
- [ ] Build a repeatable loopback reproduction with a populated Bun/JSC KV database.
- [ ] Measure Release memory at startup and after 1, 10, 100 and 1,000 sessions. Isolate the catalog and completion paths.
- [ ] Profile allocation peaks and retained bytes, and measure the effect of `malloc_trim(0)`.
- [ ] Fix measured waste and repeat the same workload after each fix.
- [ ] Add the Release budget check, document small-board use, and record the focused, shard and registration gates.

## Runner

The starting tree is `8f11c1a264bcdd6bef668139bf1a59031dd962fe` on `claude/cpp-gateway-port`. The runner has four ARM64 CPUs, Ubuntu 26.04.1, Linux 7.0.0-1020-azure, glibc 2.43, Clang 21.1.8, Boost 1.90, ICU 78.2 and Heaptrack 1.5.0. The job's compiler cache is in use.

The runner used the [C++ workflow dependencies](../../../../.github/workflows/cpp-gateway-ci.yml) and built SQLite 3.54.0 under `/usr/local` using the workflow's tarball SHA-256 `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed`, `./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'` and `make -j4`. `pkg-config --modversion sqlite3` returned `3.54.0`. The runner permits fresh network, mount and PID namespaces through noninteractive sudo. Measurements use loopback mocks and an environment created from an allowlist.
