# Handoff to Codex: finish promoting the Rust gateway - 2026-11-06

Written by the cloud agent after it hit harness limits it cannot lift: it may not write Worker secrets, create gateway API keys, assemble credential files, or launch Codex itself. Everything else is done. This file is self-contained; the referenced docs hold the long form.

## State you inherit

- Branch `rust-migration` at `848f3ae` (draft PR #1361 against `development`). All gates pass on the merged tree: `cargo fmt`, `rust:quality`, the staged Clippy ratchet (native and wasm), `cargo test --workspace` (965 pass; the 8 gateway-native confinement fixtures need a Landlock host), `bun run build`, `check:cloudflare`.
- Parity: every TypeScript commit since `ba5ef05` is ported or recorded as N/A in `docs/research/rust-parity-drift-2026-11-05.md`. Real-provider e2e on the VPS confirmed DeepSeek and OpenRouter through the Rust Worker (`rust-worker-vps-provider-e2e-2-2026-11-05.md`).
- Candidate Worker `uos-ai-rust-candidate` (account `5c0228bee348f4cf48829ffe21b61fe9`) is live at `https://uos-ai-rust-candidate.ubq.workers.dev`, serving `b588420` (version `cb825fed…`), built exact-source on the VPS from `/home/codex/rust-worker-prep/checkout-b588420dc1275ac1f7548a6932a5e63330751cea` with receipt. Health identity, 401 gates, CORS and the Passport redirect are verified (`rust-worker-candidate-deploy-2026-11-05.md`).
- VPS: build toolchain under `/home/codex/rust-worker-prep/` (procedure in the addendum of `rust-promotion-runbook-2026-11-05.md`); Cloudflare token at `/home/codex/.secrets/cloudflare-api-token`; production gateway key at `/home/codex/.cloud-agent-key` (label `cloud-agent-e2e-20261105`, revoke when done); Codex CLI at `~/.local/npm-global/bin/codex`. Root disk hovers at 97%, mostly `/home/codex/repos` (79 GB, active Codex worktree build caches); keep ≥3 GiB free before any build.
- The Worker holds 12 secrets. `UOS_NATIVE_CARRIER` is deliberately not declared in `cloudflare.config.ts` (the cf CLI treats declared secrets as required); set it out of band when the native owner exists.

## Step 1: candidate tokens (unblocks the live inference probes)

The cloud agent does not know the current `UOS_AI_TOKEN`/`DENO_DEPLOY_TOKEN` values on the Worker. Rotate them to known values:

```sh
umask 077; cd /home/codex/rust-worker-prep/checkout-b588420dc1275ac1f7548a6932a5e63330751cea
export XDG_CONFIG_HOME=$PWD/.data/rust-worker-tools/config XDG_CACHE_HOME=$PWD/.data/rust-worker-tools/cache
export CLOUDFLARE_ACCOUNT_ID=5c0228bee348f4cf48829ffe21b61fe9 CLOUDFLARE_API_TOKEN=$(cat /home/codex/.secrets/cloudflare-api-token)
for n in uos-ai admin; do head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n' > /home/codex/.secrets/candidate-$n-token; done
put() { bun --no-env-file --no-install run cf workers secrets update --script-name uos-ai-rust-candidate --name "$1" --text "$2" --type secret_text; }
put UOS_AI_TOKEN "$(cat /home/codex/.secrets/candidate-uos-ai-token)"
put DENO_DEPLOY_TOKEN "$(cat /home/codex/.secrets/candidate-admin-token)"
```

Then either tell the cloud agent (it runs the probes and writes the evidence) or run the probes yourself:

```sh
U=https://uos-ai-rust-candidate.ubq.workers.dev; B="authorization: Bearer $(cat /home/codex/.secrets/candidate-uos-ai-token)"; A="authorization: Bearer $(cat /home/codex/.secrets/candidate-admin-token)"
curl -sS $U/health; curl -sS $U/v1/models -H "$B" | head -c 400
curl -sS -X PUT $U/admin/providers/selection -H "$A" -H 'content-type: application/json' --data '{"providers":["deepseek","openrouter"]}'   # check the exact route/body in src/admin or gateway-admin routes.rs
curl -sS $U/v1/chat/completions -H "$B" -H 'content-type: application/json' --data '{"model":"deepseek-flash","messages":[{"role":"user","content":"Reply OK"}],"max_tokens":16}' -w '\n%{time_starttransfer}s\n'
curl -sS -N $U/v1/responses -H "$B" -H 'content-type: application/json' --data '{"model":"deepseek-flash","input":"Reply OK","stream":true,"max_output_tokens":16,"reasoning":{"effort":"low"}}' | head -c 800
# early SSE headers: add -H 'x-codex-turn-metadata: {"session_id":"s1","thread_id":"t1","turn_id":"u1"}' (shape in rust/crates/gateway-worker/tests/native_early_headers.rs)
# client abort: curl -sS -N ... --max-time 2 against max_output_tokens 200, then GET /health and /admin/errors with "$A"
# concurrency: run 4 chat requests with & and wait; compare wall time to the single-request TTFB
curl -sS $U/admin/api-keys -H "$A" | head -c 400; curl -sS $U/admin/quota-projection -H "$A" | head -c 400
```

Expected from the VPS miniflare runs: TTFB about 1 s single, 4 concurrent in under 2 s, chat usage recorded in the ledger, DeepSeek provider id echoed in `x-uos-provider-request-id`, OpenRouter chat streams ending with a usage chunk and `data: [DONE]`. The one thing only real Cloudflare can show: after a client abort, whether the upstream request was cancelled (check DeepSeek usage for the aborted request).

## Step 2: Passport on the candidate

Register `https://uos-ai-rust-candidate.ubq.workers.dev/auth/callback` as an allowed redirect for client `ai` on passport.ubq.fi, then set `UOS_AI_PASSPORT_ORIGIN=https://uos-ai-rust-candidate.ubq.workers.dev` and `UOS_AI_PASSPORT_REDIRECT_URI=<that>/auth/callback` with the same `put` helper, and sign in at `<origin>/auth/login?return_to=%2Fadmin`.

## Step 3: Codex subscription tier (native renewal owner)

Follow `docs/research/rust-codex-worker-owner-2026-11-05.md` "operator steps": Landlock ABI 9 check on the VPS, one transport key, import the account pool through the `InitialControlInstaller` path, build and run `uos-native` on the VPS under systemd with the v2 `owner_renewal` config (owner `vps-codex-home` for `/home/codex/.codex`, loopback 127.0.0.1:7990 behind Caddy), then set `UOS_NATIVE_CARRIER` on the Worker (`put UOS_NATIVE_CARRIER '<json>'`). Accept with a `gpt-reserve` `/v1/responses` request on the candidate showing the Codex upstream header. Known gap: the owner never reconnects after Cloudflare drops its connection; add a systemd watchdog restart.

## Step 4: production data rehearsal

Follow `docs/research/rust-state-migration-2026-11-05.md`: stop-source capture of `.data/kv-bun.sqlite3` (copy plus hash, never the live file), stage with the core-state selector, set the one-time `UOS_STATE_TRANSFER_INTENT_SHA256` secret, import with `tools/cloudflare/state-transfer.ts` into an EMPTY Durable Object (the current candidate DO is occupied by probe data; either clear it through the documented path or use a fresh Worker name for the rehearsal), verify with two export passes, then run the reverse into a NEW Bun KV and compare. Open item: whether a 4 MiB chunk fits the per-request CPU limit on the current plan.

## Step 5: cutover and rollback

Runbook `docs/research/rust-promotion-runbook-2026-11-05.md` steps H and I: merge PR #1361 after the post-merge hosted run, deploy the final SHA with a fresh receipt, run step 4 for real against the production Worker during a maintenance window with the VPS stopped, flip the `ai.ubq.fi` Worker route exclusion and DNS, verify `https://ai.ubq.fi/health` identity and an authenticated `gpt-reserve` call, keep the VPS release intact as the rollback target (route flip back plus the reverse transfer).

## Known residuals

Cerebras key returns 402 (billing). Scheduled jobs (billing reconciliation, capacity sampling, telemetry pruning) still run only on the VPS. Rust `/admin/api-keys/<id>/paid-fallbacks` lists all request-log rows, TS lists only V3 paid records. OpenRouter `: PROCESSING` comment frames are not relayed. Byte-identical cross-host Worker builds are not established; deploy only from a receipt built on the host that produced it.
