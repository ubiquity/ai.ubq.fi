# C++ gateway performance

The selected ARM64 release uses simdjson 5.2.0 DOM and simdutf 9.4.0 Base64 encoding. It passed 441/441 binaries and 5,024/5,024 cases. In the paired smoke, C++ used 39.395–39.398 MiB of settled idle RSS and 41.590 MiB at the eight-concurrent-request peak. Bun 1.4.0 used 98.680–99.168 MiB idle and 120.629–124.582 MiB at peak. In the quiet trial, C++ led all four serial request medians and both unpaced SSE burst rates. Bun led JSON parse and stringify on the three timed documents. [Build and package](BUILD.md)

These results bind manifest SHA-256 `8c0600e80ccecbbc6cdcf8f471ffe340de64e6c7c2f8bb850ee8a50d7187cbd1`, gateway SHA-256 `58cd26cca3b8c40ac08c5ad09a3e4be87adfc8f0a0cd900e0b6fd42a18f5fa22` and Bun executable SHA-256 `086c4121c8738a8e0f5ed730e8a461bc3973b4444e372ddb77aef9a747fa2ae9`. The Bun comparison uses the pinned source application, the existing Node HTTP compatibility loader and a genuine native KV artifact. The trials share the same ARM64 VPS, request data, scripted upstream and isolated loopback. [Quiet result and bindings](/tmp/claude-1002/prod/smoke_perf/quiet-v25/selected-v32-current-01/quiet-summary.json)

## Paired smoke and memory

Two trials per runtime used normal application authentication, delivery and accounting. The native trials started the packaged `ops/launch-native.sh` and verified the expected gateway executable after readiness, including after reopen. Both paired comparisons matched. Each trial recorded 87/87 terminals, with durable usage increasing from 86 to 87 and the upstream dispatch count staying at 88 across reopen. Original and reopened processes exited cleanly. [Exact smoke commands and outcomes](/tmp/claude-1002/prod/smoke_perf/quiet-v25/selected-v32-current-01/smoke-summary.json)

| Runtime | Trial | Startup to ready ms | Cold idle MiB | Settled idle median MiB | Concurrent peak MiB |
| ------- | ----: | ------------------: | ------------: | ----------------------: | ------------------: |
| C++     |     1 |              206.41 |        35.328 |                  39.395 |              41.590 |
| Bun     |     1 |             1120.25 |        71.750 |                  99.168 |             120.629 |
| Bun     |     2 |              508.41 |        75.520 |                  98.680 |             124.582 |
| C++     |     2 |              175.51 |        35.465 |                  39.398 |              41.590 |

The samples are post-exec RSS from the actual runtime process. The separate quiet replay recorded 111 terminals per trial, with 39.398 MiB settled idle and 41.574–41.605 MiB peak for C++, and 97.719–97.859 MiB idle and 122.555–123.137 MiB peak for Bun. [Quiet replay and raw RSS references](/tmp/claude-1002/prod/smoke_perf/quiet-v25/selected-v32-current-01/quiet-summary.json)

## Quiet HTTP and SSE

The isolated measurement finished in 53.980 seconds. The supervisor samples at the boundaries recorded no active compiler process and one-minute host load of 1.11 and 1.77. Request percentiles are nearest rank over individual client requests.

| Serial request            | Samples per runtime | C++ median / p95 ms | Bun median / p95 ms | C++ / Bun requests/s |
| ------------------------- | ------------------: | ------------------: | ------------------: | -------------------: |
| Buffered Chat Completions |                  30 |      2.809 / 16.100 |     10.020 / 20.034 |       216.03 / 86.49 |
| Streamed Chat Completions |                  30 |       3.284 / 7.729 |      8.919 / 15.513 |      267.15 / 103.99 |
| Buffered Responses        |                  30 |       2.852 / 3.993 |      7.045 / 12.022 |      329.30 / 131.60 |
| Streamed Responses        |                  30 |       3.484 / 5.722 |      8.309 / 14.353 |      264.72 / 110.86 |

First complete event median/p95 in serial streamed Chat Completions was 2.952/6.684 ms for C++ and 8.661/15.303 ms for Bun. Streamed Responses measured 2.928/4.946 ms for C++ and 7.739/13.653 ms for Bun. The timing stops at the read that completes the event frame.

| Unpaced 64-delta burst | Samples per runtime | C++ median / p95 ms | Bun median / p95 ms | C++ / Bun SSE events/s |
| ---------------------- | ------------------: | ------------------: | ------------------: | ---------------------: |
| Chat Completions       |                  24 |       7.183 / 8.911 |     10.252 / 19.488 |    8,998.28 / 6,024.77 |
| Responses              |                  24 |      7.932 / 14.332 |     13.326 / 25.588 |    8,417.11 / 4,989.16 |

Serial and burst rates divide completed work by summed client request durations. The paced workload uses wall time, 16 requests at concurrency eight and a 5 ms upstream interframe delay. Each trial produced 1,112 SSE events, including 1,024 text deltas.

| Paced trial |   Median / p95 ms | Requests/s | SSE events/s |
| ----------- | ----------------: | ---------: | -----------: |
| C++ 1       | 344.185 / 358.514 |      22.73 |     1,579.95 |
| Bun 1       | 365.026 / 373.075 |      21.55 |     1,497.54 |
| Bun 2       | 391.543 / 413.762 |      20.05 |     1,393.20 |
| C++ 2       | 361.343 / 372.783 |      21.87 |     1,519.90 |

Fixed Chat response bytes and ordered Responses projections matched after normalizing generated identities and clocks. [Raw request timings](/tmp/claude-1002/prod/smoke_perf/quiet-v25/selected-v32-current-01/quiet/request-sse/result.json) and [SSE timing and parity definitions](/tmp/claude-1002/prod/smoke_perf/quiet-v25/selected-v32-current-01/quiet/request-sse/sse-request-summary.json)

## Quiet JSON component

The integrated benchmark uses the production DOM parser and JavaScript stringifier. Both runtimes process identical hashed inputs and match their canonical outputs. Each workload uses three first-call samples and 24 warm batch means. Warm p95 is the nearest-rank percentile of batch-average time per operation.

| Input bytes | Operation | C++ / Bun first-call median µs | C++ warm median / p95 µs | Bun warm median / p95 µs |   C++ / Bun ops/s | C++ / Bun warm median |
| ----------: | --------- | -----------------------------: | -----------------------: | -----------------------: | ----------------: | --------------------: |
|       1,088 | Parse     |                 63.88 / 100.24 |            5.369 / 7.052 |            4.078 / 4.956 | 178,277 / 229,856 |                1.317× |
|       1,088 | Stringify |                  35.96 / 28.12 |            5.911 / 7.128 |            1.975 / 2.440 | 162,652 / 478,447 |                2.994× |
|      15,968 | Parse     |                156.48 / 152.00 |          85.993 / 87.315 |          55.211 / 67.294 |   11,597 / 17,721 |                1.558× |
|      15,968 | Stringify |                 128.04 / 79.08 |          81.851 / 86.339 |          30.919 / 42.171 |   12,116 / 30,656 |                2.647× |
|   1,027,878 | Parse     |            8,167.29 / 4,168.79 |    6,148.290 / 8,415.255 |    3,774.645 / 4,936.002 |         153 / 252 |                1.629× |
|   1,027,878 | Stringify |            8,209.01 / 5,382.67 |   8,310.544 / 12,387.541 |    2,408.711 / 5,362.745 |         119 / 336 |                3.450× |

Native benchmark SHA-256 is `4db76ff681bf3d7c6fadbea88bbe222c313e6682b49d0da3382325ef46f7ae29`. The native JSON source is `33e9333508c989775a1700015c6bed2e3f21cf21d6e77703794b8c4fe017c585`. The pre-library build had native warm parse medians of 13.783, 217.390 and 22,336.838 µs/op at these sizes. The largest native stringify median was 6,291.502 µs/op in the earlier run and 8,310.544 µs/op in the selected run. These package measurements used separate quiet windows. Use the matched trials below to compare parser alternatives. [Selected JSON raw results](/tmp/claude-1002/prod/smoke_perf/quiet-v25/selected-v32-current-01/quiet/json/json/json-results.json) and [pre-library quiet control](/tmp/claude-1002/prod/smoke_perf/quiet-v25/final-current-02/quiet-summary.json)

## Quiet regex component

All 18 workloads and 36 child processes completed with matching Bun text, captures, UTF-16 indices and observable `lastIndex`. Each child uses three warmup batches and seven measured samples. The reported p95 summarizes the per-operation batch means. The word-redaction profile sampled the matcher in 324 of 399 stacks.

| Workload             | Input bytes | C++ median / p95 ms | Bun median / p95 ms |             C++ / Bun ops/s |
| -------------------- | ----------: | ------------------: | ------------------: | --------------------------: |
| Word redaction       |     210,000 |   40.1569 / 42.8673 |    9.4455 / 10.0763 |              24.90 / 105.87 |
| Dot redaction        |     200,000 | 741.2672 / 751.7614 | 120.4570 / 129.5102 |                 1.35 / 8.30 |
| Dash redaction       |     210,000 | 465.5700 / 496.4042 |   74.5870 / 75.9841 |                2.15 / 13.41 |
| Bracket redaction    |     225,000 | 153.4617 / 157.7608 | 153.5590 / 174.6593 |                 6.52 / 6.51 |
| Unicode sticky match |          13 |     0.0003 / 0.0004 |     0.0004 / 0.0009 | 3,571,364.80 / 2,256,292.23 |

[All 18 selected regex results](../../../.data/cpp-gateway-evidence/quiet-selected-v32-current-01/regex-timing/results.json) and [profile evidence](/tmp/claude-1002/prod/smoke_perf/quiet-v25/selected-v32-current-01/quiet-summary.json)

## Selected Base64 component

The isolated simdutf encode trial passed 31 original cases and 3,866 fixed comparisons, with 43,064,932 equal output bytes. Standard encoding improved 1.52× and 2.59× at 12 and 32 bytes and 9.61× at 64 KiB. Three- and four-byte encodes added 5–11 ns. These are standalone codec measurements. UTF conversion and decoder trials have separate acceptance. [Base64 timing and parity](/tmp/claude-1002/prod/simdutf_eval/TRIAL-RESULT-v32.md)

## Candidate comparisons

A separate matched trial used Boost.JSON 1.92 on the common parse path and the JavaScript-compatible parser for exceptional inputs. All three implementations ran in the same quiet window, with 24 warm batch means per row. The candidate sped up native parsing by about 24%, 55% and 55% at the three sizes.

| Input bytes | Frozen native median µs | Boost.JSON median µs | Bun median µs | Boost.JSON / Bun |
| ----------: | ----------------------: | -------------------: | ------------: | ---------------: |
|       1,088 |                  22.865 |               17.433 |         5.956 |           2.927× |
|      15,968 |                 229.091 |              103.335 |        56.792 |           1.820× |
|   1,027,878 |              14,830.352 |            6,638.688 |     3,674.014 |           1.807× |

The intermediate Boost parser source SHA-256 is `e233b06e291c59477dc41adc4fa618a09ce8d1e00349f86b89e8285fc13e965b`. It matched 91 native fixture rows and all 86 Bun-comparable rows. The original 24 cases and the expanded 25-case focused release suite passed. The selected simdjson DOM/default-storage trial then reduced parse medians by 13.34%, 15.53% and 17.35% versus the intermediate Boost path in a matched window. The selected global API keeps independent default Boost storage. [Boost timing](/tmp/claude-1002/prod/json_library/TRIAL-RESULT-v26.md), [Boost equivalence](../../../.data/cpp-json-v26-trial/equivalence-parse-r2/result.json) and [simdjson candidate comparison](/tmp/claude-1002/prod/simdjson_eval/RESULT-v32.md)

- **Boost quoting rejected.** The independently timed quoting source `24f3907c863f39882c7f45dec58a7d07d17162c23762703f2e0d0c1acd769c41` raised stringify medians by 1.3%, 1.5% and 9.6%. The existing serializer and the baseline ASCII quote optimization were retained. [Quote timing](../../../.data/cpp-json-v26-trial/quiet-quote/result.json)
- **Regex word-boundary optimization rejected.** Trial source `d1201eafd79f0757127c3b78bb7f0897323005e2c733fcba0a95c6abc003cee5` improved two redaction and lookup workloads, while bracket and quote redaction medians increased by 21.5% and 14.2%. The measurements supported keeping the baseline matcher. [Regex trial and decision](/tmp/claude-1002/prod/idna_regex/quiet-v25/word-boundary-candidate/TRIAL-RESULT.md)

## Operator checks

Use the [native smoke guide](../tests/production_smoke/README.md) with a sealed package and fresh private scratch. The [native JSON benchmark](../bench/paths/README.md) and [native regex benchmark](../bench/regex/README.md) describe current measurement commands.

The paired comparison with the TypeScript gateway lives at [commit `ca045a83ebaab6c50c20567d1a825638abac0891`](https://github.com/ubiquity/ai.ubq.fi/tree/ca045a83ebaab6c50c20567d1a825638abac0891/cpp/gateway/conformance).

Raspberry Pi qualification requires the target’s loader, library and confinement compatibility plus measured memory under its own workload. [Platform and ABI requirements](BUILD.md#platform-receipts)
