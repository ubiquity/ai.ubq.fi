# Native gateway functional conformance

Run the packaged ARM64 release and the TypeScript application on Bun 1.4.0 against the same loopback mock. The suite uses the existing request-path benchmark build, packager, launch arguments, isolation and DeepSeek fixtures. It uses the production-smoke Bun launcher, which serves through `#http` and pinned `ws@8.18.3`. Both runtimes open their own new, codec-marked SQLite state and use ordinary authenticated routes. They restart once for durable readback.

## Run on Linux ARM64

On a disposable ARM64 host, prepare GCC 14 or newer or Clang 21 or newer, Python 3.11+, Bun 1.4.0, `rustup`, `jq`, and the packages below. The host needs Landlock ABI 8+ and passwordless `sudo` for the benchmark's network/PID namespace. The worker drops back to the invoking uid before starting the gateway processes.

```sh
sudo apt-get update
sudo apt-get install -y g++-14 ninja-build mold pkg-config jq libssl-dev zlib1g-dev \
  libicu-dev libbrotli-dev libzstd-dev util-linux iproute2
scratch=$(mktemp -d /var/tmp/cppgw-conformance.XXXXXXXX)
```

Build the Boost archives in scratch. The benchmark build reads their staged headers and static libraries from that source directory.

```sh
(
  cd "$scratch"
  curl -fLSs -o boost_1_92_0.tar.bz2 https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2
  printf '%s  %s\n' 5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c boost_1_92_0.tar.bz2 | sha256sum -c -
  tar -xjf boost_1_92_0.tar.bz2
  cd boost_1_92_0
  ./bootstrap.sh --with-libraries=json,container
  ./b2 -j2 --with-json --with-container variant=release link=static threading=multi toolset=gcc-14 stage
)
```

Use SQLite 3.54.0 with `SQLITE_DQS=3` from the portable build recipe and ICU 78.3, the version used by this native baseline. The immutable packager resolves host libraries under `/usr/lib`, including the distro's multiarch directory. It expects the SQLite SONAME `libsqlite3.so.0`. On Ubuntu 24.04, the following places the new libraries in the qualified directory and leaves the distro library files available at their original paths. [SQLite preparation](CI.md#ubuntu-prerequisites)

```sh
(
  cd "$scratch"
  curl -fLSs -o sqlite.tar.gz https://www.sqlite.org/2026/sqlite-autoconf-3540000.tar.gz
  printf '%s  %s\n' 134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed sqlite.tar.gz | sha256sum -c -
  tar -xzf sqlite.tar.gz
  cd sqlite-autoconf-3540000
  ./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
  make -j1 LDFLAGS.rpath= 'LDFLAGS.libsqlite3.soname=-Wl,-soname,libsqlite3.so.0'
  sudo make install LDFLAGS.rpath= 'LDFLAGS.libsqlite3.soname=-Wl,-soname,libsqlite3.so.0'
  sudo install -m 0755 libsqlite3.so /usr/lib/libsqlite3.so.3.54.0
  sudo ln -sfn libsqlite3.so.3.54.0 /usr/lib/libsqlite3.so.0
  sudo ln -sfn /usr/lib/libsqlite3.so.3.54.0 /usr/local/lib/libsqlite3.so.3.54.0
)
(
  cd "$scratch"
  curl -fLSs -o icu.tgz https://github.com/unicode-org/icu/releases/download/release-78.3/icu4c-78.3-sources.tgz
  printf '%s  %s\n' 3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0 icu.tgz | sha256sum -c -
  tar -xzf icu.tgz
  cd icu/source
  ./configure --prefix=/usr/local --disable-tests --disable-samples --disable-extras --disable-static
  make -j2
  sudo make install
  for name in icudata icui18n icuuc; do
    library="lib$name.so.78.3"
    sudo install -m 0755 "/usr/local/lib/$library" "/usr/lib/$library"
    sudo ln -sfn "/usr/lib/$library" "/usr/local/lib/$library"
    sudo ln -sfn "$library" "/usr/lib/lib$name.so.78"
  done
)
printf '%s\n' /usr/lib | sudo tee /etc/ld.so.conf.d/00-cppgw-conformance.conf >/dev/null
sudo ldconfig
```

From the clean checkout, run one command.

```sh
bash cpp/gateway/conformance/run.sh "$scratch"
```

The command snapshots committed source, installs frozen Bun dependencies in scratch, prepares the genuine native KV and permissions artifacts, builds the release package, and then runs the suite. Dependency preparation can use the network. The test namespace has only loopback, proves outbound IPv4 and IPv6 route denial, and gives each gateway process a new empty home and temp directory. Test processes receive an allowlisted environment with synthetic auth for the mock routes. Bun launches with `--no-env-file --no-install`.

On the supplied base, the benchmark directory is present on the port integration branch. The command extracts its pinned public Git commit `6878261fd50c7df210308879583996bc88311724` in scratch if the benchmark is absent from the checkout. A merged checkout that has the benchmark uses its own committed files. The receipt records the helper revision. An existing scratch directory can rerun the same application snapshot with new runtime state and new output. `CPP_CONFORMANCE_SOURCE_REVISION` allows reuse after conformance-only commits and verifies the rest of the committed tree against that source revision. `CPP_CONFORMANCE_BOOST`, `CPP_CONFORMANCE_LIBRARY_PATH`, `CPP_CONFORMANCE_JOBS`, `CXX` and `BUN` select prepared dependencies or tools.

## Cases and evidence

| Surface                        | Exercises                                                                                                                                                                                |
| ------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Chat Completions and Responses | Buffered and progressive SSE, text and tool calls, provider 429/503, and error after a delivered delta                                                                                   |
| Providers                      | DeepSeek and Codex inference, exact-version Codex catalog fetch and cache, DeepSeek waterfall first hop, and unavailable-provider decisions                                              |
| Embeddings                     | UOS validation and unconfigured provider responses, then cached float and base64, idempotency and job ownership. The suite imports two synthetic vector rows through the admin HTTP API. |
| Transport and access           | Authentication and CORS, oversized and malformed bodies, public and unknown routes, health, admin access, and Codex Realtime SDP and WebSocket relay                                     |
| State                          | API-key creation, rename, revoke, delete and audit, a one-request quota that persists across restart, empty paid-fallback history, and correlated delivery terminals                     |

Successful out-of-process Voyage inference and configured Cerebras, Lithos, OpenRouter, Claude Code, Surplus and OpenLux inference require provider routing hooks beyond the packaged mock map. Paid fallback accumulation requires those hooks too. The WebSocket probe observes the upgrade status, headers and the authenticated text round trip.

Cases are in [cases.json](../conformance/cases.json). A case names a method, path, credential alias and optional body or benchmark fixture. The mock mode selects a fixed upstream script. `expect.status` is required. `expect.routes` asserts the dispatch sequence, `expect.values` checks selected JSON Pointers in the observation, and `expect.absent` asserts that fields are omitted. `expect.media_type` selects a media type such as `text/event-stream` or `application/sdp`. Ordinary HTTP responses default to JSON. `capture` saves named response values for later requests. For example, each gateway supplies its own generated key ID to the same revoke request template using `$other_id`. Use `$$` for a literal dollar sign in a string value. A case tagged `log` can require one terminal record and inspect its semantic type, delivered outcome and token usage.

Status, public response bodies and relevant headers are compared for every case. SSE comparisons include named events, JSON payloads in order, sequence numbers and the final event. The progressive probes hold the mock's subsequent frames until the client receives the first delta. The comparator preserves error codes, presence, types and consequential ordering. It maps generated IDs consistently, normalizes explicitly named clocks, and allows two seconds of clock drift for retry/reset headers. Error wording, equivalent header formatting and explicitly unordered list permutations are cosmetic. An expectation can fail on both runtimes even when their wire responses match.

The command prints the new output directory. `results.json` holds the commit and tree, fixture and binary hashes, both observations, dispatch evidence, expectation failures and field-level differences. `table.md` summarizes the cases. Synthetic credential values are redacted in the receipt. Exit `0` means all functional checks passed, `1` means the suite completed and found a functional failure, and `2` means the comparison runner could not complete. Setup errors and the watchdog can also exit nonzero. [Measured baseline and complete table](findings/cppgw-conformance-e2e-20261118.md)
