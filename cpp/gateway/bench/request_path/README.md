# Loopback gateway request path

This benchmark measures the real `uos-gateway` package and the TypeScript gateway under Bun 1.4.0. Each invocation runs
both against one synthetic HTTP/1.1 DeepSeek server. Each trial starts a gateway in an empty private state directory
and uses its normal SQLite KV, authorization and delivery lifecycle. The local provider returns fixed text and token
counts. The launcher requires a Linux network namespace with only loopback, drops back to the invoking uid and passes
a sparse environment with new `HOME` and `TMPDIR` directories to each child.

| Scenario  | Client request                          | Fixture check                                                                               |
| --------- | --------------------------------------- | ------------------------------------------------------------------------------------------- |
| Small     | `POST /v1/chat/completions`, buffered   | Text, stop and usage                                                                        |
| Large     | Same Chat endpoint, 1,048,576-byte JSON | Upstream receives every message intact. Each message fits the provider's per-message limit. |
| Stream    | Chat with `stream: true`                | Eight content deltas in order, stop, usage and `[DONE]`                                     |
| Responses | `POST /v1/responses`, buffered          | Completed Responses object, output text and usage                                           |
| Models    | Authenticated bare `GET /v1/models`     | Same model IDs and order, with zero upstream calls                                          |
| Reject    | Small Chat with no authorization        | HTTP 401 and the OpenAI-shaped error, with zero upstream calls                              |

The SSE mock flushes every frame separately. Before timing, it withholds each next frame until the client sees and
acknowledges the preceding content delta. Timed runs use the same frames without the acknowledgment round trips or
artificial delays. The harness checks mock dispatch counts and one delivered terminal with usage for every successful
inference after graceful shutdown.

## Prepare

Use an ARM64 Linux host with Landlock ABI 8 or newer, the repository's pinned Bun 1.4.0, an ARM64 C++20 compiler such as
GCC 14 or newer, Ninja, mold, Python 3.11 or newer, `jq`, and development packages for OpenSSL 3, SQLite 3.40 or
newer, zlib, ICU, Brotli and Zstd. The benchmark uses `ip`, `unshare` and `setpriv` for isolation. Install `perf`, an
LLVM symbolizer and any matching system debug packages before timing. Preparation uses network access for exact tool and dependency
downloads. The benchmark itself has loopback access only.

Run from the repository root. All output goes into the scratch directory. Use the corresponding `toolchain.version`
in `tools/native-kv/inputs.json` and the exact platform binaries when running against another source revision.

```sh
bench=cpp/gateway/bench/request_path
revision=$(git rev-parse HEAD)
scratch=$(mktemp -d -t cppgw-request-path.XXXXXXXX)
bun_bin=$(command -v bun)
rustup_bin=$(command -v rustup)
mkdir "$scratch/oracle" "$scratch/prep-home" "$scratch/prep-tmp"
git archive "$revision" | tar -x -C "$scratch/oracle"
test "$("$bun_bin" --no-env-file --no-install --version)" = 1.4.0
test -f "$scratch/oracle/bun.lock" && test -f "$scratch/oracle/tools/lint/bun.lock"

(
  cd "$scratch"
  curl -fLSs -o boost_1_92_0.tar.bz2 https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2
  printf '%s  %s\n' 5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c boost_1_92_0.tar.bz2 | sha256sum -c -
  tar -xjf boost_1_92_0.tar.bz2
  cd boost_1_92_0
  ./bootstrap.sh --with-libraries=json,container
  ./b2 -j2 --with-json --with-container variant=release link=static threading=multi toolset=gcc-14 \
    cxxflags='-g1 -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer' stage
)

python3 -I "$bench/build.py" --source "$scratch/oracle" --out "$scratch/build" \
  --boost "$scratch/boost_1_92_0" --cxx g++-14 --jobs 4
```

The build script reads the existing source and vendor inventories, applies the strict first-party warning set, and
builds the gateway, startup unit and both companion CLIs. Keep the binary with its line tables and frame pointers
for `perf`. The repository's existing builders prepare the real storage and confinement artifacts before launch.

```sh
rust_version=$(jq -r .toolchain.version "$scratch/oracle/tools/native-kv/inputs.json")
env -i PATH="$PATH" HOME="$scratch/prep-home" TMPDIR="$scratch/prep-tmp" RUSTUP_HOME="$scratch/rustup" \
  "$rustup_bin" toolchain install "$rust_version" --profile minimal --no-self-update
mkdir -p "$scratch/prep-home/.rustup/toolchains"
ln -s "$scratch/rustup/toolchains/${rust_version}-aarch64-unknown-linux-gnu" \
  "$scratch/prep-home/.rustup/toolchains/stable-aarch64-unknown-linux-gnu"

(
  cd "$scratch/oracle"
  env -i PATH="$PATH" HOME="$scratch/prep-home" TMPDIR="$scratch/prep-tmp" \
    "$bun_bin" --no-env-file install --frozen-lockfile --ignore-scripts
  env -i PATH="$PATH" HOME="$scratch/prep-home" TMPDIR="$scratch/prep-tmp" \
    "$bun_bin" --no-env-file --no-install scripts/build-permissions.ts
  env -i PATH="$PATH" HOME="$scratch/prep-home" TMPDIR="$scratch/prep-tmp" \
    "$bun_bin" --no-env-file --no-install scripts/prepare-native-runtime.ts
)

binding="$scratch/oracle/.data/native-kv.binding.json"
package="$scratch/oracle/.data/request-path-package"
python3 -I "$bench/package.py" --source "$scratch/oracle" \
  --build-dir "$scratch/build" --profile-manifest "$scratch/build/profile-owner.json" \
  --native-kv "$(jq -r .artifactDirectory "$binding")" \
  --native-kv-inputs "$(jq -r .inputsManifest "$binding")" \
  --native-kv-provenance-sha256 "$(jq -r .expectedProvenanceSha256 "$binding")" \
  --permissions "$scratch/oracle/.tmp/permissions-native" --output "$package"
```

The benchmark package wrapper adds the distro's ARM64 multiarch library directory to the existing package producer's
lookup. The native entrypoint verifies the resulting package and library hashes and applies its regular fixture
confinement. On an Arch-style host this wrapper uses the existing library lookup without an extra directory.

## Measure and profile

```sh
run() {
  sh "$bench/isolated.sh" --source "$scratch/oracle" --source-revision "$revision" \
    --binding "$binding" --bun "$bun_bin" --package "$package" "$@"
}
run --out "$scratch/comparison"
python3 -I "$bench/summarize.py" "$scratch/comparison/results.json"
```

By default the six scenarios each run three times per runtime with four concurrent, closed-loop HTTP/1.1 clients.
Pair order alternates Bun/native, native/Bun, Bun/native, and scenario order rotates across repetitions. Clients
reuse connections when the server allows it. Each new process has an untimed warm-up. The receipt records exact
artifact and fixture input hashes, connection reuse, call counts and per-run metrics.

- Requests per second divides completed requests by batch wall time. Latencies run from each client send to its
  complete response read. SSE includes incremental frame decoding. The p95 and p99 use nearest rank.
- Gateway CPU is the whole process's reaped user plus system time minus its pre-batch `/proc` CPU counters. This
  includes the final durable drain and shutdown. Driver and mock CPU are separate. Gateway peak RSS is Linux
  `VmHWM` sampled after the batch, including startup and warm-up in that individual process.
- The summary displays the median of the three run statistics and the minimum–maximum across those statistics.

Use the two lowest native/Bun median throughput ratios from the summary. Pass each scenario's name and a request
count expected to give about 12–20 seconds of warm native traffic. Use `--sudo-perf` when this host's perf policy
requires elevated sampling. The gateway still runs as the unprivileged invoking user.

```sh
scenario=large
run --out "$scratch/profile-$scenario" --profile "$scenario" --requests 1600 --sudo-perf
python3 -I "$bench/profile.py" "$scratch/profile-$scenario/01-$scenario-native/perf.data" >"$scratch/$scenario-top20.json"
```

`perf record` attaches to the already-warm native process. A FIFO acknowledgment enables and disables a
997 Hz frame-pointer recording of `cycles:u` around the timed batch. `profile.py` expands DWARF inline frames
with `llvm-symbolizer`. Use `--symbolizer llvm-symbolizer-18` if the installed executable has a version suffix.
The report uses sampled period weights and
counts each function once per call chain. Inclusive rows show the function and its callees. Exclusive rows show
the sampled leaf. A reported source location is the highest-weight available line for that function. Shared library
lines require the host binaries' matching debuginfo. Keep the original binary, libraries and debug symbols available
for `perf script`.

The scratch directory holds synthetic state, logs and raw receipts. Remove it when finished with those artifacts.
