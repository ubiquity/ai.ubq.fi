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
python3 -I "$bench/profile.py" "$scratch/profile-large-final/01-large-native/perf.data" --top 60
python3 -I "$bench/profile.py" "$scratch/profile-reject-final/01-reject-native/perf.data" --top 60
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

<!-- PROFILE_TABLES -->

## Out-of-scope findings and limits

- The shipped packager's `find_library` searches direct children of its library roots. On Ubuntu it refused
  `libzstd.so.1` despite the installed library under the ARM64 multiarch directory. The benchmark wrapper
  adds that directory to the existing packager's lookup and leaves startup and host hash verification in
  place. [Library lookup](../../tools/runtime-manifest.py#L56)
- The synthetic large body uses long ASCII fields that fit the provider's per-message limit. The run measures
  one prompt shape, one provider and four local clients. It does not estimate real upstream latency, broader
  production traffic, or an upper throughput bound. Frame pointers were kept in the timed executable to
  measure the same binary that was profiled.
- Git origin permits fetches but rejected the authenticated push preflight. The task branch is committed
  locally, based on the C++ port branch for the maintainer's integration. Production deployment was outside
  the task.

## Ranked performance work

<!-- RANKED_ACTIONS -->
