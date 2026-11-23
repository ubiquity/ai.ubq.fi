# Native gateway request-path benchmark

The native gateway handled five of the six loopback scenarios at 1.19–1.88× Bun's median throughput. For the
1 MiB chat request it handled 155.8 req/s against Bun's 167.6 req/s, with 28% less gateway CPU per request.
Peak native RSS was 33–40 MiB for the other five scenarios and 103 MiB for the large request. Bun peaked at
112–135 MiB and 206 MiB respectively within this fixture.

## Contract v1 and execution

This report measures source commit `0edaf83367bbb29eaf50076afd428818f4b3e4bb`, tree
`864e6a861bbc5fcc0069f676cbddc76f4833ee34`. The work is confined to the benchmark and this report. Production
additions and deletions were both zero lines. There was no replacement library or production compatibility shim.
The C++ build used both selected vendored libraries, simdjson 5.2.0 and simdutf 9.4.0.

The Linux ARM64 host ran Ubuntu 24.04.5 with kernel `7.0.0-1017-azure`, glibc 2.39 and four Neoverse-N2 vCPUs.
The native build used GCC 14.2.0, mold 2.30.0, Boost 1.92.0, OpenSSL 3.0.13, SQLite 3.45.1, zlib 1.3,
ICU 74.2, Brotli 1.1.0 and Zstd 1.5.5, with C++20, `-O2 -g1 -DNDEBUG -fno-omit-frame-pointer
-mno-omit-leaf-frame-pointer` and the strict port warning flags. All 369 production translation units, the
startup unit, three entrypoints and the two vendored libraries compiled and linked without warnings. The Bun
oracle ran with Bun 1.4.0 revision `34cbb9a40`, frozen dependencies, `--no-env-file --no-install` and the
existing production-smoke launcher. It enters `serve.ts` and `#http` with its real `node:http` transport.

Each trial launched a new process from an empty private state root with a separate Bun-codec SQLite database.
The same loopback mock served both runtimes throughout the comparison. The Linux network namespace had only
loopback, and children received a fresh `HOME`, `TMPDIR` and only synthetic auth inputs. The mock validated the
full 1,048,576-byte client body's 17 messages arriving intact in a 1,048,591-byte upstream request on each
large-body dispatch. Both gateways used the same four models in their bare list, with zero upstream calls
from models or rejection. The eight-frame progressive proof required the downstream client to acknowledge each
content delta before the provider could send another. Timed SSE ran with eager flushes and no artificial pacing.

Native/Bun pairs switched order between repetitions, with rotated scenario order. Four closed-loop HTTP/1.1
clients per batch reused connections on successful routes. Both gateways closed after the 401, so that scenario
includes a new TCP connection on every request. Throughput measures batch wall time and latency includes the
whole response. CPU is reaped process user plus system time less the pre-batch CPU counters, including final
durable drain and exit. RSS is `VmHWM` after the timed batch and includes process startup and warm-up. The
CPU and RSS scopes are the gateway process alone. Each cell is the median of three run statistics, followed
by their minimum and maximum. Tail statistics are per run.

## Results

| Scenario  | Runtime | n/rep | req/s                  | p50 ms                 | p95 ms                 | p99 ms                 | CPU ms/req          | peak RSS MiB        |
| --------- | ------- | ----- | ---------------------- | ---------------------- | ---------------------- | ---------------------- | ------------------- | ------------------- |
| small     | bun     | 1500  | 415.7 [409.7–421.8]    | 9.127 [8.813–9.214]    | 12.470 [12.069–13.471] | 17.222 [16.722–17.335] | 3.381 [3.269–3.407] | 135.0 [132.0–138.1] |
| small     | native  | 1500  | 781.0 [743.4–782.7]    | 5.011 [4.996–5.057]    | 6.364 [6.285–6.951]    | 7.776 [7.654–8.728]    | 1.462 [1.451–1.471] | 40.2 [40.2–40.2]    |
| large     | bun     | 240   | 167.6 [151.9–171.6]    | 23.577 [23.433–25.726] | 30.226 [29.894–35.166] | 35.241 [33.096–37.466] | 9.251 [9.049–9.482] | 205.6 [201.0–206.2] |
| large     | native  | 240   | 155.8 [152.5–164.8]    | 25.147 [23.549–25.383] | 32.078 [30.885–33.683] | 34.268 [32.685–71.582] | 6.690 [6.468–6.893] | 103.2 [103.1–103.7] |
| stream    | bun     | 750   | 361.4 [358.0–370.4]    | 10.650 [10.445–10.684] | 14.457 [13.926–14.521] | 18.980 [17.930–19.041] | 4.076 [3.915–4.086] | 134.2 [133.5–134.3] |
| stream    | native  | 750   | 564.8 [564.1–576.1]    | 6.926 [6.890–7.002]    | 7.930 [7.869–8.260]    | 8.857 [8.853–9.238]    | 1.931 [1.927–1.952] | 37.5 [37.4–37.5]    |
| responses | bun     | 1500  | 406.6 [401.9–412.1]    | 9.367 [9.338–9.533]    | 12.737 [12.565–13.008] | 16.858 [16.232–17.875] | 3.425 [3.405–3.447] | 133.5 [133.2–138.6] |
| responses | native  | 1500  | 720.9 [698.7–728.0]    | 5.365 [5.361–5.413]    | 6.748 [6.601–6.931]    | 8.256 [7.874–8.815]    | 1.547 [1.533–1.556] | 40.3 [40.3–40.4]    |
| models    | bun     | 3000  | 2024.5 [2021.6–2041.0] | 1.726 [1.719–1.737]    | 3.305 [3.266–3.305]    | 4.099 [4.015–4.140]    | 0.835 [0.827–0.842] | 116.8 [115.2–118.1] |
| models    | native  | 3000  | 3596.4 [3568.0–3655.2] | 1.083 [1.063–1.090]    | 1.263 [1.228–1.271]    | 1.424 [1.419–1.449]    | 0.329 [0.324–0.333] | 34.4 [34.4–34.4]    |
| reject    | bun     | 3000  | 2574.9 [2473.6–2602.8] | 1.331 [1.302–1.396]    | 2.660 [2.628–2.686]    | 3.228 [3.185–3.253]    | 0.473 [0.462–0.479] | 111.6 [110.3–112.3] |
| reject    | native  | 3000  | 3075.3 [3028.9–3131.7] | 1.284 [1.258–1.305]    | 1.329 [1.315–1.363]    | 1.356 [1.345–1.388]    | 0.329 [0.321–0.332] | 33.4 [33.3–33.4]    |

Native/Bun median throughput, lowest first

- large 0.930×
- reject 1.194×
- stream 1.563×
- responses 1.773×
- models 1.776×
- small 1.879×

Native large-chat throughput was lower in two of the three matched repetitions. The run ranges overlap, and
the native large-chat p99 ranged from 32.685 to 71.582 ms. The mock used about 2.29 ms of process CPU per large
native request and 2.04 ms per Bun request, and the driver used about 0.50 ms per large request. Native's own
CPU was 6.69 ms/request against 9.25 ms/request for Bun. This four-client test includes both HTTP transfers and
the local mock. Its throughput ratio alone cannot identify a gateway-internal regression. For SSE, median
time to first content delta was 6.62 ms native and 10.28 ms Bun.

The paired smoke completed 12/12 scenario/runtime trials with 96/96 timed requests. The comparison completed
36/36 trials, 59,940/59,940 timed requests, 7,920 warm-up requests and six progressive controls without a
status, shape or dispatch failure. All 27,066 successful inference requests had one correlated delivered
terminal with the fixture usage. All 36 gateway processes drained and exited with status zero. The two profile
batches passed their own response, dispatch and shutdown checks. No existing vectors or expected values were
edited. The harness changes no link closure, so the changed-path gate selected zero existing C++ test binaries
for a plain or sanitizer rerun.

## Reproduction and receipts

The [benchmark README](../../bench/request_path/README.md) gives the complete scratch build, frozen installs,
native artifact preparation and launch commands. Use `revision=0edaf83367bbb29eaf50076afd428818f4b3e4bb` for
the measured application snapshot. These were the comparison and profile commands using its `run` function.

```sh
run --out "$scratch/pair-smoke" --repetitions 1 --requests 8 --warmup 2
run --out "$scratch/comparison"
python3 -I "$bench/summarize.py" "$scratch/comparison/results.json"
run --out "$scratch/profile-large-final" --profile large --requests 2600 --sudo-perf
run --out "$scratch/profile-reject-final" --profile reject --requests 45000 --sudo-perf
python3 -I "$bench/profile.py" "$scratch/profile-large-final/01-large-native/perf.data" --symbolizer llvm-symbolizer-18 --top 150
python3 -I "$bench/profile.py" "$scratch/profile-reject-final/01-reject-native/perf.data" --symbolizer llvm-symbolizer-18 --top 150
```

The runner added `libbrotli-dev`, `valgrind` and `mold` with `apt-get`. `perf` 7.0.14 was available, so neither
profile used Valgrind. SQLite and libstdc++ line resolution used build-ID-matched Ubuntu debug info, fetched
from `https://debuginfod.ubuntu.com/buildid/<build-id>/debuginfo` and verified against `readelf -n`.

| Input or receipt              | SHA-256                                                            |
| ----------------------------- | ------------------------------------------------------------------ |
| Native gateway executable     | `829891564970c6e1366debbdbf978ee8e074825d9dfd1aff3d2da8e1c698b33d` |
| Native package manifest       | `c84b5a6e81a8835b5738bc5e9f5f1606a4d958959fdb70e2c863b5dfa10e0001` |
| Bun binary                    | `086c4121c8738a8e0f5ed730e8a461bc3973b4444e372ddb77aef9a747fa2ae9` |
| Bun lockfile                  | `f59f2e4a7c02583579a772e46c35b30eaed2346f54bcb1f4a05627f3ac63bbb8` |
| Native KV build provenance    | `97c40065a9f226898a7182ff7b7d0baa6707d2b9311802d9bc9605ad1b0ccfca` |
| Verified Boost source archive | `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` |
| Comparison `results.json`     | `5220a95b4b1a3219c242e2a01a70db9c3c1a73068afb556ff46dcdebb15fceb3` |
| Large-chat `perf.data`        | `d80100a11412805c98253d3dd4d5baaf7afb0ee97310b450f5ba44bdaded09a6` |
| Reject `perf.data`            | `3ad06fe25e92f533f95f618ea4fcd2e4ffa98639be239ed4f59771b42df1e8be` |

## Native profiles

Large chat (0.930× Bun) and unauthenticated chat (1.194× Bun) had the two smallest median throughput ratios.
`perf record` sampled user cycles at 997 Hz with frame-pointer call chains, after warm-up. Each sample is
weighted by its recorded period, with recursive occurrences of a function counted once per stack for
inclusive cost. A source line is a representative sampled location, weighted by period within that function.
The measurement includes all gateway threads and excludes kernel cycles and the mock or client processes.

Large chat recorded 17,092 samples over 16.08 s and 44.95 billion weighted cycles in two threads. Rejection
recorded 14,935 samples over 14.98 s and 32.23 billion weighted cycles in the listener thread. Unresolved
leaves accounted for 0.58% and 1.02% of sampled cycles, respectively, and remain in the denominators. The
large profile validated 2,600 timed inference requests and 2,620 delivered terminals including warm-up. The
rejection profile validated 45,000 timed 401 responses and zero mock calls.

**Port** identifies port-layer work, including calls into existing platform libraries. **App** identifies
application logic. **Generic** identifies a candidate for library replacement or a library fast path under
the plan. The inclusive lists share ranks 1–14. Each percentage includes descendants and several rows
describe the same stack, so the percentages are not additive. CRT and outlined atomic locations are the
source entry or call instruction matched to the disassembly, using glibc 2.39 and GCC 13.3/14.2 source. The
remaining locations came from DWARF. [Replacement plan](../OSS-REPLACEMENT-PLAN.md)
[glibc AArch64 startup](https://github.com/bminor/glibc/blob/glibc-2.39/sysdeps/aarch64/start.S#L98)
[GCC 13.3 atomics](https://github.com/gcc-mirror/gcc/blob/releases/gcc-13.3.0/libgcc/config/aarch64/lse.S#L192)
[GCC 14.2 atomics](https://github.com/gcc-mirror/gcc/blob/releases/gcc-14.2.0/libgcc/config/aarch64/lse.S#L302)

### Inclusive cost, ranks 1–14 for both scenarios

| Rank | Function                         | File:line                                      | Large % | Reject % | Class                    |
| ---: | -------------------------------- | ---------------------------------------------- | ------: | -------: | ------------------------ |
|    1 | `_start`                         | `glibc/sysdeps/aarch64/start.S:98`             |   95.34 |   100.00 | Port, CRT ancestry       |
|    2 | `__libc_start_main`              | `glibc/csu/libc-start.c:360`                   |   95.34 |   100.00 | Port, CRT ancestry       |
|    3 | `__libc_start_call_main`         | `glibc/sysdeps/nptl/libc_start_call_main.h:58` |   95.34 |   100.00 | Port, CRT ancestry       |
|    4 | `main`                           | `cpp/gateway/entrypoints/gateway.cpp:16`       |   95.34 |   100.00 | App, entrypoint ancestry |
|    5 | `rt::blockOn<void>(Task)`        | `cpp/gateway/include/uos/rt/async.hpp:586`     |   95.34 |   100.00 | Port, loop ancestry      |
|    6 | `rt::blockOn<void>(Loop&, Task)` | `cpp/gateway/include/uos/rt/async.hpp:582`     |   95.34 |   100.00 | Port, loop ancestry      |
|    7 | `rt::detail::runUntilSettled`    | `cpp/gateway/src/rt/async.cpp:454`             |   95.34 |   100.00 | Port, loop ancestry      |
|    8 | `Loop::Impl::runLoop`            | `cpp/gateway/src/rt/async.cpp:373`             |   95.34 |   100.00 | Port, loop ancestry      |
|    9 | `Loop::Impl::turn`               | `cpp/gateway/src/rt/async.cpp:324`             |   95.09 |    99.97 | Port, loop ancestry      |
|   10 | `awaitable_thread::pump`         | `boost/asio/impl/awaitable.hpp:776`            |   88.33 |    82.02 | Port, Asio #36           |
|   11 | `awaitable_frame_base::resume`   | `boost/asio/impl/awaitable.hpp:508`            |   88.18 |    81.28 | Port, Asio #36           |
|   12 | `coroutine_handle<void>::resume` | `libstdc++/coroutine:137`                      |   87.82 |    80.41 | Port, coroutine ancestry |
|   13 | `Loop::Impl::checkpoint`         | `cpp/gateway/src/rt/async.cpp:293`             |   86.91 |    74.15 | Port, microtask ancestry |
|   14 | `rt::detail::Job::operator()`    | `cpp/gateway/include/uos/rt/async.hpp:118`     |   86.78 |    73.92 | Port, job ancestry       |

### Inclusive cost, large chat ranks 15–20

| Rank | Function                                 | File:line                                          | Large % | Class                                 |
| ---: | ---------------------------------------- | -------------------------------------------------- | ------: | ------------------------------------- |
|   15 | `fetchDeepSeekChatCompletions` coroutine | `cpp/gateway/src/deepseek/index.cpp:629`           |   34.38 | App, provider projection and dispatch |
|   16 | `rt::jsonStringify`                      | `cpp/gateway/src/rt/json.cpp:1310`                 |   22.37 | Port, JSON stringify KEEP #17         |
|   17 | `Serializer::root`                       | `cpp/gateway/src/rt/json.cpp:240`                  |   22.35 | Port, JSON stringify KEEP #17         |
|   18 | `Serializer::run`                        | `cpp/gateway/src/rt/json.cpp:363`                  |   22.31 | Port, JSON stringify KEEP #17         |
|   19 | `appendQuoted`                           | `cpp/gateway/src/rt/json.cpp:189`                  |   21.97 | Port, JSON stringify KEEP #17         |
|   20 | `__memcpy_sve`                           | `glibc/sysdeps/aarch64/multiarch/memcpy_sve.S:126` |   21.63 | Port, byte and string copies          |

### Inclusive cost, rejected chat ranks 15–20

| Rank | Function                                      | File:line                                    | Reject % | Class                            |
| ---: | --------------------------------------------- | -------------------------------------------- | -------: | -------------------------------- |
|   15 | `executor_function::operator()`               | `boost/asio/detail/executor_function.hpp:62` |    53.19 | Port, type-erased dispatch #36   |
|   16 | `any_executor_base::execute_ex<LoopExecutor>` | `boost/asio/execution/any_executor.hpp:901`  |    51.12 | Port, type-erased dispatch #36   |
|   17 | `LoopExecutor::execute<executor_function>`    | `cpp/gateway/src/rt/async.cpp:200`           |    51.01 | Port, executor dispatch ancestry |
|   18 | `rt::detail::start`                           | `cpp/gateway/src/rt/async.cpp:438`           |    43.66 | Port, coroutine startup ancestry |
|   19 | `co_spawn` from `rt::detail::start`           | `boost/asio/impl/co_spawn.hpp:382`           |    43.62 | Port, Asio #36                   |
|   20 | `async_initiate` from `rt::detail::start`     | `boost/asio/async_result.hpp:683`            |    43.53 | Port, Asio #36                   |

### Exclusive cost, large chat

| Rank | Function                   | File:line                                             | Large % | Class                               |
| ---: | -------------------------- | ----------------------------------------------------- | ------: | ----------------------------------- |
|    1 | `__memcpy_sve`             | `glibc/sysdeps/aarch64/multiarch/memcpy_sve.S:126`    |   21.40 | Port, byte and string copies        |
|    2 | `validPrefix`              | `cpp/gateway/src/rt/text_codec.cpp:54`                |   15.25 | Generic, simdutf 9.4.0 FAST-PATH #2 |
|    3 | `appendQuoted`             | `cpp/gateway/src/rt/json.cpp:189`                     |   14.11 | Port, JSON stringify KEEP #17       |
|    4 | `rt::isAscii`              | `cpp/gateway/src/rt/string.cpp:1779`                  |    6.25 | Generic, simdutf 9.4.0 FAST-PATH #2 |
|    5 | `_int_malloc`              | `glibc/malloc/malloc.c:4009`                          |    3.95 | Port, allocation                    |
|    6 | `vorrq_u8`                 | `GCC/arm_neon.h:1234`                                 |    2.76 | Port, existing SIMD parse #1        |
|    7 | `OPENSSL_cleanse`          | `OpenSSL/crypto/arm64cpuid.S:90`                      |    2.44 | Port, required buffer clearing      |
|    8 | `cfree`                    | `glibc/malloc/malloc.c:3375`                          |    1.31 | Port, allocation                    |
|    9 | `parse_string`             | `cpp/gateway/third_party/simdjson/simdjson.cpp:21125` |    1.15 | Port, existing simdjson #1          |
|   10 | `unlink_chunk.isra.0`      | `glibc/malloc/malloc.c:1616`                          |    1.07 | Port, allocation                    |
|   11 | `_int_free`                | `glibc/malloc/malloc.c:4622`                          |    0.91 | Port, allocation                    |
|   12 | `__memchr_generic`         | `glibc/sysdeps/aarch64/memchr.S:99`                   |    0.79 | Port, byte scanning                 |
|   13 | `tcache_get_n`             | `glibc/malloc/malloc.c:3190`                          |    0.78 | Port, allocation                    |
|   14 | `__aarch64_swp4_rel`       | `GCC-13.3/libgcc/config/aarch64/lse.S:254`            |    0.71 | Port, allocator synchronization     |
|   15 | `__aarch64_ldadd4_acq_rel` | `GCC-14.2/libgcc/config/aarch64/lse.S:302`            |    0.69 | Port, shared ownership atomics      |
|   16 | `malloc`                   | `glibc/malloc/malloc.c:3318`                          |    0.68 | Port, allocation                    |
|   17 | `malloc_consolidate`       | `glibc/malloc/malloc.c:4874`                          |    0.62 | Port, allocation                    |
|   18 | `tcache_put`               | `glibc/malloc/malloc.c:3165`                          |    0.51 | Port, allocation                    |
|   19 | `vandq_u8`                 | `GCC/arm_neon.h:1122`                                 |    0.44 | Port, existing SIMD parse #1        |
|   20 | `__memset_zva64`           | `glibc/sysdeps/aarch64/memset.S:127`                  |    0.43 | Port, buffer initialization         |

### Exclusive cost, rejected chat

| Rank | Function                              | File:line                                           | Reject % | Class                           |
| ---: | ------------------------------------- | --------------------------------------------------- | -------: | ------------------------------- |
|    1 | `__aarch64_ldadd4_acq_rel`            | `GCC-14.2/libgcc/config/aarch64/lse.S:302`          |     4.13 | Port, shared ownership atomics  |
|    2 | `_int_malloc`                         | `glibc/malloc/malloc.c:3954`                        |     3.44 | Port, allocation                |
|    3 | `_int_free`                           | `glibc/malloc/malloc.c:4518`                        |     2.72 | Port, allocation                |
|    4 | `malloc`                              | `glibc/malloc/malloc.c:3311`                        |     2.48 | Port, allocation                |
|    5 | `cfree`                               | `glibc/malloc/malloc.c:3375`                        |     2.22 | Port, allocation                |
|    6 | `__memcpy_sve`                        | `glibc/sysdeps/aarch64/multiarch/memcpy_sve.S:76`   |     2.07 | Port, byte and string copies    |
|    7 | `_Sp_counted_base::_M_release`        | `libstdc++/bits/shared_ptr_base.h:337`              |     1.43 | Port, shared ownership          |
|    8 | `boost::json::detail::digest`         | `boost/json/detail/digest.hpp:50`                   |     1.34 | Port, JSON object lookup #19    |
|    9 | `__aarch64_swp4_rel`                  | `GCC-13.3/libgcc/config/aarch64/lse.S:254`          |     1.21 | Port, allocator synchronization |
|   10 | `tcache_get_n`                        | `glibc/malloc/malloc.c:3190`                        |     1.20 | Port, allocation                |
|   11 | `__strlen_asimd`                      | `glibc/sysdeps/aarch64/multiarch/strlen_asimd.S:97` |     1.15 | Port, string conversions        |
|   12 | `basic_string::_M_is_local`           | `libstdc++/bits/basic_string.h:269`                 |     1.02 | Port, string lifetime           |
|   13 | `malloc_consolidate`                  | `glibc/malloc/malloc.c:4874`                        |     0.97 | Port, allocation                |
|   14 | `unlink_chunk.isra.0`                 | `glibc/malloc/malloc.c:1610`                        |     0.83 | Port, allocation                |
|   15 | `__aarch64_cas4_acq`                  | `GCC-13.3/libgcc/config/aarch64/lse.S:192`          |     0.83 | Port, synchronization           |
|   16 | `tag_new_usable`                      | `glibc/malloc/malloc.c:1461`                        |     0.77 | Port, allocation                |
|   17 | `basicParse`                          | `cpp/gateway/src/rt/url.cpp:774`                    |     0.77 | Generic, Ada 4.0.0 REPLACE #4   |
|   18 | `char_traits<char>::copy`             | `libstdc++/bits/char_traits.h:427`                  |     0.75 | Port, string copies             |
|   19 | `boost::json::detail::find_in_object` | `boost/json/impl/object.ipp:54`                     |     0.75 | Port, JSON object lookup #19    |
|   20 | `appendQuoted`                        | `cpp/gateway/src/rt/json.cpp:189`                   |     0.71 | Port, JSON stringify KEEP #17   |

## Out-of-scope findings and limits

- The shipped packager's `find_library` searches direct children of its library roots. On Ubuntu it refused
  `libzstd.so.1` despite the installed library under the ARM64 multiarch directory. The benchmark wrapper
  adds that directory to the existing packager's lookup and leaves startup and host hash verification in
  place. [Library lookup](../../tools/runtime-manifest.py#L56)
- The synthetic large body uses long ASCII fields that fit the provider's per-message limit. The run measures
  one prompt shape, one provider and four local clients. It does not estimate real upstream latency, broader
  production traffic, or an upper throughput bound. Frame pointers were kept in the timed executable to
  measure the same binary that was profiled.
- Git fetch succeeded. The noninteractive push preflight could not authenticate, so the task branch is
  committed locally on top of the C++ port branch for the maintainer's integration.

## Ranked performance work

1. **Carry owned byte buffers and JSON values through the large request path.** `__memcpy_sve` alone took
   21.40% of sampled user cycles. Its largest callers were string copies and byte-vector copies.
   Boost.JSON's storage-aware object copy reached 16.75% inclusive and the large process high-water mark
   was 103 MiB. Measure moving the admitted body through decode, projection and dispatch while retaining
   the caps, replay ownership and buffer-clearing semantics. [Bounded body read](../../src/request.cpp#L81)
   [DeepSeek projection](../../src/deepseek/index.cpp#L600)
2. **Qualify simdutf 9.4.0 for long UTF-8 and ASCII inputs.** `validPrefix` and `isAscii` together consumed
   21.50% of exclusive large-chat cycles. The profiler reached the validator from both UTF-8 decode and
   `isUtf8`. Use the library's bulk validators on proven well-formed input, with the current scalar
   handling for incomplete, malformed and surrogate cases as specified by plan row 2. [Text codec](../../src/rt/text_codec.cpp#L50)
   [Replacement plan](../OSS-REPLACEMENT-PLAN.md#ranked-component-inventory)
3. **Reduce repeated JSON serialization and quote work.** `appendQuoted` took 14.11% of exclusive cycles
   in large chat, and `jsonStringify` reached 22.37% inclusive. Admission and dispatch each serialize the
   projected Chat body. Measure reuse where the projected bytes match, and scan long ordinary string runs
   in bulk while retaining exact `JSON.stringify` ordering, escapes and lone-surrogate behavior under
   plan row 17. [Admission serialization](../../src/deepseek/forwarded_payload_policy.cpp#L28)
   [Wire serialization](../../src/deepseek/index.cpp#L624) [Quoting](../../src/rt/json.cpp#L184)
4. **Serialize terminal records with fewer intermediate objects.** Rejection's `logTerminalRequest` reached
   21.68% inclusive. Eight allocator leaves accounted for 14.63% exclusive across the rejection path.
   Start by measuring a typed terminal record built once for the existing ordered serializer. Preserve
   emitted fields and the settlement triggered by actual delivery. [Terminal record](../../src/handler/terminal_log.cpp#L128)
5. **Reduce shared ownership churn in request and delivery callbacks.** The rejection run spent 4.13% exclusive
   in the outlined atomic add, led by `std::__shared_count` callers, plus 1.43% exclusive in `_M_release`.
   `LoopExecutor::execute` carried 51.01% inclusive, much of it downstream work. Measure callback allocations
   and frame lifetimes while retaining the Asio scheduler, promise ordering and safe cancellation specified
   by plan row 36. [Executor](../../src/rt/async.cpp#L193) [Replacement plan](../OSS-REPLACEMENT-PLAN.md#ranked-component-inventory)
