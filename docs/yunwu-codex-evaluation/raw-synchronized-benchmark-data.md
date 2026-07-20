# Raw synchronized benchmark data

Date measured: 2026-07-19

This table contains the 20 measured rounds used by [Latency and network benchmark](./latency-and-network-benchmark.md).
All three provider calls and the ICMP probe began together in each round.

`Outputs` lists completion tokens in Yunwu / `ai.ubq.fi` / direct-Codex order. `Loss` means the single ICMP probe to
`1.1.1.1` did not return within the probe window; it does not imply that the three HTTPS requests failed.

| Round | Yunwu ms | `ai.ubq.fi` ms | Direct Codex ms | Probe ms |         Outputs |
| ----: | -------: | -------------: | --------------: | -------: | --------------: |
|     1 |   10,027 |          8,560 |           5,737 |   22.876 | 292 / 239 / 249 |
|     2 |    7,417 |          6,453 |           6,421 |   19.440 | 329 / 230 / 227 |
|     3 |    4,454 |          6,720 |           5,941 |   14.937 | 237 / 241 / 238 |
|     4 |    4,430 |         12,631 |           5,498 |   32.495 | 235 / 237 / 248 |
|     5 |    4,849 |          6,843 |           5,484 |   21.889 | 244 / 248 / 241 |
|     6 |    4,310 |          8,642 |           5,529 |   26.595 | 250 / 239 / 248 |
|     7 |    6,553 |          6,469 |           5,255 |   23.523 | 295 / 245 / 233 |
|     8 |    7,108 |          6,283 |           5,605 |   18.065 | 322 / 238 / 234 |
|     9 |    7,005 |          6,266 |           5,762 |   18.375 | 311 / 236 / 249 |
|    10 |    7,099 |          7,066 |           6,193 |   58.592 | 291 / 254 / 251 |
|    11 |    8,512 |          9,147 |           5,709 |     Loss | 379 / 237 / 249 |
|    12 |    7,230 |          6,131 |           5,319 |   79.241 | 314 / 220 / 240 |
|    13 |    6,784 |          6,226 |           7,454 |   21.138 | 303 / 237 / 249 |
|    14 |    8,368 |          6,955 |           5,855 |   96.562 | 310 / 246 / 237 |
|    15 |    9,380 |          6,708 |          11,486 |   25.686 | 319 / 233 / 246 |
|    16 |    4,412 |          6,042 |           5,843 |   14.999 | 238 / 238 / 248 |
|    17 |    4,610 |          7,016 |           6,463 |   23.949 | 238 / 243 / 237 |
|    18 |    7,381 |          7,063 |           5,489 |   25.099 | 306 / 239 / 254 |
|    19 |    7,210 |          8,208 |           5,399 |   22.940 | 336 / 236 / 245 |
|    20 |    7,399 |          6,244 |           6,770 |     Loss | 336 / 237 / 239 |

## Derived statistics

| Route        | Samples | Median ms | p95 ms | Mean ms | Min ms | Max ms | Median output tokens |
| ------------ | ------: | --------: | -----: | ------: | -----: | -----: | -------------------: |
| Yunwu        |      20 |     7,104 |  9,380 |   6,727 |  4,310 | 10,027 |                304.5 |
| `ai.ubq.fi`  |      20 |     6,782 |  9,147 |   7,284 |  6,042 | 12,631 |                  238 |
| Direct Codex |      20 |     5,750 |  7,454 |   6,161 |  5,255 | 11,486 |                245.5 |

Nearest-rank p95 was used:

```text
sorted_values[ceil(0.95 × 20) - 1]
```

## Per-round latency correlations

| Pair                       | Pearson correlation |
| -------------------------- | ------------------: |
| Yunwu ↔ `ai.ubq.fi`        |             -0.1336 |
| Yunwu ↔ direct Codex       |             +0.3511 |
| `ai.ubq.fi` ↔ direct Codex |             -0.2011 |

## Probe summary

| Measure              |     Result |
| -------------------- | ---------: |
| Probes sent/received |    20 / 18 |
| Observed loss        |        10% |
| Median reply         | 23.2315 ms |
| Maximum reply        |  96.562 ms |

The local-router control is recorded separately in the main latency report.
