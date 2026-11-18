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
