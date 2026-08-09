# Marketplace Pivot Handoff Plan

## Context

- Repository: `ai.ubq.fi` (Deno/TypeScript API gateway)
- Goal: transition from admin-managed fixed auth slots to a user marketplace for `auth.json` capacity with per-owner
  metering and user-level API keys.
- Constraint: preserve OpenAI-compatible public request behavior and visible prototype-first delivery.

## 1) Target architecture

- Separate identities:
  - **api_requester**: holder of API key used to send requests.
  - **auth_owner**: user who uploads `auth.json` and sells compute capacity.
- Remove fixed-slot logic in routing (`1/2` slots) and move to dynamic account IDs.
- Keep requester quota policy unchanged at entry, then add seller-side spend accounting on provider dispatch.

## 2) Data model changes (unbounded capacity)

- Add `AuthAccount` records (KV-backed, not array-limited):
  - `id`, `ownerUserId`, `provider`, `encryptedAuthJson`, `status`, `pricing`, `maxConcurrent`, `health`, `enabled`,
    `labels`, `createdAt`, `updatedAt`.
- Add seller ledger: `AuthBalanceLedger`:
  - `ownerUserId`, `currency`, `availableUnits`, `reservedUnits`, `spentUnits`, `updatedAt`.
- Add usage event journal: `AuthUsageEvent`:
  - `requestId`, `authAccountId`, `ownerUserId`, `apiKeyId`, `model`, `estimatedCost`, `actualCost`, `reservedAmount`,
    `settledAmount`, `result`, `ts`.
- Add user-owned API key table (public keys):
  - `id`, `ownerUserId`, `name`, `secretHash`, `scopes`, `rateCaps`, `revoked`, `createdAt`, `lastUsedAt`.

## 3) Public API surfaces

- User-facing key management:
  - `POST /api-keys` create key for authenticated user.
  - `GET /api-keys` list own keys.
  - `POST /api-keys/{id}/revoke` revoke own key.
- Keep admin key endpoints for privileged flows.
- Marketplace auth endpoints:
  - `POST /marketplace/auths` upload `auth.json` + owner config.
  - `GET /marketplace/auths/me` list owned auth accounts.
  - `PATCH /marketplace/auths/{id}` update price/enabled/status metadata.
  - `POST /marketplace/auths/{id}/disable` remove from routing.
- Optional public catalog for discoverability:
  - `GET /marketplace/auths` (non-secret fields only: provider, status, pricing, latency indicators).

## 4) Routing and capacity logic

- Replace all hardcoded two-account assumptions in:
  - `src/provider_capacity.ts`
  - `src/codex_account_routing.ts`
  - `src/codex.ts` (pool max constant and slot-based structures)
- New routing flow:
  1. Load all `AuthAccount` records for provider.
  2. Filter by `enabled`, health, capacity, and owner balance.
  3. Sort by policy: price first, then health, then usage spread/least recently used.
  4. Select account by ID, no fixed upper bound.

## 5) Metering/billing model

- Admission sequence:
  1. Authenticate API key (requester policy).
  2. Select auth account.
  3. Estimate cost and reserve against seller ledger.
  4. Execute request.
  5. Settle on completion with actual cost (`actual - estimated` adjust reserve).
- On failure before execution or mid-failure, release reservation and record failed usage event.
- Insufficient seller balance returns clear “seller capacity exhausted” error path.

## 6) Security and abuse boundaries

- Ownership checks:
  - Users can manage only own auth uploads and own API keys.
  - Admin retains override for moderation and remediation.
- Credential handling:
  - store encrypted `auth.json` blobs only.
  - never log secrets or raw key material.
- Permissions:
  - `marketplace:manage_own_auths`
  - `marketplace:view_own_ledger`
  - `api_keys:manage_own`
- Keep admin-only flows and scopes unchanged for compatibility.

## 7) Migration strategy

- Backward compatibility release window:
  - migrate existing fixed-slot accounts into dynamic `AuthAccount` objects.
  - map old admin-owned slots to `ownerUserId = admin` initially.
- Add temporary dual-read path so pre-migration records still function in case of partial rollout.
- Remove `CODEX_AUTH_POOL_MAX_ACCOUNTS` and any `slot <= 1`/`1 | 2` checks.

## 8) Rollout phases

- Phase 1: storage model + compatibility migration + key issuance endpoints.
- Phase 2: routing rewrite and unlimited auth selection.
- Phase 3: owner ledger reservation + settlement and public ledger views.
- Phase 4: seller marketplace surfaces (listings, status, pricing controls).
- Phase 5: anti-abuse caps, moderation, and balancing policy hardening.

## 9) Acceptance criteria (prototype-level)

- Non-admin user uploads a new `auth.json`.
- User creates own API key and calls request flow successfully.
- System routes across >2 `auth.json` accounts.
- Seller balance decrements on successful usage and rolls back on failed dispatch.
- Request fails with actionable error when seller balance is insufficient.
- Admin legacy flows remain functional during migration.

## 10) Open decisions before implementation

- Cost unit basis: token-based estimate vs output-inclusive estimate.
- Unit precision: integer micro-units or decimal with fixed scale.
- Seller pricing mode: flat rate per request, token-based, or tiered grid.
- Buyer preference model: requester-specified seller preference or policy-only assignment.

## Implementation order suggestion

1. Schema + migration baseline.
2. Public API-key issuance endpoints.
3. Auth-account CRUD and catalog.
4. Routing rewrite and unbounded selection.
5. Ledger reservation/settlement.
6. Admin + moderation/observability improvements.
