# Native C++ gateway

The native gateway runs compiled C++20. It ports the repository's TypeScript gateway, which runs on Bun, and serves
the same gateway APIs, including OpenAI Chat Completions and Responses. The port preserves wire formats, status
codes, stored data, authentication, validation and accounting as application contracts. Generic operations use
standard or mature open-source libraries where they meet those contracts, including the required security checks.
Differences in diagnostic wording are expected. [TypeScript source](../../src/) ·
[TypeScript entry point](../../serve.ts) · [Architecture](docs/ARCHITECTURE.md)

## Where it stands

| Platform      | Recorded result                                                                                                                                                                                                                                                            |
| ------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Linux ARM64   | **441/441 Release binaries and 5,031/5,031 cases passed** at the suite-repair source and test commit `380a9b0a`. Both ARM64 startup fixtures passed. A scoped sanitizer run passed 19 binaries and 347 cases. [Suite repair](docs/findings/cppgw-suite-repair-20261118.md) |
| Linux x64     | The portable driver lists 441 binaries. `permissions_linux_test` and `runtime_native_startup_test` exercise Linux ARM64 contracts. [This guide's verification](docs/findings/cppgw-docs-readme-20261118.md)                                                                |
| Apple Silicon | **441/441 binaries and 5,029/5,029 cases passed** in the recorded macOS 26.7.2 development build at `c6cd29f9`. Packaging and service startup still need qualification. [macOS results](docs/MACOS.md)                                                                     |
| Raspberry Pi  | A target for a 64-bit ARM build. The OS, loader, kernel confinement and memory headroom still need checking on the device. [Platform requirements](docs/BUILD.md)                                                                                                          |

The Linux ARM64 suite-repair result is the latest full suite recorded for this source. The older **5,024-case**
result belongs to the separately packaged GCC release. For current JSON, Base64 and input limits, this guide
follows the source and vendor registry. The package and benchmark documents describe their own builds.
[Suite repair](docs/findings/cppgw-suite-repair-20261118.md) ·
[Package evidence](docs/BUILD.md) · [Vendor registry](cmake/vendors.json)

### Speed and memory versus Bun

- In the Linux ARM64 request-path benchmark with four local clients and a scripted upstream, C++ delivered
  **1.19–1.88×** Bun 1.4.0's median requests per second in five of six scenarios. Peak gateway memory was
  **33–40 MiB vs 112–135 MiB**, about a third. The 1 MiB Chat request reached **155.8 vs 167.6 requests/s**,
  using **6.69 vs 9.25 ms** of gateway CPU per request and **103 vs 206 MiB** peak memory.
  [Request-path benchmark](docs/findings/cppgw-perf-request-path-20261117.md)
- On the separately sealed ARM64 package, paired smoke tests measured **39.4 vs 98.7–99.2 MiB** settled idle memory
  and **41.6 vs 120.6–124.6 MiB** peak with eight concurrent requests. Quiet serial buffered Chat had a median of
  **2.81 vs 10.02 ms**. Bun had lower warm parse and stringify times on the three documents in that package's JSON benchmark.
  [Package performance](docs/PERFORMANCE.md)

## Build and test on Linux ARM64

From the repository root on a disposable Ubuntu 26.04 ARM64 host, use the portable driver with Clang and Ninja.
On the Arch Linux ARM release host, use its prepared native build profile for package construction.
[Portable CI prerequisites](docs/CI.md) · [Arch release profile](docs/BUILD.md)

Install the development libraries. The SQLite step also needs `curl`, a C compiler and `make` on the host.

```sh
sudo apt-get update
sudo apt-get install -y clang ninja-build pkg-config libboost-json-dev libboost-container-dev \
  libssl-dev libsqlite3-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev util-linux mold
```

The SQLite suite expects 3.54.0 with double-quoted-string support. On the disposable runner, the following built
the pinned library into `/usr/local`. The final command printed `3.54.0`.

```sh
(
  sqlite_work=$(mktemp -d /tmp/cppgw-guide-sqlite.XXXXXX) &&
  curl --fail --location https://www.sqlite.org/2026/sqlite-autoconf-3540000.tar.gz -o "$sqlite_work/sqlite.tar.gz" &&
  printf '%s  %s\n' 134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed "$sqlite_work/sqlite.tar.gz" | sha256sum --check &&
  tar -xzf "$sqlite_work/sqlite.tar.gz" -C "$sqlite_work" &&
  cd "$sqlite_work/sqlite-autoconf-3540000" &&
  ./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3' &&
  make -j2 && sudo make install && sudo ldconfig &&
  pkg-config --modversion sqlite3
)
```

List the tests, then build and run Release. The first command printed `441`. The driver builds common objects once
and links each standalone test. It uses real Linux namespaces with loopback-only networking, a fresh temp filesystem
and an empty home for every test. It tries unprivileged namespaces, then passwordless `sudo` for namespace setup.
[Driver and isolation](docs/CI.md)

```sh
bash cpp/gateway/tools/ci-build.sh --list | wc -l
build_dir=$(mktemp -d /tmp/uos-gateway-build.XXXXXX)
env -i PATH="$PATH" LC_ALL=C LD_LIBRARY_PATH=/usr/local/lib CXX=clang++ UOS_CI_JOBS=4 \
  UOS_CI_BUILD_DIR="$build_dir" bash cpp/gateway/tools/ci-build.sh --config release >"$build_dir/driver.log" 2>&1
```

The build directory holds `driver.log`, and the compiler-specific `release/` directory holds `receipt.json`,
`results.tsv` and each test's build and run logs. Its outputs are for testing. Package construction uses the native
build profile. [Build and test entry points](docs/BUILD.md) · [Verification report](docs/findings/cppgw-docs-readme-20261118.md)

## Run a sealed Linux ARM64 package

Prepare the ARM64 package for the target's host libraries using the native build and packaging instructions. Set
`package_root` to its absolute path and `launch_manifest` to the absolute path of the owner-private JSON. The
manifest binds the package and native KV provenance hashes, the private state root and the loopback listener.
Normal startup needs an existing private `.data/kv-bun.sqlite3` with the Bun codec marker in that state root. The isolated package smoke guide
describes the separate fixture mode with fresh state and a scripted upstream. [Package instructions](docs/RUNTIME.md) ·
[Launch schema](runtime/launch-schema.json) · [Package smoke](tests/production_smoke/README.md)

```sh
bash "$package_root/ops/launch-native.sh" "$launch_manifest"
```

The x64 runner exercised this command against the compiled entry point in isolation. It exited `1` with
`package manifest integrity check failed` for an intentionally wrong hash. The recorded paired ARM64 package smoke
supplies the successful startup evidence. The current operator files are an uninstalled draft. The candidate binds
to `127.0.0.1` and refuses service ports `7999` and `8000`. [Launch probe](docs/findings/cppgw-docs-readme-20261118.md) ·
[Smoke results](docs/PERFORMANCE.md) · [Operations draft](ops/README.md)

### Configuration

The launch manifest selects package, state and listener. The process reads the existing gateway environment for
authentication, provider settings and identity, including `UOS_AI_TOKEN`, `CODEX_AUTH_JSON_B64` and `DENO_TIMELINE`.
`DENO_TIMELINE=production` disables the legacy localhost authentication bypass. The binary does no implicit
`.env` loading. The draft service supplies an explicit operator-owned environment file, and its use needs cutover
review. [Configuration source](src/config.cpp) · [Auth policy](src/auth/index.cpp) · [Environment inventory](docs/RUNTIME.md) ·
[Draft unit](ops/uos-gateway-native.service)

### Security in five points

1. Startup verifies the sealed package, host libraries and native artifacts before opening KV or binding the listener. [Startup](runtime/startup.cpp)
2. Linux ARM64 startup requires Landlock ABI 8+ and seccomp thread synchronization and refuses unsupported hosts. [Confinement](src/runtime/permissions_linux.cpp)
3. The service reads the package, private state and required OS files. It writes under private `.data` and cannot execute subprocesses. [Startup enforcement](runtime/startup.cpp)
4. The loopback service applies the gateway's authentication, validation and admission rules at its routes. [Request routing](src/handler/index.cpp)
5. HTTP heads cap at 16,408 bytes and 198 fields. Default JSON parsing caps at 16 MiB and 128 levels, with route-specific limits including allowances for image edits. [HTTP parser](src/runtime/http.cpp) · [Limits](include/uos/rt/limits.hpp)

## Libraries in use

The versions below are from the native Linux ARM64 profile and the checked-in vendor registry. The portable driver
uses installed versions of the host libraries. [Release toolchain](docs/BUILD.md) · [Vendor registry](cmake/vendors.json)

| Library  | Version         | What it provides or replaces                                                                                                                                                                                                                                                                   |
| -------- | --------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| simdjson | 5.2.0, vendored | On-Demand JSON parsing replaces the handwritten parser. The adapter retains input limits and the required value conversion. [Parser](src/rt/json_simdjson.hpp) · [Provenance](third_party/simdjson/PROVENANCE.md)                                                                              |
| simdutf  | 9.4.0, vendored | Base64 encoding and decoding, UTF validation and text conversion replace generic byte loops. The runtime keeps streaming and malformed-text handling. [Provenance](third_party/simdutf/PROVENANCE.md)                                                                                          |
| Boost    | 1.92.0          | Asio supplies sockets, the event loop and coroutine support. Boost.JSON supplies values and serializer primitives, with Boost.Container linked alongside it. Boost.Algorithm handles hex conversion. [Async](include/uos/rt/async.hpp) · [JSON](src/rt/json.cpp) · [Crypto](src/rt/crypto.cpp) |
| ICU      | 78.3            | UTS #46 hostname normalization through the platform Unicode library. [URL adapter](src/rt/url.cpp)                                                                                                                                                                                             |
| OpenSSL  | 3.6.5           | TLS, certificate verification and cryptographic primitives behind the native transport and crypto APIs. [TLS](src/rt/tls.cpp) · [Crypto](src/rt/crypto.cpp)                                                                                                                                    |
| SQLite   | 3.54.0          | Database engine under the linked KV implementation, replacing the JavaScript runtime's SQLite binding. [KV backend](src/rt/kv_sqlite.cpp)                                                                                                                                                      |
| zlib     | 1.3.2           | Gzip streams and gzip/deflate decoding for Fetch. [Gzip](src/rt/gzip.cpp) · [Fetch decoding](src/rt/fetch_transport_codec.cpp)                                                                                                                                                                 |
| Brotli   | 1.2.0           | Brotli decoding for Fetch. [Fetch decoding](src/rt/fetch_transport_codec.cpp)                                                                                                                                                                                                                  |
| Zstd     | 1.5.7           | Zstandard decoding for Fetch. [Fetch decoding](src/rt/fetch_transport_codec.cpp)                                                                                                                                                                                                               |

## Open items

- The storage reader expands sparse arrays and rejects cycles it cannot represent. Cutover still needs complete
  decoding on a stopped copy, verified reverse conversion of the latest writes, and durability evidence. The
  packaged tools run conversion separately from service startup. [Storage cutover](docs/OPEN-STAGES.md) ·
  [Durable KV](docs/rt/kv-durable.md)
- The native Fetch client uses HTTP/1.1 and has no proxy support. The loopback server has no TLS termination of
  its own. Some buffered-body transfer framing and gzip bytes differ from Bun. The recorded request-path tests
  use mocks. Live provider, client and production cutover acceptance remain open. [Transport](docs/rt/fetch-transport.md) ·
  [Open stages](docs/OPEN-STAGES.md)
- The open-source replacement plan tracks URL, HTTP, regex, WebSocket, passkey and other library work. The current
  library list comes from the code and build registry. Production activation needs separate approval, including
  credential, release and applicable Sentinel-reader acceptance. [Replacement plan](docs/OSS-REPLACEMENT-PLAN.md) ·
  [Cutover requirements](docs/RUNTIME.md)

## Document map

- [Architecture](docs/ARCHITECTURE.md) follows a request, coroutine ownership and storage from the code.
- [CI](docs/CI.md) gives portable commands, prerequisites, isolation details and receipt locations.
- [Build](docs/BUILD.md) covers release toolchains, native package targets and build measurements.
- [Runtime](docs/RUNTIME.md) specifies the package, launch manifest, limits, environment and cutover gates.
- [Operations draft](ops/README.md) describes the uninstalled launcher and service unit.
- [Package smoke](tests/production_smoke/README.md) runs local authenticated checks on a sealed package.
- [Testing](docs/TESTING.md) explains test link lists and the original SDK sweep.
- [macOS](docs/MACOS.md) reproduces the Apple Silicon development build and its platform checks.
- [Performance](docs/PERFORMANCE.md) gives the sealed-package measurements and component comparisons.
- [Open stages](docs/OPEN-STAGES.md) tracks remaining platform, storage and production acceptance.
- [Deviations from Bun](docs/DEVIATIONS-FROM-BUN.md) records intentional runtime behavior differences.
- [OSS replacement plan](docs/OSS-REPLACEMENT-PLAN.md) shows library candidates and their acceptance needs.
- [Porting rules](PORTING.md) define module naming, coroutine safety and test conventions.
- [Port notes](docs/PORT-NOTES.md) give module decisions and the history of the porting waves.
- [Release repairs](docs/RELEASE-REPAIRS.md) traces fixes made during release and sanitizer qualification.

### Runtime references

- [Async](docs/rt/async.md) explains tasks, promise ordering, timers, aborts and streams.
- [Web](docs/rt/web.md) maps URL, headers, request and response objects to C++.
- [Fetch transport](docs/rt/fetch-transport.md) covers HTTP wire behavior, TLS, connection pooling and limits.
- [WebSocket](docs/rt/websocket.md) covers the client, server, upgrades and `ws` contracts.
- [Text](docs/rt/text.md) covers strings, encodings, Base64 and dates.
- [Values](docs/rt/values.md) explains JSON, numbers, collections and nullable values.
- [Record](docs/rt/record.md) explains conversion between typed records and JSON.
- [Regex](docs/rt/regex.md) documents supported expressions, indices, state and matcher limits.
- [Platform](docs/rt/platform.md) is the reference for crypto, environment, process, file and console APIs.
- [Gzip](docs/rt/gzip.md) covers streaming compression and Fetch decoding.
- [SQLite](docs/rt/sqlite.md) covers the database API, errors and library version differences.
- [KV](docs/rt/kv.md) explains the store API, typed values, keys and test helpers.
- [Durable KV](docs/rt/kv-durable.md) gives the on-disk schema, interoperability evidence and operational limits.
- [Durable KV design](docs/rt/kv-durable-design.md) explains why the port links its own SQLite-backed adapter.

### Findings and benchmark instructions

- [Guide verification](docs/findings/cppgw-docs-readme-20261118.md) records the commands, x64 results and documentation discrepancies.
- [Suite repair](docs/findings/cppgw-suite-repair-20261118.md) has the latest ARM64 Release and scoped sanitizer counts.
- [CI workflow](docs/findings/cppgw-ci-workflow-20261118.md) gives earlier shard diagnostics and artifact handling.
- [Portable x64 build](docs/findings/cppgw-ci-build-x86-20261117.md) traces the original driver qualification.
- [Native macOS](docs/findings/cppgw-macos-native-20261117.md) has the exact development run and dependency decisions.
- [libc++ portability](docs/findings/cppgw-libcxx-portability-20261117.md) traces the standard-library portability fixes.
- [Request-path performance](docs/findings/cppgw-perf-request-path-20261117.md) has the paired throughput and memory data.
- [Ada URL](docs/findings/cppgw-oss-ada-url-20261117.md) records work on the URL replacement candidate.
- [Clang-tidy audit](docs/findings/cppgw-clang-tidy-audit-20261117.md) gives static analysis results and their scope.
- [Coroutine audit](docs/findings/local-coroutine-lifetime-audit.md) lists lifetime sites and dispositions.
- [Runtime ASan](docs/findings/cppgw-asan-rt-20261117.md) records sanitizer findings in runtime primitives.
- [Core ASan](docs/findings/cppgw-asan-core-20261117.md) records sanitizer findings in core request handling.
- [Authority ASan](docs/findings/cppgw-asan-authority-20261117.md) records sanitizer findings in authority and admission.
- [Codex ASan](docs/findings/cppgw-asan-codex-20261117.md) records sanitizer findings in Codex routing and supervisor code.
- [Provider ASan](docs/findings/cppgw-asan-providers-20261117.md) records sanitizer findings in provider paths.
- [Storage ASan](docs/findings/cppgw-asan-storage-20261117.md) records sanitizer findings in storage and conversion.
- [Request benchmark](bench/request_path/README.md) reproduces the end-to-end loopback comparison.
- [JSON benchmark](bench/paths/README.md) reproduces the JSON component timings.
- [Regex benchmark](bench/regex/README.md) reproduces the regex component timings.
