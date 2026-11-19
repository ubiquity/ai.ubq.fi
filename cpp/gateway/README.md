# Native C++ gateway

The C++20 gateway serves OpenAI-compatible APIs, operator routes and browser assets. It uses
a linked SQLite store and native Linux confinement. The repository contains the gateway, C++ tests and fixtures,
native build and packaging scripts, and browser assets. [Architecture](docs/ARCHITECTURE.md)

## Build and test on Linux ARM64

From the repository root on a disposable Ubuntu 26.04 ARM64 host, use the portable driver with Clang and Ninja.
On the Arch Linux ARM release host, use its prepared native build profile for package construction.
[Portable CI prerequisites](docs/CI.md) · [Arch release profile](docs/BUILD.md)

Install the development libraries. The SQLite step also needs `curl`, a C compiler and `make` on the host.

```sh
sudo apt-get update
sudo apt-get install -y clang ninja-build pkg-config libboost-json-dev libboost-container-dev \
  libssl-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev util-linux mold
```

On Ubuntu 24.04, prepare Clang 21, Boost 1.92.0 and ICU 78.3 as described in [CI](docs/CI.md).

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

List the registered tests, then build and run a focused Release fixture. The driver builds common objects once
and links each standalone test. It uses real Linux namespaces with loopback-only networking, a fresh temp filesystem
and an empty home for every test. It tries unprivileged namespaces, then passwordless `sudo` for namespace setup.
[Driver and isolation](docs/CI.md)

```sh
bash cpp/gateway/tools/ci-build.sh --list | wc -l
build_dir=$(mktemp -d /tmp/uos-gateway-build.XXXXXX)
mkdir "$build_dir/home" "$build_dir/tmp"
env -i PATH=/usr/local/bin:/usr/bin:/bin LC_ALL=C HOME="$build_dir/home" TMPDIR="$build_dir/tmp" \
  CC=clang CXX=clang++ UOS_CI_JOBS=4 \
  UOS_CI_BUILD_DIR="$build_dir" bash cpp/gateway/tools/ci-build.sh --config release oss_gateway_http_test >"$build_dir/driver.log" 2>&1
```

The build directory holds `driver.log`, and the compiler-specific `release/` directory holds `receipt.json`,
`results.tsv` and each test's build and run logs. Build the portable package separately with Docker Buildx.
[Build and test entry points](docs/BUILD.md) · [Portable packages](docs/RELEASES.md)

## Run a sealed Linux ARM64 package

Prepare the ARM64 package for the target's host libraries using the native build and packaging instructions. Set
`package_root` to its absolute path and `launch_manifest` to the absolute path of the owner-private JSON. The
manifest binds the package hash, the private state root and the loopback listener.
Normal startup needs an existing private `.data/kv-bun.sqlite3` with the `bun-jsc-v1` marker in that state root.
The isolated package smoke guide describes the separate fixture mode with fresh state and a scripted upstream.
[Package instructions](docs/RUNTIME.md) ·
[Launch schema](runtime/launch-schema.json) · [Package smoke](tests/production_smoke/README.md)

```sh
bash "$package_root/ops/launch-native.sh" "$launch_manifest"
```

The candidate binds to IPv4 loopback and reserves ports `7999` and `8000`. See the
[operator checklist](docs/CUTOVER.md) for the Arch Linux ARM rehearsal and health verification.

### Configuration

The launch manifest selects package, state and listener. The process reads the existing gateway environment for
authentication, provider settings and identity, including `UOS_AI_TOKEN`, `CODEX_AUTH_JSON_B64` and `DENO_TIMELINE`.
`DENO_TIMELINE=production` disables the legacy localhost authentication bypass. The binary reads the process
environment. Set `EnvironmentFile=` to the operator-owned environment file in the service unit. Its use needs cutover
review. [Configuration source](src/config.cpp) · [Auth policy](src/auth/index.cpp) · [Environment inventory](docs/RUNTIME.md) ·
[Draft unit](ops/uos-gateway-native.service)

### Security in five points

1. Startup verifies the sealed package, and host libraries before opening KV or binding the listener. [Startup](runtime/startup.cpp)
2. Linux ARM64 startup requires Landlock ABI 8+ by default. On ABI 3–7, `UOS_GATEWAY_ALLOW_REDUCED_CONFINEMENT=1` explicitly selects the controls that the kernel supports. Seccomp thread synchronization remains required. [Confinement](docs/RUNTIME.md#linux-confinement)
3. By default the service reads the package, private state and required OS files, writes under private `.data` and denies subprocesses. The Antigravity setting explicitly enables the CLI and its tighter child filesystem profile. [Confinement](docs/RUNTIME.md#linux-confinement)
4. The loopback service applies the gateway's authentication, validation and admission rules at its routes. [Request routing](src/handler/index.cpp)
5. HTTP heads cap at 16,408 bytes and 198 fields. Default JSON parsing caps at 16 MiB and 128 levels, with route-specific limits including allowances for image edits. [HTTP parser](src/runtime/http.cpp) · [Limits](include/uos/rt/limits.hpp)

## Libraries in use

The table lists the release build pins and the shared libraries supplied by the target OS. [Release packages](docs/RELEASES.md) · [Vendor registry](cmake/vendors.json)

| Library              | Release version    | Responsibility                                                                                                                                                                                                                                      |
| -------------------- | ------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| simdjson             | 5.2.0, vendored    | Bounded On-Demand JSON parsing into owned Boost.JSON values. [Parser](src/rt/json_simdjson.hpp)                                                                                                                                                     |
| simdutf              | 9.4.0, vendored    | Base64, UTF validation and text conversion. [Provenance](third_party/simdutf/PROVENANCE.md)                                                                                                                                                         |
| llhttp               | 9.4.4, vendored    | HTTP server request parsing and outbound Fetch and WebSocket response parsing. [Provenance](third_party/llhttp/PROVENANCE.md)                                                                                                                       |
| Ada                  | 4.0.0, vendored    | URL parsing, IDNA and URL search parameters. [URL adapter](src/rt/url.cpp)                                                                                                                                                                          |
| QCBOR                | 1.6.1, vendored    | CBOR encoding and strict decoding for passkeys. [Passkeys](src/auth/passkeys.cpp)                                                                                                                                                                   |
| QuickJS-ng libregexp | 0.18.0, vendored   | Bounded ECMAScript matching and Unicode sets. [Regex adapter](docs/rt/regex.md)                                                                                                                                                                     |
| Boost                | 1.92.0             | Asio for I/O and coroutines, Beast for WebSocket handshake and frame primitives, JSON and Container for values, and Multiprecision for BigInt. [Async](include/uos/rt/async.hpp) · [WebSocket](docs/rt/websocket.md) · [Numbers](src/rt/number.cpp) |
| ICU                  | 78.3               | Unicode casing and collation. [Text implementation](src/rt/string.cpp)                                                                                                                                                                              |
| OpenSSL              | 3, OS library      | TLS, crypto and passkey ASN.1, X.509 and CRL decoding. [TLS](src/rt/tls.cpp) · [Passkeys](src/auth/passkeys.cpp)                                                                                                                                    |
| SQLite               | 3.54.0             | Database engine for the KV backend. [KV backend](src/rt/kv_sqlite.cpp)                                                                                                                                                                              |
| zlib, Brotli, Zstd   | OS libraries       | Gzip streams and HTTP content decoding. [Gzip](src/rt/gzip.cpp) · [Fetch decoding](src/rt/fetch_transport_codec.cpp)                                                                                                                                |
| C++20 and POSIX      | Standard libraries | Dates, filesystem paths, numeric conversion, collections and process environment. [Completed replacements](docs/OSS-REPLACEMENT-PLAN.md#ranked-component-inventory)                                                                                 |

## Open items

- The coroutine lifetime audit and rollout of the tighter input limits are in progress.
  [Coroutine audit](docs/findings/local-coroutine-lifetime-audit.md) · [Input limits](docs/RUNTIME.md#input-limits)
- The storage reader preserves sparse entries, references and cycles in the typed graph. Projection to dense JSON
  can materialize holes. Cutover still needs complete decoding on a stopped copy, verified reverse conversion of
  the latest writes, and durability evidence. [Storage cutover](docs/OPEN-STAGES.md) ·
  [Durable KV](docs/rt/kv-durable.md)
- The native Fetch client uses HTTP/1.1 and has no proxy support. The loopback server has no TLS termination of
  its own. Some buffered-body transfer framing and gzip bytes differ from Bun. The recorded request-path tests
  use mocks. Live provider, client and production cutover acceptance remain open. [Transport](docs/rt/fetch-transport.md) ·
  [Open stages](docs/OPEN-STAGES.md)
- Production activation needs separate approval, including credential, release and applicable Sentinel-reader
  acceptance. The remaining runtime library work is tracked in the replacement plan. [Replacement plan](docs/OSS-REPLACEMENT-PLAN.md) ·
  [Cutover requirements](docs/RUNTIME.md)

## Document map

- [Architecture](docs/ARCHITECTURE.md) follows a request, coroutine ownership and storage from the code.
- [CI](docs/CI.md) gives portable commands, prerequisites, isolation details and receipt locations.
- [Build](docs/BUILD.md) covers release toolchains, native package targets and build measurements.
- [Runtime](docs/RUNTIME.md) specifies the package, launch manifest, limits, environment and cutover gates.
- [Releases](docs/RELEASES.md) explains how to verify and install the ARM64 archives.
- [Conformance](docs/CONFORMANCE.md) links to the historical paired comparison.
- [Cutover checklist](docs/CUTOVER.md) covers the week-long observation, switch and rollback.
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
- [Final integration](docs/findings/cppgw-integrate-f2-20261118.md) has the merged ARM64 Release, sanitizer, robustness and package evidence.
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
