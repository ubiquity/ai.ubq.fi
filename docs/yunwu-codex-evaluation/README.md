# Yunwu as a Codex replacement: evaluation index

Date of investigation: 2026-07-19

This directory records the pricing, latency, protocol, routing, and security findings from testing Yunwu's `gpt-5.6-sol`
route against:

1. the ChatGPT Codex subscription backend directly;
2. `ai.ubq.fi`, a Deno Deploy gateway to that backend; and
3. Yunwu's OpenAI-compatible API.

## Reports

- [Pricing and subscription economics](./pricing-and-subscription-economics.md)
- [Assistant turn unit economics](./assistant-turn-unit-economics.md)
- [Latency and network benchmark](./latency-and-network-benchmark.md)
- [Raw synchronized benchmark data](./raw-synchronized-benchmark-data.md)
- [Token-limit and model-routing diagnostics](./token-limit-and-model-routing.md)
- [Security and operational risks](./security-and-operational-risks.md)

## Executive conclusion

- Yunwu's tested Codex-specific route charged about **5.84% of the displayed official API price**, or approximately
  **1/17.1 of API list pricing**.
- That short-context discount does not extend to Terra requests above 272K. A verified 600,086-token request switched
  to Yunwu's separate `官转` group, recovered markers placed through 580K, and cost about **21.93% of OpenAI retail**
  (**4.56x cheaper**). An 870K-local-token payload was rejected against Yunwu's 1M system limit.
- A fully utilized $200 ChatGPT Pro/Codex subscription may still deliver cheaper compute than Yunwu. The exact
  comparison cannot be known without measuring the subscription's monthly API-equivalent usage.
- Purchased OpenAI Codex overage credits map GPT-5.6 Sol back to API-list pricing: $5/M input and $30/M output. The
  measured Yunwu route is about 17.1x cheaper, while official overage provides the safer direct-provider path.
- OpenAI `/fast` consumes GPT-5.6 ChatGPT credits at 2.5x the standard rate for about 1.5x speed. Yunwu's tested route
  normalized both `fast` and `priority` tier requests to `default`; its “price priority” setting is routing policy, not
  Codex Fast mode.
- Direct ChatGPT Codex was the lowest-latency route in the synchronized benchmark. `ai.ubq.fi` and Yunwu added modest
  end-to-end latency.
- Yunwu silently ignored all tested output-limit fields for `gpt-5.6-sol`. Direct ChatGPT Codex rejected the
  corresponding Responses parameter as unsupported. This is consistent with Yunwu stripping unsupported fields before
  forwarding an uncapped request.
- Black-box testing cannot prove model identity, but the matching tokenizer counts, reasoning-token accounting,
  unsupported-parameter behavior, and output distribution are all consistent with Yunwu forwarding to the real Codex
  subscription backend.
- The tested Yunwu credential must be rotated because it was exposed during the investigation. Yunwu's console also
  opened a third-party domain with the full credential embedded in the URL.

## Evidence boundary

The numerical results in these reports are measurements from a small test window, not service-level guarantees. Pricing,
routing, model availability, limits, and provider behavior can change without notice.
