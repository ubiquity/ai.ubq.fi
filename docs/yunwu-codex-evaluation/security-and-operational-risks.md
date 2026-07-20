# Security and operational risks

Date observed: 2026-07-19

## Credential exposure

During testing, a complete Yunwu bearer token was pasted into a conversation. It must be treated as compromised and
revoked.

The replacement should be:

- stored in an environment variable or secret manager;
- excluded from shell history, screenshots, logs, and source control;
- scoped by balance, expiration, or quota if Yunwu supports those controls; and
- rotated immediately if it appears in a URL or third-party page.

No secret values are included in these reports.

## Third-party key transmission

Clicking Yunwu's console “Chat” action unexpectedly opened:

```text
web.apiplus.org
```

The full API credential was embedded in the resulting URL. The tab was closed immediately.

This creates several leakage channels:

- browser history;
- intermediary and destination server logs;
- analytics and referrer data;
- screenshots or screen recording;
- browser synchronization; and
- copied or shared URLs.

Do not use that console action with a production credential. Rotate the tested credential even if the page was opened
only briefly.

## Prompt and code confidentiality

Yunwu is an intermediary between the client and the upstream model. It can technically observe:

- prompts and system instructions;
- source code and repository context;
- generated responses and reasoning metadata;
- account identifiers and request timing; and
- tools, function arguments, and attached content sent through the API.

The provider's public documentation and marketing claims do not establish an audited no-retention or no-training
guarantee. Treat all submitted material as visible to the intermediary unless a binding agreement states otherwise.

Recommended boundary:

- do not send production secrets, private keys, customer data, credentials, or unreleased proprietary code;
- use redacted or synthetic fixtures for evaluation;
- prefer direct OpenAI or an approved internal gateway for sensitive work;
- isolate the Yunwu credential from other provider credentials.

## Routing and supply risk

The low price may depend on pooled subscriptions, promotional capacity, cross-region routing, or other arrangements that
can change quickly. The exact mechanism was not verified.

Operational risks include:

- abrupt price or multiplier changes;
- unavailable or renamed routing groups;
- fallback to a different route with a different price;
- variable concurrency and rate limits;
- account-pool exhaustion;
- inconsistent model snapshots or system wrappers;
- provider shutdown or balance loss; and
- terms-of-service or payment-fraud exposure elsewhere in the supply chain.

The cited Hacker News discussion describes allegations about the broader Chinese token-resale market. It is useful
context but is not proof of Yunwu's specific sourcing:

- [Hacker News discussion](https://news.ycombinator.com/item?id=48667495)
- [ChinaTalk: How to Buy Cheap Claude Tokens in China](https://www.chinatalk.media/p/how-to-buy-cheap-claude-tokens-in)

## Safer operating model

1. Keep the official Codex subscription as the default route.
2. Use Yunwu only for non-sensitive overflow.
3. Maintain a small prepaid balance rather than a large stored balance.
4. Log route, model, input/output tokens, charge, latency, and failures locally.
5. Alert when the selected group or multiplier differs from the expected Codex-specific route.
6. Rotate keys periodically and after any accidental URL exposure.
7. Never use the exposed key again.

## Evidence boundary

The testing established real billing, latency, protocol, and browser behavior. It did not establish Yunwu's corporate
controls, upstream contracts, data retention policy, physical serving region, or source-account legitimacy.
