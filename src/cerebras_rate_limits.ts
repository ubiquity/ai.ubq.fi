export const CEREBRAS_RATE_LIMIT_HEADERS = [
  "x-ratelimit-limit-requests-minute",
  "x-ratelimit-remaining-requests-minute",
  "x-ratelimit-reset-requests-minute",
  "x-ratelimit-limit-tokens-minute",
  "x-ratelimit-remaining-tokens-minute",
  "x-ratelimit-reset-tokens-minute",
  "x-ratelimit-limit-requests-day",
  "x-ratelimit-remaining-requests-day",
  "x-ratelimit-reset-requests-day",
  "x-ratelimit-limit-tokens-day",
  "x-ratelimit-remaining-tokens-day",
  "x-ratelimit-reset-tokens-day",
] as const;
