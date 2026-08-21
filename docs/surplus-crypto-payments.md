# Paying Surplus Intelligence with Crypto

Snapshot: August 20, 2026.

Surplus Intelligence supports two distinct crypto payment paths. Both use native USDC on Base mainnet, but their
approvals and request flows are not interchangeable.

## Account treasury and API key

Use this path for normal gateway traffic through a Surplus API key.

1. Open [Buy → Funding](https://www.surplusintelligence.ai/buy#funding).
2. Select **Crypto**.
3. Select **Create treasury**. This creates a shared on-chain wallet controlled by the organization's administrators.
   Surplus states that it never holds the wallet key.
4. Transfer native Base USDC to the treasury. If the USDC is on another network, bridge it to Base first.
5. Approve the settlement contract shown by Surplus. Its address is resolved dynamically from `GET /v1/buyer/me` through
   the `settlement_contract` field and must not be hard-coded.
6. Create or use an API key. Each request settles its actual cost from the treasury through `transferFrom()`.

The USDC remains in the treasury until a request settles. Surplus says that it relays gas for approvals and withdrawals,
so the treasury does not need ETH. The allowance can be revoked.

## x402 pay-per-request

Use this path when a client or autonomous agent supports x402 and should pay without a Surplus account or API key.

1. Keep native Base USDC in a signing wallet.
2. Send an OpenAI-compatible request without `Authorization`.
3. Read the `HTTP 402 Payment Required` response. Decode the base64 JSON in `PAYMENT-REQUIRED` and select an entry from
   `accepts[]`.
4. Prefer the `upto` scheme when available. It authorizes a maximum amount but settles only the actual post-response
   cost. The `exact` fallback pre-charges the full estimate.
5. Sign the selected payment requirement and retry the identical request body with a base64-encoded `PAYMENT-SIGNATURE`
   header.
6. On success, read the transaction result from `PAYMENT-RESPONSE`.

The `upto` scheme requires a one-time USDC approval to Permit2 at `0x000000000022D473030F116dDEE9F6B43aC78BA3`, followed
by a separate authorization for each request. This approval is different from the Buy-page SettlementV2 approval.
Neither approval satisfies the other payment path.

## Current production identifiers

- Network: Base mainnet (`eip155:8453`)
- Asset: native Base USDC
- USDC contract: `0x833589fCD6eDb6E08f4c7C32D4f71b54bdA02913`
- x402 Permit2 proxy: `0x4020A4f3b7b90ccA423B9fabCc0CE57C6C240002`
- Payment recipient: resolve it from the current 402 challenge or `GET /x402/info`; do not hard-code it

## Recommendation for this gateway

Use the account treasury and API-key path for sustained gateway traffic. Surplus documents no flat fee on that path,
while x402 adds a flat convenience fee for facilitation and settlement. That flat fee can dominate the cost of
inexpensive Luna or DeepSeek requests.

Use x402 when accountless, API-key-free, per-request settlement is the main requirement. Prefer `upto` so the wallet
pays actual usage rather than the maximum estimate.

## Official references

- [Payment options](https://www.surplusintelligence.ai/docs/payments/index)
- [USDC on-chain payment](https://www.surplusintelligence.ai/docs/payments/usdc-onchain)
- [x402 protocol](https://www.surplusintelligence.ai/docs/payments/x402)
- [Wallet funding](https://www.surplusintelligence.ai/docs/guides/wallet-funding)
- [x402 service overview](https://www.surplusintelligence.ai/x402)
