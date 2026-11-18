# C++ terminal record and request ownership performance

## Work contract v1

- Start from `b54357a92c0f2669335d83f04047c3f7de32f925` (tree `d0aabad25d3dbe02f2f33ff519e5b77715d19d37`) on Linux ARM64. Reduce CPU per rejected request by at least 10%, with no regression for model listing or a small inference request.
- Source ownership starts with `cpp/gateway/src/handler/terminal_log.cpp` and `cpp/gateway/src/handler/serve_handler.cpp`. Use the baseline profile to confirm the callback change before editing. A change in source ownership requires a new contract version.
- Keep terminal field names, values, types and null/omission semantics, response statuses and bodies, authentication, delivery receipts, and settlement and durable-write ordering. Preserve owners across suspended or detached work. Deployment, other gateway modules, test edits, and provider traffic are outside this work.
- Run the rejected request, authorized model list, and small inference against loopback fixtures. Measure gateway CPU, request rate, allocation calls, and a reject call-stack profile on the same baseline and candidate configuration.
- After measurement, run the exact reverse `tools/links.py` closure under the portable release and ASan+UBSan profiles, and run the full release suite. Use isolated processes with empty home, fresh temp storage, and only mock credentials. Check the path boundary and diff before handing off the branch.
- The supplied checkout omits the benchmark and its report. Both were recovered from `6878261f` on `origin/claude/cpp-gateway-port` with `git archive` into scratch. The reverse link inventory contains 50 affected binaries among 442 total.

## Before changes

The original scenarios exercised the packaged native gateway, real SQLite storage, and the original loopback mock. Each timing trial used four clients and a fresh private state directory. The native startup enforced the loopback-only fixture boundary. The isolated launcher supplied an empty home and temp directory with synthetic fixture auth inputs. The native-only runner reused the original request driver, mock, launch manifest, health and auth setup, warm-up, dispatch and completion checks, and graceful shutdown.

| Scenario | Timed requests per trial | Median req/s | Median gateway CPU ms/request | Median allocations/request |
| -------- | -----------------------: | -----------: | ----------------------------: | -------------------------: |
| `reject` |                    3,000 |      2,182.4 |                         0.357 |                    1,264.0 |
| `models` |                    3,000 |      2,302.6 |                         0.373 |                    1,514.6 |
| `small`  |                    1,500 |        374.4 |                         1.746 |                    5,516.5 |

Timing completed five trials per scenario, 37,500 requests total. Allocation counting completed three trials per scenario, 22,500 requests total. A separate 45,000-request profile reached `logTerminalRequest` in 17.21% of sampled user cycles, compared with 21.68% in the original report. The AArch64 atomic add used by shared ownership took 4.50% exclusive and `_M_release` took 1.43%. The profile included 19,146 samples and 37,659,702,430 period-weighted cycles. 18.33% of leaf cycles lacked symbol resolution.

The portable Clang build initially failed on sign-conversion diagnostics inside the vendored simdutf header included by `src/rt/base64.cpp`. The scratch CI driver adapter marks `third_party/` as a system-header prefix only for that consumer, preserving first-party warnings. The recovered package wrapper also needed the baseline package tool directory on its Python import path. SQLite 3.54.0 required an explicit `libsqlite3.so.0` SONAME at link time for the packager's existing ABI check. The repository build, packaging and runtime files were kept byte-identical.

## Implementation

- `terminal_log.cpp` reserves one Boost.JSON object for all 56 possible fields and supplies a 4 KiB buffer for its keys and ordinary values. Larger fields use the allocator's upstream resource. The buffer is released before asynchronous work, and the existing serializer still handles escaping, numbers, nested failure values and nulls. The reserved object removes the implicit JSON value copy. [Terminal log source](../../src/handler/terminal_log.cpp)
- Delivery callbacks move existing request and promise handles. The abort controller keeps its own signal state by value. Removing the registered listener hands abort authority over to the transport receipt, and the receipt's rejection still aborts downstream work. The buffered coroutine takes its owner by value, and terminal write reactions share one counter. [Serve handler](../../src/handler/serve_handler.cpp)
- Buffered body resolution keeps its checkpoint before diagnostic writes. Requests with replay data or an omission still run replay finalization. Gateway authentication failures without diagnostic consumers avoid cloning and reparsing their outgoing error body. Both the terminal line and delivery receipt remain active, and the telemetry, analytics and rollup writes still drain.

The record-only paired trial reduced reject CPU from 0.372 to 0.342 ms/request and allocations from 1,264.0 to 1,138.0 per request. The five paired 10,000-request rejection trials after the diagnostic change reduced CPU from 0.3573 to 0.3018 ms/request (15.5%) and increased throughput from 2,809.9 to 3,327.3 req/s. Each trial's request, dispatch and terminal payload checks passed.

## Commands and scope

Scratch was `/tmp/cppgw-perf-log-records-20261118`. The host was Linux ARM64 Neoverse-N2 with four CPUs, Ubuntu 24.04, Clang 21.1.8, mold 2.30.0, OpenSSL 3.0.13, SQLite 3.54.0, ICU 74.2, zlib 1.3, Brotli 1.1.0 and Zstd 1.5.5. Release used `-O2 -g1 -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer` and the portable driver's fatal warnings. ASan+UBSan used the driver's `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer` and enabled LeakSanitizer.

```sh
work=/tmp/cppgw-perf-log-records-20261118
bench="$work/harness/cpp/gateway/bench/request_path"
revision=b54357a92c0f2669335d83f04047c3f7de32f925
git archive 6878261f -- cpp/gateway/bench/request_path cpp/gateway/docs/findings/cppgw-perf-request-path-20261117.md | tar -x -C "$work/harness"
git archive "$revision" | tar -x -C "$work/oracle"
# From the verified Boost 1.92.0 source directory in scratch
./bootstrap.sh --with-libraries=json,container
./b2 -j2 variant=release link=static threading=multi --with-json --with-container stage
# From the verified SQLite 3.54.0 source directory in scratch
./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
make -j1 libsqlite3.so LDFLAGS.libsqlite3.soname=-Wl,-soname,libsqlite3.so.0
sudo -n make install LDFLAGS.libsqlite3.soname=-Wl,-soname,libsqlite3.so.0
sudo -n ldconfig
```

The archived harness README gives the native runtime preparation commands, including pinned Rust 1.98.1, the frozen Bun 1.4.0 install, `scripts/build-permissions.ts` and `scripts/prepare-native-runtime.ts`. The gateway and both companion CLIs were linked using the portable release driver objects plus the production startup object and entrypoints. The benchmark packages use the same archive of static assets, native KV provenance, and permissions artifact. Before and after load their own manifest-verified executable. The adapter adds native-only and alternating native-vs-native runs, allocation counting, and normalized terminal equality. For the final timing trials it pins the client and mock to CPUs 0–1 and the gateway to CPUs 2–3. The stock harness profiles both executables over their own 45,000-request rejection batches. [Recovered harness](https://github.com/ubiquity/ai.ubq.fi/tree/6878261f/cpp/gateway/bench/request_path)

```sh
python3 -B "$work/link_bench.py" "$PWD" before
bash "$work/package_bench.sh" before
bash "$work/run_bench.sh" timed --native-only --scenarios reject models small --repetitions 5 --out "$work/baseline-timed"
bash "$work/run_bench.sh" timed --native-only --scenarios reject models small --repetitions 3 --allocations "$work/count.so" --out "$work/baseline-alloc"
bash "$work/run_bench.sh" profile --out "$work/baseline-profile" --profile reject --requests 45000 --sudo-perf
python3 -I "$bench/profile.py" "$work/baseline-profile/01-reject-native/perf.data" --symbolizer llvm-symbolizer-21 --top 150
```

The allocation shim counted gateway-process calls to glibc `malloc`, `calloc`, `realloc`, `aligned_alloc`, `memalign` and `posix_memalign` after warm-up and through shutdown. Allocation samples used a separate batch, with CPU and throughput taken from the uninstrumented batch. Logs were parsed within the isolated harness to compare every field after normalizing generated request IDs and numeric elapsed times, preserving nulls and all non-time values.

The release smoke mapped `terminal_log.cpp` to admission logging and receipt-driven writes, and `serve_handler.cpp` to handoff, abort and transport delivery. The release gate covered those tests, the rest of the reverse closure, and the complete inventory. The sanitizer gate used the exact `tools/links.py` reverse closure and compiled its required application objects plus the runtime objects. Both gates used the portable driver's verified loopback namespace, empty home and private tmpfs for every test process.

```sh
python3 -B cpp/gateway/tools/links.py --check
bash "$work/ci_env.sh" release smoke 2
bash "$work/ci_env.sh" release full 2
bash "$work/ci_env.sh" asan linked 2
git diff --check "$revision"..HEAD
```

## Continuation measurements

The resumed Linux ARM64 Neoverse-N2 runner rebuilt the saved candidate at `be0d420a502ff702943ba96cd8e2da0ad49b9cad` (tree `a6e15e41e39fc671fe574136e1f906130e3a3e7a`). Both native packages linked all 369 application and runtime objects and both vendored archives. The original `b54357a92c0f2669335d83f04047c3f7de32f925` baseline substituted only the two owned source objects. Each package verified its launch manifest, permissions artifact and native KV provenance. The six package and six allocation smoke trials passed.

The final comparison alternated package order within each round and rotated scenario order. The gateway ran on CPUs 2–3 and the client and synthetic mock on CPUs 0–1. The controller paused the two compiler processes and their descendants for each measurement window. Metrics below are medians across all uninstrumented trials for each scenario and three separately instrumented allocation trials per package. Gateway CPU includes user and system time through the graceful drain.

| Scenario | Timed requests per trial | Timed pairs | Req/s before | Req/s after | CPU ms/request before | CPU ms/request after | CPU change | Allocations/request before | Allocations/request after |
| -------- | -----------------------: | ----------: | -----------: | ----------: | --------------------: | -------------------: | ---------: | -------------------------: | ------------------------: |
| `reject` |                   10,000 |           7 |      2,975.4 |     3,501.6 |               0.33663 |              0.28648 |     −14.9% |                   1,263.96 |                    947.70 |
| `models` |                   10,000 |           7 |      3,560.4 |     3,594.8 |               0.34007 |              0.33395 |      −1.8% |                   1,666.87 |                  1,663.65 |
| `small`  |                    3,000 |          18 |        675.2 |       692.8 |               1.62064 |              1.60078 |      −1.2% |                   5,785.96 |                  5,639.00 |

Rejection saved 12.7% to 16.2% CPU in each pair. The initial seven small-inference pairs had a 1.1% median paired CPU saving and a 0.35% increase between the two pooled CPU medians. Eleven additional pairs on the same binaries gave medians of 1.63510 and 1.59784 ms/request. The table includes all 18 small-inference pairs. The median paired change across those 18 was −1.30% CPU and +2.15% throughput.

Across 388,000 timed requests and 138,000 allocation-counted requests, the drivers validated status, response shape, upstream dispatch, graceful shutdown, and terminal field equality with only generated request IDs and numeric elapsed times normalized. Each inference request contributed one delivered terminal with fixture usage. Rejections returned 401 with zero upstream calls. Authenticated model requests returned the same catalog in the same order with zero upstream calls.

The two 45,000-request rejection profiles used hardware user cycles at 997 Hz with frame-pointer call stacks. The before and after captures contained 15,696 and 13,105 samples, with 17.51% and 15.64% unresolved leaf weight respectively. Period-weighted values include unresolved samples in the denominator.

| Function or event                           | Before share | After share | Before cycles/request | After cycles/request |
| ------------------------------------------- | -----------: | ----------: | --------------------: | -------------------: |
| `logTerminalRequest` coroutine, inclusive   |       16.75% |      11.74% |               127,386 |               69,303 |
| `__aarch64_ldadd4_acq_rel`, executable leaf |        4.74% |       4.66% |                36,016 |               27,517 |
| `_M_release`, exclusive                     |        1.34% |       1.36% |                10,195 |                8,024 |

The application diff is 154 additions and 119 deletions across the reserved terminal record and delivery callback changes. The per-file counts are +143/−99 in `terminal_log.cpp` and +11/−20 in `serve_handler.cpp`. The continuation applied no further application edits.

### Continuation commands and receipts

Clang 21.1.8 was installed from the signed LLVM Noble repository. Boost 1.92.0 used the verified archive and GCC 14.2 static builds of JSON and Container. The Clang Boost.Build attempt selected an ARM64 target triple that could not find the installed C++ headers. The host packager adapter added Ubuntu multiarch and the dedicated `/usr/lib/cppgw-sqlite-3.54.0` directory. The versioned SQLite shared library kept its `libsqlite3.so.0` SONAME. `patchelf --remove-rpath` removed its default `/usr/local/lib` RUNPATH from the packaged host copy, which the loader selected via `ldconfig`. The packager retained the normal system-library and hash checks. A loader probe returned SQLite 3.54.0.

The scratch wrappers run the archived driver, fixture definitions, mock and profiler. `link_bench.py` takes flags from the portable driver's Ninja file and swaps the two baseline translation units from Git. The package adapter invokes the original package producer with the same frozen KV and confinement artifacts for both binaries. The allocation shim counts the six glibc allocation entrypoints after warm-up. The namespace launcher uses the archived loopback-only setup with sparse environments and private trial state.

```sh
work=/tmp/cppgw-perf-log-records-20261118
bench="$work/harness/cpp/gateway/bench/request_path"
before=b54357a92c0f2669335d83f04047c3f7de32f925
after=be0d420a502ff702943ba96cd8e2da0ad49b9cad
# From the verified Boost source directory
printf '%s\n' 'using gcc : 14 : /usr/bin/g++-14 ;' > "$work/boost-user-config.jam"
./b2 --user-config="$work/boost-user-config.jam" -j2 toolset=gcc-14 variant=release link=static threading=multi \
  --with-json --with-container cxxflags='-g1 -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer' stage
# From the repository root, with the verified SQLite 3.54.0 host library and release objects ready
python3 -B "$work/bench_vendors.py" "$PWD"
python3 -B "$work/link_bench.py" "$PWD" before "$before"
python3 -B "$work/package_bench.py" before
python3 -B "$work/link_bench.py" "$PWD" after "$after"
python3 -B "$work/package_bench.py" after
clang-21 -std=c17 -D_GNU_SOURCE -O2 -fPIC -shared -Wall -Wextra -Werror "$work/count.c" -o "$work/count.so" -ldl
# The measurement controller used these four runs, followed by the small confirmation
bash "$work/run_bench.sh" --before "$before" --after "$after" --out "$work/baseline-profile" --label before --profile reject --requests 45000
bash "$work/run_bench.sh" --before "$before" --after "$after" --out "$work/paired-timed" --repetitions 7
bash "$work/run_bench.sh" --before "$before" --after "$after" --out "$work/paired-alloc" --repetitions 3 --allocations
bash "$work/run_bench.sh" --before "$before" --after "$after" --out "$work/candidate-profile" --label after --profile reject --requests 45000
bash "$work/run_bench.sh" --before "$before" --after "$after" --out "$work/small-confirm" --scenarios small --repetitions 11
python3 -I "$bench/profile.py" "$work/baseline-profile/before/01-reject-native/perf.data" --symbolizer llvm-symbolizer-21 --top 150
python3 -I "$bench/profile.py" "$work/candidate-profile/after/01-reject-native/perf.data" --symbolizer llvm-symbolizer-21 --top 150
```

| Item                         | SHA-256                                                            |
| ---------------------------- | ------------------------------------------------------------------ |
| Before gateway binary        | `efd156af4be828a4581fd35f3341608376f44c1e150d45b03ed61aa285bc7a7e` |
| After gateway binary         | `4f46733259660ed2d4e7fb6754ac5bcee215913545790453bd923371b225c313` |
| Before package manifest      | `9cafdab38919fabc35dc567b5dbdc8a11bf26a449ecc70c2d3d86ce5a117ae62` |
| After package manifest       | `3939a2c43082724fc18662c10af01837e3db1b551ee04244652a594f46ddc466` |
| Timing results               | `26f8651603f1f7fa36853b6a4a4eed1f0e43cbdb9f858063fd17cbacbd57a30f` |
| Allocation results           | `477760f8ee39efa3c7473b43ff3ef3ad0e8cbfed7821cea8d721278f27908653` |
| Small confirmation results   | `e2a930d21c516d76a2ee274ee5eff6e3c8a8f476890e222a503110f5a1b49da7` |
| Before rejection `perf.data` | `d3da88ef6f5800d819df9efaa460eb4976b87d6cbafef3d9a4ab0f6fbf162cf1` |
| After rejection `perf.data`  | `2ead06e7dd7a06e9531bed990a3753390fef20d79323658bfab75092fec8bab4` |

The full release and linked ASan+UBSan gates were still running at this checkpoint.

### Allocation checks for the implementation commits

Three additional 10,000-request rejection trials for each cumulative step reproduced the savings. The 150,000 timed requests passed status, zero-dispatch, shutdown and normalized terminal comparison checks. All 15 runs produced the same normalized terminal digest.

| Cumulative source change     | Commit     | Median allocations/request |
| ---------------------------- | ---------- | -------------------------: |
| Source baseline              | `b54357a9` |                   1,263.96 |
| Reserved record              | `319b2475` |                   1,137.96 |
| Move delivery ownership      | `72042438` |                   1,129.96 |
| Avoid empty replay work      | `62e4850a` |                   1,106.93 |
| Skip default 401 diagnostics | `0ed173c2` |                     947.70 |

```sh
record=$(git rev-parse 319b2475)
ownership=$(git rev-parse 72042438)
replay=$(git rev-parse 62e4850a)
for checkpoint in "record:$record" "ownership:$ownership" "replay:$replay"; do
  label=${checkpoint%%:*}
  revision=${checkpoint#*:}
  python3 -B "$work/link_bench.py" "$PWD" "$label" "$revision"
  python3 -B "$work/package_bench.py" "$label"
done
python3 -I "$work/measure_pair.py" "$before" "$after" --checkpoints \
  --checkpoint record "$record" --checkpoint ownership "$ownership" --checkpoint replay "$replay"
```

The SHA-256 of `checkpoint-alloc/results.json` is `b06b9db67fd1c8d9c1ac401f44f4e52e706137107a544d5ad6d16ee725ff0a6b`.
