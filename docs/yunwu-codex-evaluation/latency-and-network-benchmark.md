# Latency and network benchmark

Date measured: 2026-07-19

## Routes

The benchmark compared:

| Name         | Request path                                                 |
| ------------ | ------------------------------------------------------------ |
| Direct Codex | Client → `chatgpt.com/backend-api/codex/responses`           |
| `ai.ubq.fi`  | Client → Deno Deploy gateway → ChatGPT Codex                 |
| Yunwu        | Client → Yunwu proxy/router → presumed pooled Codex upstream |

The last route's internal topology and physical region were not independently verified. Yunwu advertises service across
several global regions, so it should not be assumed that the tested serving edge was in mainland China.

## Prompt

All routes received the same semantic task:

```text
System: You are a precise senior TypeScript engineer.

User: Implement a TypeScript function stableUnique<T>(
  items: readonly T[],
  key: (item: T) => string
): T[] that preserves first occurrence order.
Include two compact tests and one sentence stating time complexity.
Keep the entire answer under 180 words.
```

The Chat Completions payload selected `gpt-5.6-sol` and included `max_tokens: 300`. `ai.ubq.fi` ignores that field when
translating to the Codex Responses wire format. The direct request therefore omitted it and used the gateway model
catalog's default `medium` reasoning effort.

## Initial matched-token comparison

One early comparison happened to produce exactly 71 input and 231 output tokens on both routes:

| Route       | Total latency | Input/output tokens |
| ----------- | ------------: | ------------------: |
| Yunwu       |       4.795 s |            71 / 231 |
| `ai.ubq.fi` |       7.746 s |            71 / 231 |

In that single matched-output sample, `ai.ubq.fi` took 2.951 seconds longer. One request is not a reliable latency
benchmark.

## Synchronized 20-round benchmark

Twenty measured rounds launched all three requests simultaneously with `Promise.all`. This intentionally exposed common
network disturbances. The p95 is the nearest-rank 19th value of 20 samples, so it should be treated as a coarse tail
estimate.

The complete per-round measurements are preserved in
[Raw synchronized benchmark data](./raw-synchronized-benchmark-data.md).

| Route        |      Median |         p95 |    Mean |     Min |      Max | Median output tokens |
| ------------ | ----------: | ----------: | ------: | ------: | -------: | -------------------: |
| Direct Codex | **5.750 s** | **7.454 s** | 6.161 s | 5.255 s | 11.486 s |                245.5 |
| `ai.ubq.fi`  |     6.782 s |     9.147 s | 7.284 s | 6.042 s | 12.631 s |                  238 |
| Yunwu        |     7.104 s |     9.380 s | 6.727 s | 4.310 s | 10.027 s |                304.5 |

Direct Codex had the best raw median and p95. Relative to direct:

- `ai.ubq.fi` added about 1.03 seconds at the median;
- Yunwu added about 1.35 seconds at the median;
- Yunwu produced roughly 25% more output tokens than the other routes at the median, so its raw total latency is not
  generation-normalized.

Yunwu responses around 235–250 output tokens generally completed in roughly 4.3–4.9 seconds. Its inference throughput
therefore appeared competitive even though its uncapped outputs were often longer.

## Expected `ai.ubq.fi` UX impact

For the tested short response, `ai.ubq.fi` was 17.9% slower at the median and 22.7% slower at p95:

| Measure | Direct Codex | `ai.ubq.fi` | Added time | Relative increase |
| ------- | -----------: | ----------: | ---------: | ----------------: |
| Median  |      5.750 s |     6.782 s |    1.032 s |             17.9% |
| p95     |      7.454 s |     9.147 s |    1.693 s |             22.7% |
| Mean    |      6.161 s |     7.284 s |    1.123 s |             18.2% |

The gateway streams the upstream response rather than waiting for the complete answer. Local tool execution, file I/O,
terminal commands, and user think time are therefore unaffected. Only new model HTTP round trips incur the route's
additional latency.

A simple accumulation model using the measured median delta is:

```text
added task time ≈ model round trips × 1.032 seconds
```

| Model round trips | Naive accumulated median delay |
| ----------------: | -----------------------------: |
|                10 |                         10.3 s |
|                20 |                         20.6 s |
|                50 |                         51.6 s |
|               100 |                        103.2 s |

These totals are planning estimates, not predictions: the synchronized results show substantial provider-specific
queueing variance, so the observed 1.032-second difference should not be treated as a fixed proxy tax. The benchmark
also measured total completion time, not time to first token. A dedicated streaming benchmark is needed to quantify the
delay a user feels before text first appears.

## Cross-route correlation

The synchronized per-round Pearson latency correlations were:

| Pair                       | Correlation |
| -------------------------- | ----------: |
| Yunwu ↔ `ai.ubq.fi`        |      -0.134 |
| Yunwu ↔ direct Codex       |      +0.351 |
| `ai.ubq.fi` ↔ direct Codex |      -0.201 |

The weak or negative correlations show that multi-second latency spikes were generally not shared by all three
providers. Provider/upstream generation and queueing were the dominant sources of variance.

## Network controls

Each synchronized round also sent one ICMP probe to `1.1.1.1`:

| Measure                  |   Result |
| ------------------------ | -------: |
| Sent/received            |  20 / 18 |
| Observed loss            |      10% |
| Median RTT among replies | 23.23 ms |
| Maximum RTT              | 96.56 ms |

This is too small a probe set to estimate internet packet loss reliably, and ICMP may be deprioritized.

A separate 100-packet probe to the local network gateway isolated the Wi-Fi link:

| Measure                     |                     Result |
| --------------------------- | -------------------------: |
| Sent/received               |                  100 / 100 |
| Packet loss                 |                     **0%** |
| Minimum/average/maximum RTT | 2.516 / 14.436 / 97.623 ms |
| Standard deviation          |                  23.680 ms |

The Wi-Fi link showed noticeable jitter but no measured local packet loss. Tens of milliseconds of Wi-Fi variation
cannot explain several seconds of provider-specific completion variance.

## Interpretation

The measured ordering matches the expected forwarding architecture:

```text
Direct:      client → OpenAI
ai.ubq.fi:   client → Deno Deploy → OpenAI → Deno Deploy → client
Yunwu:       client → proxy/router → OpenAI capacity → proxy/router → client
```

Extra network legs, TLS handling, buffering, authentication, and queueing can reasonably add latency. Physical distance
alone is unlikely to explain multi-second differences; model generation and upstream scheduling dominate.

## Benchmark limitation and next test

The output limit was not enforced, and Yunwu generated longer responses. The clean follow-up benchmark should require
every model to copy one fixed literal block and reject responses that differ. That would hold output length constant and
produce a more defensible transport-plus-inference comparison.
