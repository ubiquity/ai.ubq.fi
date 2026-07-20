# Pricing and subscription economics

Date measured: 2026-07-19

## Verified Yunwu billing

A real `gpt-5.6-sol` request routed through Yunwu's `Codex专属` group produced:

| Item                           |                Measured value |
| ------------------------------ | ----------------------------: |
| Input tokens                   |                            71 |
| Output tokens                  |                           231 |
| Total tokens                   |                           302 |
| Balance deduction              |              0.005828 credits |
| Input rate before group ratio  |  5 credits per million tokens |
| Output rate before group ratio | 30 credits per million tokens |
| Group ratio                    |                           0.8 |

The billing record reconciled exactly:

```text
input  = 71 / 1,000,000 × 5 × 0.8  = 0.000284 credits
output = 231 / 1,000,000 × 30 × 0.8 = 0.005544 credits
total                                   0.005828 credits
```

The recharge screen showed:

```text
100 credits = 49.5 CNY
1 credit    = 0.495 CNY
```

Using the contemporaneous conversion rate of approximately 6.7767 CNY/USD:

| Direction | Yunwu effective price | Displayed official price | Yunwu/official |
| --------- | --------------------: | -----------------------: | -------------: |
| Input     |        about $0.292/M |                     $5/M |          5.84% |
| Output    |        about $1.753/M |                    $30/M |          5.84% |

Therefore the measured route was:

- **94.16% below displayed official API pricing**;
- approximately **17.1 times cheaper than API list pricing**.

This validates Yunwu's advertised “94% cheaper” claim for the tested Codex-specific route. It does not validate every
Yunwu routing group.

## Equal cash top-up

At the measured recharge rate:

```text
100 credits ≈ $7.30
1 credit    ≈ $0.0730
```

Spending the same cash as a $200 subscription would buy approximately:

```text
$200 × 6.7767 CNY/USD / 0.495 CNY per credit ≈ 2,738 credits
```

That is an equal-spend comparison, not an equal-compute comparison.

## Comparing Yunwu with the $200 Codex subscription

OpenAI does not publish a fixed API-equivalent dollar value for a fully used ChatGPT Pro/Codex subscription. The
effective subsidy depends on:

- how completely the weekly allowance is used;
- the input/output mix;
- caching;
- model and reasoning settings; and
- whether the workload is rate-limited before the subscription month ends.

Let `S` be the subscription's effective API discount multiplier. For example, `S = 27` means the subscription delivered
27 times the compute that $200 would buy at API list prices.

At Yunwu's measured 5.84% of API pricing:

```text
Yunwu cost for the same monthly work = $200 × S × 0.0584
```

| Assumed Codex value | API-equivalent monthly use | Yunwu cost for same compute | Yunwu credits | Relative to $200 |
| ------------------- | -------------------------: | --------------------------: | ------------: | ---------------: |
| 16×                 |                     $3,200 |                  about $187 |   about 2,558 |            0.93× |
| 27×                 |                     $5,400 |                  about $315 |   about 4,317 |            1.58× |
| 47×                 |                     $9,400 |                  about $549 |   about 7,516 |            2.74× |

The 16×, 27×, and 47× values are scenarios derived from anecdotal estimates, not official OpenAI guarantees.

## Break-even point

Yunwu costs the same $200 when monthly official-API-equivalent use reaches:

```text
$200 / 0.0584 ≈ $3,425
```

Consequently:

- below about **$3,425 API-equivalent use per month**, Yunwu is cheaper than paying $200;
- above that amount, a fully utilized $200 subscription is cheaper per unit of compute;
- Yunwu remains useful as pay-as-you-go overflow after the subscription reaches a weekly limit.

## Practical recommendation

Use the subscription first and Yunwu as overflow. Determine the correct Yunwu top-up from observed overflow:

```text
required Yunwu dollars = overflow API-equivalent dollars × 0.0584
required credits       = required Yunwu dollars / 0.0730
```

Avoid holding a large prepaid balance until route stability, privacy, and longer-term pricing have been observed.

## Sources

- [Yunwu pricing](https://yunwu.ai/pricing)
- [Yunwu API documentation](https://yunwu.apifox.cn/)
- [OpenAI Codex rate card](https://help.openai.com/en/articles/20001106)
- [About ChatGPT Pro](https://help.openai.com/en/articles/9793128-about-chatgpt-pro)
