// Priced rate-card module for the DeepSeek V4.1 Flash provider waterfall benchmark (m2-cost).
//
// Units and conventions
// ---------------------
// - Every embedded rate is USD per token exactly as published by the provider;
//   `usdPerMillionTokens` renders the same rate per 1M tokens for reporting.
// - `expectedCostMicroUsd` returns micro-USD (1 micro-USD = 1e-6 USD), computed
//   as `tokens x usd_per_token x 1_000_000` and rounded to six decimal places so
//   binary-float dust never leaks into aggregates. Components are rounded
//   independently and the total is their sum and is rounded again, so the split
//   always reconciles with its total.
// - `usage.input_tokens` is the total prompt-token count (cache hits included);
//   `cached_input_tokens` and `cache_write_input_tokens` are subsets of it. The
//   input component therefore prices `input_tokens - cached_input_tokens`,
//   mirroring the gateway's paid settlement (`recordSurplusUsage` in
//   src/paid-fallback/index.ts).
// - When a card carries `cache_write: null` the provider publishes no separate
//   cache-write charge, and those tokens are billed inside the input component
//   under that provider's own published rules (stated per card below). Surplus
//   publishes a cache-write rate and the gateway charges it as an extra amount
//   on top of the miss-input amount; this module mirrors that settlement.
// - This module never guesses: a provider without a bounded published rate has
//   no card and `expectedCostMicroUsd` returns null for it. As of
//   RATE_CARD_VERSION every one of the five benchmark providers has a bounded
//   published rate, recorded with its source URL and retrieval date.
//
// cost-probe.ts re-fetches every source below and self-checks these numbers.

import type { ProviderId } from "./providers.ts";

export const RATE_CARD_VERSION = "2026-10-06.v1";

const MICRO_USD_PER_USD = 1_000_000;

/** Paid-ledger conversion: 1 credit = 1_000_000 microcredits (src/api-keys.ts). */
export const MICROCREDITS_PER_CREDIT = 1_000_000;

export type CostUsage = Readonly<{
  input_tokens: number;
  cached_input_tokens: number;
  cache_write_input_tokens: number;
  output_tokens: number;
}>;

export type CostBreakdown = Readonly<{
  input_micro_usd: number;
  cached_input_micro_usd: number;
  cache_write_micro_usd: number;
  output_micro_usd: number;
  total_micro_usd: number;
}>;

/** USD per token for one pricing schedule. */
export type TokenRate = Readonly<{
  input_cache_miss: number;
  input_cache_hit: number;
  /** null when the provider publishes no separate cache-write rate. */
  cache_write: number | null;
  output: number;
}>;

export type RateSource = Readonly<{
  /** Exact URL the rate was retrieved from. */
  url: string;
  /** Retrieval date, UTC, YYYY-MM-DD. */
  retrieved: string;
  /** Unit conversion and provider-specific billing rule. */
  note: string;
}>;

type FlatRateCard = Readonly<{
  kind: "flat";
  model: string;
  source: RateSource;
  rate: TokenRate;
}>;

type DeepSeekRateCard = Readonly<{
  kind: "deepseek";
  model: string;
  source: RateSource;
  peak: TokenRate;
  off_peak: TokenRate;
}>;

export type ProviderRateCard = FlatRateCard | DeepSeekRateCard;

/** Whole-hour UTC windows on weekdays (Mon-Fri) priced at the DeepSeek peak rate. */
export type HourWindow = Readonly<{ start_hour: number; end_hour: number }>;

export const DEEPSEEK_PEAK_WINDOWS_UTC: readonly HourWindow[] = [
  { start_hour: 1, end_hour: 4 },
  { start_hour: 6, end_hour: 10 },
];

/**
 * Versioned rate card. Each entry carries the source URL and retrieval date it
 * was read from. A missing entry means no bounded published rate exists, and
 * `expectedCostMicroUsd` returns null rather than an invented number.
 */
export const RATE_CARD: Readonly<Partial<Record<ProviderId, ProviderRateCard>>> = {
  surplus: {
    kind: "flat",
    model: "deepseek-v4.1-flash",
    source: {
      url: "https://api.surplusintelligence.ai/v1/models",
      retrieved: "2026-10-06",
      note: "Catalogue strings are USD per token: prompt 0.0000003000, completion 0.0000012000, input_cache_read 0.0000000060, input_cache_write 0.0000003000. The gateway parses them into input/output/cache_read/cache_write prices per token and settles spend_microcredits = USD charge x 1,000,000 (src/paid-fallback/index.ts recordSurplusUsage, src/paid-fallback/ledger-settlement.ts).",
    },
    rate: {
      input_cache_miss: 0.0000003,
      input_cache_hit: 0.000000006,
      cache_write: 0.0000003,
      output: 0.0000012,
    },
  },
  openlux: {
    kind: "flat",
    model: "deepseek-v4.1-flash",
    source: {
      url: "https://api.openlux.ai/api/ratio_config + https://api.openlux.ai/api/status",
      retrieved: "2026-10-06",
      note: "USD per token = ratio / quota_per_unit: model_ratio 0.15, completion_ratio 4, cache_ratio 0.02, quota_per_unit 500000. Input 0.15/500000, cached 0.15x0.02/500000, output 0.15x4/500000. Billed quota is authoritative from /api/log/token rows; the gateway settles spend_microcredits = round(quota x 1,000,000 / quota_per_credit) with quota_per_credit = quota_per_unit.",
    },
    rate: {
      input_cache_miss: 0.0000003,
      input_cache_hit: 0.000000006,
      cache_write: null,
      output: 0.0000012,
    },
  },
  lithos: {
    kind: "flat",
    model: "deepseek-ai/DeepSeek-V4.1-Flash (Base tier)",
    source: {
      url: "https://www.lithosai.com/pricing",
      retrieved: "2026-10-06",
      note: "Published current early-access rates per 1M tokens: input $0.15, cached input $0.003, output $0.60; the struck non-discounted output was $1.20 (2x early-access). Docs (docs.lithosai.com/billing) state cached input is a subset of input and there is no separate cache-write charge. Fast tier (model id suffix -fast): $0.25/$0.005/$1.00; Ultra tier (-ultra/-ultra-chat): $0.35/$0.007/$1.40.",
    },
    rate: {
      input_cache_miss: 0.00000015,
      input_cache_hit: 0.000000003,
      cache_write: null,
      output: 0.0000006,
    },
  },
  deepseek: {
    kind: "deepseek",
    model: "deepseek-flash (serves DeepSeek-V4.1-Flash; alias deepseek-v4-flash)",
    source: {
      url: "https://api-docs.deepseek.com/quick_start/pricing/",
      retrieved: "2026-10-06",
      note: "Official USD per 1M tokens for deepseek-flash. Peak vs off-peak is selected by the request's UTC instant: off-peak is half of peak; peak hours are 01:00-04:00 and 06:00-10:00 UTC Monday-Friday, excluding Chinese public holidays, and all other hours (including full weekends and Chinese public holidays) are off-peak. Cache writes are not charged separately; cache-miss input covers them.",
    },
    peak: {
      input_cache_miss: 0.0000003,
      input_cache_hit: 0.000000006,
      cache_write: null,
      output: 0.0000012,
    },
    off_peak: {
      input_cache_miss: 0.00000015,
      input_cache_hit: 0.000000003,
      cache_write: null,
      output: 0.0000006,
    },
  },
  openrouter: {
    kind: "flat",
    model: "deepseek/deepseek-v4.1-flash",
    source: {
      url: "https://openrouter.ai/api/v1/models",
      retrieved: "2026-10-06",
      note: "Catalogue strings are USD per token: prompt 0.0000003, completion 0.0000012, input_cache_read 0.000000006 (no input_cache_write is published for this model). Authoritative observed cost per generation is the generation endpoint total_cost in USD (https://openrouter.ai/api/v1/generation?id=<id>); micro-USD = total_cost x 1,000,000.",
    },
    rate: {
      input_cache_miss: 0.0000003,
      input_cache_hit: 0.000000006,
      cache_write: null,
      output: 0.0000012,
    },
  },
};

/**
 * DeepSeek pricing window for a request instant. Window boundaries are exact
 * whole UTC hours; weekends are off-peak. Chinese public holidays are also
 * off-peak per the source but are not excluded here because no bounded
 * machine-readable calendar is embedded (documented caveat; expected peak cost
 * may overstate on those dates).
 */
export const deepSeekPricingWindow = (atMs: number): "peak" | "off_peak" => {
  const at = new Date(atMs);
  const day = at.getUTCDay();
  if (day === 0 || day === 6) return "off_peak";
  const hour = at.getUTCHours();
  for (const window of DEEPSEEK_PEAK_WINDOWS_UTC) {
    if (hour >= window.start_hour && hour < window.end_hour) return "peak";
  }
  return "off_peak";
};

const roundToMicroUsdPrecision = (value: number): number => Math.round(value * 1e6) / 1e6;

const assertUsage = (usage: CostUsage): void => {
  for (const [field, value] of Object.entries(usage)) {
    if (!Number.isFinite(value) || value < 0) {
      throw new RangeError(`usage.${field} must be a finite non-negative number, received ${String(value)}`);
    }
  }
  if (usage.cached_input_tokens > usage.input_tokens) {
    throw new RangeError("usage.cached_input_tokens must not exceed usage.input_tokens");
  }
};

const costFromRate = (rate: TokenRate, usage: CostUsage): CostBreakdown => {
  assertUsage(usage);
  const uncachedInputTokens = usage.input_tokens - usage.cached_input_tokens;
  const input = roundToMicroUsdPrecision(uncachedInputTokens * rate.input_cache_miss * MICRO_USD_PER_USD);
  const cached = roundToMicroUsdPrecision(usage.cached_input_tokens * rate.input_cache_hit * MICRO_USD_PER_USD);
  const cacheWrite = rate.cache_write === null ? 0 : roundToMicroUsdPrecision(usage.cache_write_input_tokens * rate.cache_write * MICRO_USD_PER_USD);
  const output = roundToMicroUsdPrecision(usage.output_tokens * rate.output * MICRO_USD_PER_USD);
  return {
    input_micro_usd: input,
    cached_input_micro_usd: cached,
    cache_write_micro_usd: cacheWrite,
    output_micro_usd: output,
    total_micro_usd: roundToMicroUsdPrecision(input + cached + cacheWrite + output),
  };
};

/**
 * Expected cost in micro-USD for one request against one provider, or null when
 * that provider has no bounded published rate in the embedded card.
 */
export const expectedCostMicroUsd = (providerId: ProviderId, usage: CostUsage, atMs: number): CostBreakdown | null => {
  const card = RATE_CARD[providerId];
  if (!card) return null;
  if (card.kind === "deepseek") {
    return costFromRate(deepSeekPricingWindow(atMs) === "peak" ? card.peak : card.off_peak, usage);
  }
  return costFromRate(card.rate, usage);
};

export type ObservedCostDescriptor = Readonly<{
  provider: ProviderId;
  kind: "provider_endpoint" | "gateway_ledger" | "rate_card_projection";
  /** Exact endpoint or KV namespace; null when no API-reachable billing surface exists. */
  surface: string | null;
  /** Fields carrying billed cost, in the surface's own naming. */
  fields: readonly string[];
  /** Unit conversion into micro-USD, stated exactly; null when the surface is not itself a cost. */
  conversion: string | null;
  /** Whether one inference attempt can be reconciled individually. */
  per_request: boolean;
  notes: string;
}>;

const OBSERVED_COST_SOURCES: Readonly<Record<ProviderId, ObservedCostDescriptor>> = {
  surplus: {
    provider: "surplus",
    kind: "gateway_ledger",
    surface:
      'Gateway paid-fallback ledger: settled request rows and usage rollups under KV namespace ["uos_ai","paid_fallback","v3"] surfaced by GET /admin/providers/quota-projection (models[].usage[].spend_microcredits/request_count/tokens)',
    fields: ["spend_microcredits", "quota_sum", "input_tokens", "cached_input_tokens", "output_tokens", "request_count"],
    conversion:
      "recordSurplusUsage charges chargedCredits = (input-cached)x0.0000003 + cachedx0.000000006 + cache_writex0.0000003 + outputx0.0000012 in USD; settlement spend_microcredits = chargedCredits x 1,000,000, so 1 microcredit = 1 micro-USD on this path.",
    per_request: true,
    notes:
      "Surplus returns usage in the terminal Responses event and shares the Metered-ledger settlement; the rollup row's provider label is \"surplus\". Surplus's own /v1/models exposes rates but no settled spend.",
  },
  openlux: {
    provider: "openlux",
    kind: "provider_endpoint",
    surface:
      "GET https://api.openlux.ai/api/log/token?key=<METERED_API_KEY>&page=<n>&page_size=100 (per-request rows) and GET https://api.openlux.ai/api/status (quota_per_unit)",
    fields: [
      "data[].quota",
      "data[].prompt_tokens",
      "data[].cached_prompt_tokens",
      "data[].completion_tokens",
      "data[].model_name",
      "data[].request_id",
      "data[].created_at",
    ],
    conversion:
      "USD = quota / quota_per_unit (500000). Gateway settlement: spend_microcredits = round(quota x 1,000,000 / quota_per_credit), quota_per_credit = quota_per_unit; net 1 quota = 2 micro-USD and 1 credit = 1 USD. Expected-rate math: quota = tokens x model_ratio (0.15), x completion_ratio (4) for output, x cache_ratio (0.02) for cached input.",
    per_request: true,
    notes:
      "Token-log rows are correlated by request_id; the gateway backfill/settlement reads them in src/paid-fallback/ledger-backfill.ts and ledger-settlement.ts. Settled spend also lands in the usage rollups surfaced by /admin/providers/quota-projection.",
  },
  lithos: {
    provider: "lithos",
    kind: "rate_card_projection",
    surface: null,
    fields: [],
    conversion:
      "Three rates per 1M tokens (input, cached input, output); cached input is a subset of input: USD = (prompt_tokens - cached_tokens)xinput_rate/1e6 + cached_tokensxcached_rate/1e6 + completion_tokensxoutput_rate/1e6. No separate cache-write charge is documented.",
    per_request: false,
    notes:
      "No API-reachable per-request billing surface is documented. Live spend by day/model/API key is exposed only in the Lithos console (https://console.lithosai.cloud/billing and /analytics), so observed cost cannot be reconciled per request from this gateway. GET /v1/models and GET /v1/models/deepseek-ai/DeepSeek-V4.1-Flash return no pricing fields.",
  },
  deepseek: {
    provider: "deepseek",
    kind: "rate_card_projection",
    surface: null,
    fields: [],
    conversion:
      "Official rate card per 1M tokens selected by the request's UTC instant: peak 01:00-04:00 and 06:00-10:00 UTC Mon-Fri; off-peak is half the peak rate at all other hours. USD = (prompt_tokens - cached_tokens)xmiss_rate/1e6 + cached_tokensxhit_rate/1e6 + completion_tokensxoutput_rate/1e6.",
    per_request: false,
    notes:
      "The official API exposes no per-request billing readback for API keys; observed cost is the rate-card projection, cross-checked only by account-level invoices. Chinese public holidays are off-peak per the source but are not machine-excluded here.",
  },
  openrouter: {
    provider: "openrouter",
    kind: "provider_endpoint",
    surface: "GET https://openrouter.ai/api/v1/generation?id=<generation id>",
    fields: ["total_cost", "cache_discount", "tokens_prompt", "tokens_completion", "native_tokens_cached", "usage"],
    conversion:
      "total_cost is USD (JSON number); micro-USD = total_cost x 1,000,000. cache_discount (USD|null) is already reflected inside total_cost. Catalogue prompt/completion/input_cache_read prices are USD per token.",
    per_request: true,
    notes:
      "Authoritative observed cost per generation; the id is the upstream response id the gateway surfaces for x-uos-upstream=openrouter rows. Compare it against the catalogue-derived expected cost; a divergence flags marketplace surcharges or catalogue drift.",
  },
};

export const observedCostSource = (providerId: ProviderId): ObservedCostDescriptor => OBSERVED_COST_SOURCES[providerId];
