# Pricing and subscription economics

Date measured: 2026-07-19

## Verified metered billing

A real `gpt-5.6-sol` request routed through Metered's `Codex专属` group produced:

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

| Direction | metered effective price | Displayed official price | Metered/official |
| --------- | ----------------------: | -----------------------: | ---------------: |
| Input     |          about $0.292/M |                     $5/M |            5.84% |
| Output    |          about $1.753/M |                    $30/M |            5.84% |

Therefore the measured route was:

- **94.16% below displayed official API pricing**;
- approximately **17.1 times cheaper than API list pricing**.

This validates Metered's advertised “94% cheaper” claim for the tested Codex-specific route. It does not validate every
Metered routing group.

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

## Official OpenAI overage credits

OpenAI now lets Plus and Pro users purchase credits that extend Codex usage after the plan's included allowance is
exhausted. Included usage is consumed first; only subsequent usage draws from the purchased balance.

The Codex auto-reload screen observed on 2026-07-21 showed:

```text
500 OpenAI credits = $20
5,000 OpenAI credits = $200
1 OpenAI credit = $0.04
```

OpenAI's current token-based Codex rate card charges GPT-5.6 Sol 125 credits per million input tokens, 12.5 credits per
million cached-input tokens, and 750 credits per million output tokens. At $0.04 per credit, this converts to:

| Token type   | OpenAI credits/M | OpenAI overage price | Measured metered price | OpenAI/Metered |
| ------------ | ---------------: | -------------------: | ---------------------: | -------------: |
| Input        |              125 |              $5.00/M |               $0.292/M |          17.1x |
| Cached input |             12.5 |              $0.50/M |           Not measured |              - |
| Output       |              750 |             $30.00/M |               $1.753/M |          17.1x |

OpenAI states that Codex credit pricing was changed to align with API token usage. These credits are therefore
convenient official overage, not subscription-subsidized compute. For the tested GPT-5.6 Sol route, metered costs about
5.84% as much:

| Official OpenAI overage spend | metered cost for approximately the same compute |
| ----------------------------: | ----------------------------------------------: |
|                           $20 |                                           $1.17 |
|                          $100 |                                           $5.84 |
|                          $200 |                                          $11.68 |

The analytics screen also showed 2,918.309 credits in one usage-history entry. If that amount were charged against a
purchased balance, it would represent:

```text
2,918.309 credits x $0.04 = $116.73 of OpenAI overage
$116.73 x 0.0584          = about $6.82 through Metered
```

The history entry may represent included usage expressed in credit units rather than an actual overage charge; the
calculation above is the counterfactual paid value.

Auto-reload should always have a monthly spending ceiling. A 500-credit minimum and 5,000-credit target can trigger a
charge that restores the balance toward $200, and leaving the monthly maximum blank allows repeated reloads.

## Codex `/fast` economics

OpenAI documents Codex `/fast` as approximately 1.5x faster for supported models. For GPT-5.6 and GPT-5.5 it consumes
ChatGPT credits at 2.5x the standard rate. Applied to the GPT-5.6 Sol overage rate card, that is equivalent to:

| Token type   | Standard OpenAI overage | OpenAI `/fast` at 2.5x |
| ------------ | ----------------------: | ---------------------: |
| Input        |                 $5.00/M |               $12.50/M |
| Cached input |                 $0.50/M |                $1.25/M |
| Output       |                $30.00/M |               $75.00/M |

OpenAI separately documents Priority processing for API-key traffic at 2x standard GPT-5.6 API pricing. That is not the
same billing surface as the ChatGPT-credit `/fast` mode.

Metered did not expose an equivalent working speed tier in testing on 2026-07-21:

- a normal request returned `service_tier: "default"`;
- `service_tier: "fast"` was accepted but also returned `service_tier: "default"`;
- `service_tier: "priority"` was accepted but also returned `service_tier: "default"`; and
- Metered's model catalog advertised reasoning-effort suffixes, but no `-fast` model suffix or Fast surcharge.

Consequently, Metered's tested standard route should not be described as a cheaper implementation of Codex `/fast`. It
is a cheaper default-speed route. Comparing OpenAI Fast overage with metered default pricing produces a nominal 42.8x
price difference (`17.1 x 2.5`), but that is not an apples-to-apples speed-tier comparison.

Metered's dashboard separately announced a new “special-price Codex group” with a 0.2 group multiplier on 2026-07-21,
while the live `gpt-5.6-sol` catalog still displayed the tested `Codex专属` group at 0.8. This appears to be a
routing-price change, not a Fast-mode feature, and should be remeasured after the catalog and usage ledger agree.

## Comparing metered with the $200 Codex subscription

OpenAI does not publish a fixed API-equivalent dollar value for a fully used ChatGPT Pro/Codex subscription. The
effective subsidy depends on:

- how completely the weekly allowance is used;
- the input/output mix;
- caching;
- model and reasoning settings; and
- whether the workload is rate-limited before the subscription month ends.

Let `S` be the subscription's effective API discount multiplier. For example, `S = 27` means the subscription delivered
27 times the compute that $200 would buy at API list prices.

At Metered's measured 5.84% of API pricing:

```text
Metered cost for the same monthly work = $200 × S × 0.0584
```

| Assumed Codex value | API-equivalent monthly use | metered cost for same compute | metered credits | Relative to $200 |
| ------------------- | -------------------------: | ----------------------------: | --------------: | ---------------: |
| 16×                 |                     $3,200 |                    about $187 |     about 2,558 |            0.93× |
| 27×                 |                     $5,400 |                    about $315 |     about 4,317 |            1.58× |
| 47×                 |                     $9,400 |                    about $549 |     about 7,516 |            2.74× |

The 16×, 27×, and 47× values are scenarios derived from anecdotal estimates, not official OpenAI guarantees.

## Break-even point

Metered costs the same $200 when monthly official-API-equivalent use reaches:

```text
$200 / 0.0584 ≈ $3,425
```

Consequently:

- below about **$3,425 API-equivalent use per month**, metered is cheaper than paying $200;
- above that amount, a fully utilized $200 subscription is cheaper per unit of compute;
- metered remains useful as pay-as-you-go overflow after the subscription reaches a weekly limit.

## Practical recommendation

Use the subscription first and metered as overflow for non-sensitive workloads. Use official OpenAI overage when
privacy, supportability, or direct-provider reliability is worth the approximately 17.1x price premium. Determine the
correct Metered top-up from observed overflow:

```text
required metered dollars = overflow API-equivalent dollars × 0.0584
required credits       = required metered dollars / 0.0730
```

Avoid holding a large prepaid balance until route stability, privacy, and longer-term pricing have been observed.

## Sources

- [Metered pricing](https://api.openlux.ai/pricing)
- [Metered API documentation](https://metered.apifox.cn/)
- [OpenAI Codex rate card](https://help.openai.com/en/articles/20001106)
- [OpenAI Codex speed configuration](https://learn.chatgpt.com/docs/agent-configuration/speed)
- [OpenAI credits for flexible Plus/Pro usage](https://help.openai.com/en/articles/12642688-using-credits-for-flexible-usage-in-chatgpt-pluspro)
- [About ChatGPT Pro](https://help.openai.com/en/articles/9793128-about-chatgpt-pro)
