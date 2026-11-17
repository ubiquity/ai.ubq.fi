# C++ gateway performance

The tested ARM64 Linux package used 39.52–39.53 MiB of settled idle RSS and 41.71–41.72 MiB under the eight-concurrent-request smoke load. Bun 1.4.0 used 96.45–96.91 MiB idle and 123.35–125.75 MiB under the same load. The quiet comparison favored C++ on serial request medians and paced stream throughput. Bun led the measured JSON and unpaced SSE burst workloads.

The baseline measurements bind the accepted ARM64 package. Its manifest SHA-256 is `636ccd35ad984fac8280b7599d885bd4bed6d3abd964bcb7ab0ca81d64ce05c0`, and the gateway SHA-256 is `1b3f81123f7b6c81ee114a7dffb94df5d33c4ae89c05f7caff8dacc4cabbcf03`. The Bun executable SHA-256 is `086c4121c8738a8e0f5ed730e8a461bc3973b4444e372ddb77aef9a747fa2ae9`. See [accepted build and package](BUILD.md).

## Paired smoke and memory

Two interleaved trials per runtime used the packaged executable, ordinary authentication and accounting, and an isolated loopback upstream. Both pairs matched their response and terminal projections. Each process produced 87 inference terminals. Durable usage advanced from 86 to 87 after shutdown and reopen, and the upstream dispatch count stayed at 88 during reopen. The original and reopened processes exited cleanly.

| Runtime | Trial | Startup to ready ms | Cold idle MiB | Settled idle median MiB | Concurrent peak MiB |
| ------- | ----: | ------------------: | ------------: | ----------------------: | ------------------: |
| C++     |     1 |              224.63 |         35.34 |                   39.52 |               41.72 |
| Bun     |     1 |              755.58 |         72.38 |                   96.45 |              123.35 |
| Bun     |     2 |              382.70 |         74.38 |                   96.91 |              125.75 |
| C++     |     2 |              168.65 |         35.85 |                   39.53 |               41.71 |

Memory is the actual child's post-exec RSS. The idle median uses 32–33 samples per process, and the concurrency peak uses 33–37 samples during the scripted load. The separate quiet replay sampled 39.52–40.34 MiB idle and 41.71–41.72 MiB peak for C++, and 97.13–97.79 MiB idle and 122.89–124.84 MiB peak for Bun. It included 111 terminals per process and the same successful durability and shutdown checks. [Smoke, samples and executable binding](/tmp/claude-1002/prod/smoke_perf/quiet-v25/final-current-02/smoke-summary.json) and [quiet replay](/tmp/claude-1002/prod/smoke_perf/quiet-v25/final-current-02/quiet-summary.json)

## Quiet HTTP and SSE measurements

The measurement took 58.608 seconds. The supervisor samples at the boundaries reported zero active compiler processes. The host's one-minute load was 0.71 at the start and 2.10 at the end. Both runtimes used the same requests, scripts and parity assertions. Percentiles below are nearest rank over individual requests.

| Serial request            | Samples per runtime | C++ elapsed median / p95 ms | Bun elapsed median / p95 ms | C++ / Bun requests/s |
| ------------------------- | ------------------: | --------------------------: | --------------------------: | -------------------: |
| Buffered Chat Completions |                  30 |               3.364 / 4.023 |              9.236 / 16.271 |       303.70 / 96.36 |
| Streamed Chat Completions |                  30 |               4.122 / 5.376 |              8.906 / 16.869 |      243.36 / 101.32 |
| Buffered Responses        |                  30 |              3.524 / 10.986 |              6.909 / 13.571 |      210.80 / 128.21 |
| Streamed Responses        |                  30 |              4.689 / 19.599 |              9.577 / 14.558 |       139.74 / 96.76 |

For serial streamed Chat Completions, median/p95 time to first complete event was 3.861/5.107 ms in C++ and 8.598/16.524 ms in Bun. Serial Responses was 3.648/16.521 ms in C++ and 8.286/13.825 ms in Bun. The timer stops at the read that completes the event frame, so events in one read can share a timestamp.

| Unpaced burst route | Samples per runtime | C++ elapsed median / p95 ms | Bun elapsed median / p95 ms | C++ / Bun SSE events/s |
| ------------------- | ------------------: | --------------------------: | --------------------------: | ---------------------: |
| Chat Completions    |                  24 |             13.071 / 24.128 |             11.919 / 22.919 |    4,762.46 / 5,200.37 |
| Responses           |                  24 |             14.156 / 47.255 |             13.723 / 19.261 |    4,010.08 / 5,074.73 |

The burst delivers all 64 text deltas per response without an upstream interframe delay. The rates for serial and burst rows divide completed work by the sum of the client request durations. The paced, eight-concurrent-request trials use wall time for throughput. Each trial serves 16 streams and emits 1,112 SSE data events, including 1,024 text deltas.

| Paced trial | Elapsed median / p95 ms | Requests/s | SSE events/s |
| ----------- | ----------------------: | ---------: | -----------: |
| C++ 1       |       344.483 / 350.975 |      22.60 |     1,570.59 |
| Bun 1       |       402.801 / 435.410 |      18.82 |     1,307.98 |
| Bun 2       |       367.249 / 426.212 |      20.10 |     1,396.73 |
| C++ 2       |       354.995 / 381.199 |      21.45 |     1,490.66 |

The paced workload uses 64 frames and the same 5 ms upstream interframe delay for each runtime. Exact Chat Completions bytes and Responses byte counts and ordered protocol projections matched after normalizing generated identities and clocks. [Raw HTTP/SSE timings and byte counts](/tmp/claude-1002/prod/smoke_perf/quiet-v25/final-current-02/quiet/request-sse/result.json) and [throughput definitions](/tmp/claude-1002/prod/smoke_perf/quiet-v25/final-current-02/quiet/request-sse/sse-request-summary.json)

## Quiet JSON component measurements

The native component calls the production parser and stringifier on identical input bytes, with the checked ASCII quote optimization. The benchmark uses 36 interleaved process trials with matching output checksums. Each runtime and workload has three first-call samples and 24 measured warm batch means. JSON warm p95 therefore measures the distribution of batch-average cost per operation.

| Input bytes | Operation | C++ / Bun cold median µs | C++ warm median / p95 µs | Bun warm median / p95 µs |   C++ / Bun ops/s | C++ / Bun warm median |
| ----------: | --------- | -----------------------: | -----------------------: | -----------------------: | ----------------: | --------------------: |
|       1,088 | Parse     |            88.16 / 95.32 |            13.78 / 14.44 |              4.10 / 5.08 |  71,154 / 227,168 |                 3.36× |
|       1,088 | Stringify |            27.60 / 28.32 |              5.92 / 6.25 |              1.81 / 2.44 | 168,486 / 532,867 |                 3.26× |
|      15,968 | Parse     |          264.92 / 152.64 |          217.39 / 304.15 |           58.98 / 131.17 |    4,267 / 13,334 |                 3.69× |
|      15,968 | Stringify |           116.36 / 77.36 |           99.57 / 214.44 |            41.91 / 80.77 |    8,047 / 21,234 |                 2.38× |
|   1,027,878 | Parse     |     25,589.01 / 8,339.93 |    22,336.84 / 38,550.20 |      5,785.18 / 7,651.55 |          40 / 179 |                 3.86× |
|   1,027,878 | Stringify |      6,714.36 / 2,347.29 |      6,291.50 / 6,681.94 |      1,704.97 / 4,406.70 |         164 / 490 |                 3.69× |

C++ warm medians ranged from 2.38 to 3.86 times Bun across the six baseline workloads. The native benchmark SHA-256 is `babc537ba1ba4a6975919d0cae2b3c50b5d6d97b28dccfab3b29295959a6160b`, and the accepted `src/rt/json.cpp` SHA-256 is `21bcee7923eed33b90ae31a4eb700916e7113bb590483739bde7d1987d44a803`. [Exact JSON inputs, timings and parity](/tmp/claude-1002/prod/smoke_perf/quiet-v25/final-current-02/quiet/json/json/json-results.json)

## Quiet regex component measurements

All 18 fixed workloads matched Bun output, captures, UTF-16 indices and observable `lastIndex`. The 36 child processes completed, each with three warmup batches and seven measured samples. Median C++/Bun ratios ranged from 0.68× to 6.51×. Selected rows show the range of observed component costs.

| Workload             | Input bytes | C++ median / p95 ms | Bun median / p95 ms |             C++ / Bun ops/s |
| -------------------- | ----------: | ------------------: | ------------------: | --------------------------: |
| Word redaction       |     210,000 |   41.8596 / 73.9331 |   14.0480 / 18.6994 |               23.89 / 71.18 |
| Dot redaction        |     200,000 | 734.4954 / 746.3964 | 119.6233 / 119.8914 |                 1.36 / 8.36 |
| Dash redaction       |     210,000 | 464.9798 / 465.8143 |   71.4218 / 72.8894 |                2.15 / 14.00 |
| Bracket redaction    |     225,000 | 147.3539 / 150.9384 | 162.3813 / 217.5972 |                 6.79 / 6.16 |
| Unicode sticky match |          13 |     0.0003 / 0.0004 |     0.0004 / 0.0012 | 3,548,553.08 / 2,423,654.87 |

The word-redaction profile collected 400 stack samples, with 326 in the matcher. These are stack sample counts. The timing receipt preserves every workload and its sample and throughput data. [All 18 measured results](../../../.data/cpp-gateway-evidence/quiet-final-current-02/regex-timing/results.json) and [profile summary](/tmp/claude-1002/prod/smoke_perf/quiet-v25/final-current-02/quiet-summary.json)

## Runtime library follow-up

A separate matched trial used Boost.JSON 1.92 on the common parse path and the JavaScript-compatible parser for exceptional inputs. All three implementations ran in the same quiet window, with 24 warm batch means per row. The candidate sped up native parsing by about 24%, 55% and 55% at the three sizes.

| Input bytes | Frozen native median µs | Boost.JSON median µs | Bun median µs | Boost.JSON / Bun |
| ----------: | ----------------------: | -------------------: | ------------: | ---------------: |
|       1,088 |                  22.865 |               17.433 |         5.956 |           2.927× |
|      15,968 |                 229.091 |              103.335 |        56.792 |           1.820× |
|   1,027,878 |              14,830.352 |            6,638.688 |     3,674.014 |           1.807× |

The candidate source SHA-256 is `e233b06e291c59477dc41adc4fa618a09ce8d1e00349f86b89e8285fc13e965b`. It matched 91 native fixture rows and all 86 Bun-comparable rows. The original JSON suite passed 24/24, and the working-source version with the added regression passed 25/25 in focused release qualification. Those receipts cover the later parser source. The 441-binary release, package and smoke receipts above bind the earlier source. [Matched timing](/tmp/claude-1002/prod/json_library/TRIAL-RESULT-v26.md), [finite equivalence](../../../.data/cpp-json-v26-trial/equivalence-parse-r2/result.json) and [working-source regression](../../../.data/cpp-json-v30-publish/unchanged-regression/result.json)

- **Boost quoting rejected.** The independently timed quoting source `24f3907c863f39882c7f45dec58a7d07d17162c23762703f2e0d0c1acd769c41` raised stringify medians by 1.3%, 1.5% and 9.6%. The existing serializer and the baseline ASCII quote optimization were retained. [Quote timing](../../../.data/cpp-json-v26-trial/quiet-quote/result.json)
- **Regex word-boundary optimization rejected.** Trial source `d1201eafd79f0757127c3b78bb7f0897323005e2c733fcba0a95c6abc003cee5` improved two redaction and lookup workloads, while bracket and quote redaction medians increased by 21.5% and 14.2%. The measurements supported keeping the baseline matcher. [Regex trial and decision](/tmp/claude-1002/prod/idna_regex/quiet-v25/word-boundary-candidate/TRIAL-RESULT.md)

## Operator checks

Use the [same-binary smoke command](../tests/production_smoke/README.md) with the immutable package, build receipt and fresh scratch directory. The existing `smoke.py` entry point also accepts `--json-benchmark` and the exact benchmark build receipt. Use the [regex commands](../bench/regex/README.md) for the production regex component. The actual paired request/SSE argv and OS boundary checks are recorded with the current benchmark. [Exact timed request invocation](/tmp/claude-1002/prod/smoke_perf/quiet-v25/final-current-02/quiet/request-sse.receipt.json)

The Raspberry Pi still needs its own ARM64 loader, exact library closure, process confinement support and available RAM under its real workload. The VPS measurements establish candidate service RSS under the named fixture. See [platform qualification](BUILD.md#platform-receipts).
