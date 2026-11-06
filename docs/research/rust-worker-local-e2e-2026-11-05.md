# Rust Worker local build and workerd e2e evidence (2026-11-05)

This is a go/no-go input for promotion of the Rust gateway Worker. It records one real `worker-build --release --locked` artifact and one scripted local run of that exact artifact under the pinned Miniflare/workerd. No gateway source was changed. No credential, real provider, Cloudflare account or deployment was used.

## Identity

| Item                 | Value                                                                         |
| -------------------- | ----------------------------------------------------------------------------- |
| Source commit        | `66c04d3680c0ce3ef016491687865cdb2e39249e` (tip of `rust-migration`)          |
| Source tree          | `7000a1ab5c5ca65250ad1147e1a94814978fad96`                                    |
| Checkout state       | clean (`git status --porcelain --untracked-files=all` empty before build)     |
| Host                 | Linux x86_64, 4 vCPU, 15 GiB RAM                                              |
| Rust                 | `rustc 1.98.1 (48a229cea 2026-09-01)`, target `wasm32-unknown-unknown`        |
| worker-build         | `0.8.7` (`cargo install worker-build --version 0.8.7 --locked`)               |
| wasm-bindgen-cli     | `0.2.129` (`cargo install wasm-bindgen-cli --version 0.2.129 --locked`)       |
| Miniflare / workerd  | `miniflare@5.20261012.1-beta` / `workerd 1.20261012.1` (`workerd 2026-10-12`) |
| wrangler (installed) | `4.151.1` (installed with Miniflare; not used to run, see below)              |
| Node / Bun           | `v22.23.4` / `1.4.0`                                                          |

The tool versions match the pins: `rust/rust-toolchain.toml` (1.98.1), `rust/Cargo.toml` and `rust/Cargo.lock` (`worker =0.8.7`, `wasm-bindgen =0.2.129`), the contract's "Build and packaging" section (`worker-build 0.8.7`, `wasm-bindgen-cli 0.2.129`), and the root `package.json` dev pins (`miniflare 5.20261012.1-beta`, `wrangler 4.151.1`). The repository's Node pin for the Worker test runtime is 24.21.0. This run used the host's Node 22.23.4, which meets Miniflare's `>=22.0.0` engine requirement.

## Step 1: Worker artifact build

Commands:

```sh
git checkout -q -B rust-e2e-local 66c04d3
CARGO_INCREMENTAL=0 cargo +1.98.1 install worker-build --version 0.8.7 --locked      # 2m26s
CARGO_INCREMENTAL=0 cargo +1.98.1 install wasm-bindgen-cli --version 0.2.129 --locked # 1m20s
cd rust/crates/gateway-worker
CARGO_INCREMENTAL=0 worker-build --release --locked
```

Result: exit 0. Wall time was 3m25.5s (user 7m26s). The cargo `release` profile finished in 2m40s. The build ran from 2026-11-05T15:55:58Z to 15:59:23Z. `build.rs` accepted the clean tree.

| Artifact (`rust/crates/gateway-worker/`) | Bytes      | gzip -9 bytes | SHA-256                                                            |
| ---------------------------------------- | ---------- | ------------- | ------------------------------------------------------------------ |
| `build/index_bg.wasm`                    | 12,620,130 | 4,111,972     | `f8ce7e3ec15b40eda915d7089d93906d9c6418765475f0a831a5d21af5078e9c` |
| `build/index.js`                         | 34,847     | 8,762         | `8091b607c4fbf57d402a75a29740057506f0a8e1dd4f8ce672a3f7da9ebb9b34` |
| `build/worker/shim.mjs`                  | 164        |               | `ac99177861405a6046923cbacabbd6adfa1853a56d0c679b9f5e09097c2f4728` |
| `build/package.json`                     | 207        |               | `a9024c7f6a3469e6f97f4682199362cc6045a409ed1e38fca6a61c3fc0ac24f5` |
| `entrypoint.mjs` (committed)             |            |               | `5ae810969f749a786001d4b2843f39257e80399ca9187ad4ee1ed8e3551ffc10` |

As the contract states, no `index.d.ts` was produced, even though `build/package.json` lists one.

Build-time downloads: `worker-build 0.8.7` did not use the cargo-installed `wasm-bindgen` on `PATH`. It downloaded its own tools into `~/.cache/worker-build/`:

| Tool                        | SHA-256                                                            |
| --------------------------- | ------------------------------------------------------------------ |
| wasm-bindgen 0.2.129 (musl) | `fb59da714982a04273e9e4b09a85e523e5dd04de35b1a4a8b1641785ee7a71cf` |
| wasm-opt 132                | `1014958e6f20d412f1542320b43970214b0fb1ed780595e8f7c0d8761ed53725` |
| esbuild 0.28.2              | `e1698a3d5c6c0798fee4fd3b5cc816651f460c63d390a7a26ea4beb0b1884100` |

These are build-host downloads, not application-startup installs. However, a prepared offline build host must pre-seed this cache, or the build is not hermetic.

Size note: the gzip WASM is 4.11 MB. Cloudflare publishes compressed Worker size limits of 3 MB on the Free plan and 10 MB on paid plans; those limits were not re-verified for this run. By them, this artifact exceeds the Free limit and fits the paid one. Cloudflare's upload-time compression accounting was not measured here.

## Step 2: Local runtime mechanism

The repository's own workerd mechanism (`scripts/prepare-worker-test-runtime.ts`, `tests/native-authority-workerd.test.ts`) only accepts Linux ARM64. It also only exercises the TypeScript authority fixture, not the Rust Worker. This host is x86_64. So the run used the same pinned `miniflare` package programmatically, the way that fixture does (`convertV4MiniflareOptions`). The packages were installed into a scratch directory with `bun install`. Install and proxy access worked.

`wrangler dev` was not used. Its outbound fetches would leave the process. Miniflare's `outboundService` hook intercepts every Worker subrequest before any I/O, and the Rust Worker hard-codes provider hosts with no base-URL override binding. So `outboundService` is the only way here to keep real providers unreachable while still having a fixture upstream.

Configuration, mirroring `cloudflare.config.ts` and `wrangler.config.ts`:

- Main module `entrypoint.mjs`, plus `build/index.js` (ESModule) and `build/index_bg.wasm` (CompiledWasm, equivalent to the `**/*.wasm` CompiledWasm rule). V4 `modulesRules` cannot be converted to Miniflare 5, so the three modules are listed explicitly.
- `compatibilityDate: "2026-10-18"`, flags `enable_request_signal`, `request_signal_passthrough`.
- Durable Object `AUTHORITY` → class `GatewayAuthority` with `useSQLite: true`, persisted under `resourcePersistencePath`.
- `ASSETS` from `./static` with `invoke_user_worker_ahead_of_assets: true` (`runWorkerFirst: true`).
- Fake secrets for every binding in `cloudflare.config.ts`: `UOS_AI_TOKEN`, `DEEPSEEK_API_KEY`, `CEREBRAS_API_KEY`, `LITHOSAI_API_KEY`, `OPENROUTER_API_KEY`, `SURPLUS_API_KEY`, `METERED_API_KEY`, `CLAUDE_API_KEY`.
- Profile `exact` uses only those bindings.
- Profile `extended` adds local-only fake bindings that `cloudflare.config.ts` does not declare: `DENO_DEPLOY_TOKEN` (the admin token source in `gateway-auth/src/bootstrap.rs`), and `UOS_AI_PASSPORT_CLIENT_ID`, `UOS_AI_PASSPORT_ORIGIN=https://rust-candidate.ai.ubq.fi`, `UOS_AI_PASSPORT_REDIRECT_URI=https://rust-candidate.ai.ubq.fi/auth/callback`. Admin, Passport, concurrency and restart cases need them.
- `InitialControlInstaller` was not exercised.

Outbound modes, set per test by the harness:

- `deny`: the hook throws before any I/O. Miniflare turns that into an upstream HTTP 500 `text/plain` response, not a network error (`throw-semantics.mjs`). The Worker therefore sees "upstream 500".
- `fixture`: synthetic OpenAI-shaped JSON/SSE. It is never forwarded.
- `slow`: one SSE chunk, then the stream stays open for 8 s.
- `transport-failure.mjs` uses a Miniflare `node` outbound handler that either destroys the socket with no response (`reset`), or sends 200 with one chunk and then destroys it (`midbody`).

Across all runs, the only outbound origins were `api.surplusintelligence.ai`, `api.openlux.ai`, `api.lithosai.cloud`, `openrouter.ai`, `api.anthropic.com`, `api.cerebras.ai` and `api.deepseek.com`. Every one was intercepted. There were 27 attempts in the main run (`run3/outbound.json`).

## Step 3: Commands and transcripts

Scratch directory: `/tmp/claude-0/-home-user-ai-ubq-fi/568eeac2-146f-5e95-b060-7dccdfe42e3d/scratchpad/e2e/` (not committed).

```sh
cd <scratch>/e2e/tools && bun install            # miniflare@5.20261012.1-beta, wrangler@4.151.1
cd <scratch>/e2e
EXPECT_SHA=66c04d3680c0ce3ef016491687865cdb2e39249e node run-e2e.mjs <repo> <scratch>/e2e/run3     # exit 0
node transport-failure.mjs <repo> <scratch>/e2e/run3-transport                                    # exit 0
node harness-control.mjs        # harness cancellation control, fetcher outbound
node harness-control-node.mjs   # harness cancellation control, node-handler outbound
node throw-semantics.mjs        # what a throwing outboundService looks like to a Worker
```

| File                                             | SHA-256                                                            |
| ------------------------------------------------ | ------------------------------------------------------------------ |
| `run-e2e.mjs`                                    | `2d3d4eb97ad52399c17ae016dd8b472120502291ea598f5dacb20860450b006c` |
| `transport-failure.mjs`                          | `5d8e370a686e5fcf8e651f77d71a43e7351ac669294746c82f8dcb408a067456` |
| `harness-control.mjs`                            | `3979b1f143af55258d9895b7d29a737a6481ada8b7d5eabf84a7e725a96a057a` |
| `harness-control-node.mjs`                       | `9c8b17257d203739bd91722553dbb7a7eb8ec6d88fbcb1c38174d1292235664c` |
| `run3/transcript.json` (status, headers, bodies) | `a28b339a7dbcb43965a33ac6fc746220e454b195dcefabee6fdc49e99e80344d` |
| `run3/results.json`                              | `6dad5a27948ce399f364dd939510376dbae04f0b6e9bbcbc218b743ed3a7d06b` |
| `run3/outbound.json`                             | `5654daee87f9f1c042171fa90cdad520cecf4167a4f4a0e466935d337029c9c5` |
| `run3-transport/transport-failure.json`          | `a2d56efec9296282645442fff0f5af8eb2d1a37d6dbd42bca8b532df5fc7a4ba` |

Earlier iterations `run1` and `run2` used the same artifact. They were superseded only by harness changes: an explicit module list, the slow-upstream case and the race case. `run3` is the evidence run.

## Results

Results are PASS, FAIL, BLOCKED or OBSERVED. OBSERVED means context with no expected value asserted.

| #   | Case                                                                                                  | Result         | Observed evidence                                                                                                                                                                                                                                                                                                                  |
| --- | ----------------------------------------------------------------------------------------------------- | -------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | `GET /health`                                                                                         | PASS           | 200 `{"status":"available","release":{"git_sha":"66c04d36…249e","deployment_id":"cloudflare-66c04d36…249e"}}`, headers `x-uos-git-sha` and `x-uos-deployment-id` carry the same full values                                                                                                                                        |
| 2   | CORS preflight `OPTIONS /v1/responses`                                                                | PASS           | 204, `access-control-allow-origin: *`, methods `GET,POST,PUT,PATCH,DELETE,HEAD,OPTIONS`, headers list including `Authorization,Content-Type,Idempotency-Key,If-None-Match,OpenAI-Beta,…`                                                                                                                                           |
| 3   | `GET /v1/models` unauthenticated                                                                      | PASS           | 401 `{"error":{"message":"Unauthorized","type":"invalid_request_error","code":"invalid_api_key"}}`                                                                                                                                                                                                                                 |
| 4   | `GET /v1/models` with fake `UOS_AI_TOKEN`                                                             | PASS           | 200 `{"object":"list","data":[15 models]}`, first `{"id":"gpt-oss-120b","object":"model","created":0,"owned_by":"cerebras"}`, 0 outbound calls                                                                                                                                                                                     |
| 5   | `GET /v1/models?client_version=0.80.0`                                                                | PASS           | 200 `{"models":[14 entries]}` (Codex-native shape: `slug`, `display_name`, `supported_reasoning_levels` …). 7 discovery subrequests were made and all were denied (upstream 500)                                                                                                                                                   |
| 6   | `GET /auth/login?return_to=%2Fadmin`, exact profile (no Passport binding)                             | PASS           | 404 `passport_not_configured`. This is the behaviour of the binding set in `cloudflare.config.ts` as committed                                                                                                                                                                                                                     |
| 7   | `/admin/api-keys` without session (exact)                                                             | PASS           | 401 `invalid_api_key`                                                                                                                                                                                                                                                                                                              |
| 8   | `GET /admin` page without session                                                                     | PASS           | 200 `text/html; charset=utf-8`, 55,659 bytes, CSP `default-src 'self'; …`. The page is a static asset (sign-in is client-side) with no redirect                                                                                                                                                                                    |
| 9   | `/admin/api-keys` without session (extended)                                                          | PASS           | 401 `invalid_api_key`                                                                                                                                                                                                                                                                                                              |
| 10  | `/admin/api-keys` with client (non-admin) token                                                       | PASS           | 401 `invalid_api_key`                                                                                                                                                                                                                                                                                                              |
| 11  | Passport login redirect (extended, fake client)                                                       | PASS           | 302 to `https://passport.ubq.fi/authorize?client_id=…&audience=ai&origin=…&redirect_uri=…&state=<43 base64url>.admin&code_challenge=<…>&code_challenge_method=S256`. Sets `__Host-uos_ai_passport_state=…; Path=/; Max-Age=600; HttpOnly; Secure; SameSite=Lax` and `__Host-uos_ai_passport_verifier=…` with the same attributes   |
| 12  | `/auth/callback` without state cookie                                                                 | PASS           | 401 `invalid_api_key`, and both transient cookies are cleared (`Max-Age=0`)                                                                                                                                                                                                                                                        |
| 13  | `POST /v1/responses` non-stream, `gpt-oss-120b`, upstream 500                                         | PASS           | 500 `{"error":{"message":"Cerebras upstream returned an error.","type":"server_error","code":"cerebras_upstream_error"}}`. The upstream body was not forwarded                                                                                                                                                                     |
| 14  | `POST /v1/responses` stream, `gpt-oss-120b`, upstream 500                                             | PASS           | Same 500 JSON envelope, with no SSE started (the Cerebras route is buffered)                                                                                                                                                                                                                                                       |
| 15  | `POST /v1/responses` unauthenticated                                                                  | PASS           | 401 `invalid_api_key`                                                                                                                                                                                                                                                                                                              |
| 16  | `POST /v1/chat/completions`, upstream 500                                                             | PASS           | 500 `cerebras_upstream_error`                                                                                                                                                                                                                                                                                                      |
| 17  | `POST /v1/responses` non-stream, fixture upstream                                                     | PASS           | 200 `{"object":"response","status":"completed","output":[{"type":"message",…"text":"fixture says hi"}],"usage":{…}}`                                                                                                                                                                                                               |
| 18  | `POST /v1/responses` stream, fixture upstream                                                         | PASS           | 200 `text/event-stream`: `response.created`, `response.in_progress`, … `response.completed`. The upstream Cerebras call was `stream=false`, so this is a buffered replay                                                                                                                                                           |
| 19  | `POST /v1/chat/completions`, fixture upstream                                                         | PASS           | 200 `{"object":"chat.completion",…"content":"fixture says hi"…}`                                                                                                                                                                                                                                                                   |
| 20  | Client abort after first SSE frame (buffered route)                                                   | PASS           | Reader got `AbortError`. Follow-up `/health` and `/v1/models` returned 200 within 57 ms                                                                                                                                                                                                                                            |
| 21  | Client abort mid-stream, slow streaming upstream, `/v1/chat/completions` `deepseek-flash`             | PASS (no hang) | First chunk at 79 ms, aborted at 79 ms, `/health` 200 in 11 ms afterwards. Upstream showed `aborted:false`, ran to completion after 8 s; see row 21b                                                                                                                                                                               |
| 22  | Same, `/v1/responses` stream `deepseek-flash`                                                         | PASS (no hang) | `response.created` at 165 ms, aborted, `/health` 200 in 7 ms. Upstream `aborted:false`, ran to completion                                                                                                                                                                                                                          |
| 21b | Upstream cancellation propagation after client abort                                                  | BLOCKED        | Not provable with this harness. A minimal JS control Worker that explicitly aborts its upstream on `request.signal`, or pipes and cancels, showed the same `aborted:false`/`completed:true` with both fetcher and node-handler outbound hooks. Its `request.signal` abort marker subrequest never arrived (`harness-control*.log`) |
| 23  | `POST /v1/decisions` `not json`                                                                       | PASS           | 400 `{"error":{"message":"Invalid JSON body","type":"invalid_request_error","code":"invalid_request_error"}}`                                                                                                                                                                                                                      |
| 24  | `POST /v1/decisions` `{}`                                                                             | PASS           | 400 `model must be a string`, `code":"decisions_request_invalid","param":"model"`                                                                                                                                                                                                                                                  |
| 25  | `POST /v1/decisions` missing `questions`                                                              | PASS           | 400 `questions must be an object`, `param":"questions"`                                                                                                                                                                                                                                                                            |
| 26  | `POST /v1/decisions` unknown key                                                                      | PASS           | 400 `Unsupported key: bogus`, `decisions_request_invalid`                                                                                                                                                                                                                                                                          |
| 27  | `POST /v1/decisions` bad question type                                                                | PASS           | 400 `questions.q.type must be noul, choice, score, or box`                                                                                                                                                                                                                                                                         |
| 28  | `POST /v1/decisions` unauthenticated                                                                  | PASS           | 401 `invalid_api_key`                                                                                                                                                                                                                                                                                                              |
| 29  | `POST /v1/decisions` valid body, provider enabled                                                     | OBSERVED       | 400 `The requested model is not served by the OpenRouter Decisions API` (`param":"model"`). The OpenRouter catalog fixture returned 404, so no model resolved and no Decisions dispatch was made                                                                                                                                   |
| 30  | `GET /admin/providers/selection` (fake admin token)                                                   | PASS           | 200 provider picker (`codex` "unavailable", `configured:false`, …)                                                                                                                                                                                                                                                                 |
| 31  | `POST /admin/providers/selection {"provider_ids":["cerebras"]}`                                       | PASS           | 200 `{"stored":true,"provider_ids":["cerebras"],"updated_at_ms":…}`                                                                                                                                                                                                                                                                |
| 32  | `POST /v1/decisions` provider-disabled path                                                           | PASS           | 503 `{"error":{"message":"Decisions provider is disabled by the provider selection","type":"invalid_request_error","code":"decisions_provider_disabled"}}`                                                                                                                                                                         |
| 33  | 8 concurrent `POST /admin/api-keys` (Durable Object authority)                                        | PASS           | 8× 200, 8 distinct ids, 8 keys listed afterwards, 232 ms total                                                                                                                                                                                                                                                                     |
| 34  | 2 concurrent conflicting creates with the same token                                                  | PASS           | One 200 and one 409 `API key already exists`. Exactly one key was persisted                                                                                                                                                                                                                                                        |
| 35  | 2 concurrent authenticated `/v1/models`                                                               | PASS           | 200, 200                                                                                                                                                                                                                                                                                                                           |
| 36  | Created API key authenticates (before restart)                                                        | PASS           | `/v1/models` 200 with the generated `u_…` key                                                                                                                                                                                                                                                                                      |
| 37  | workerd restart (`dispose()` and a new Miniflare on the same persistence directory): API keys persist | PASS           | 8 `e2e-concurrent-*` keys listed after restart. The DO SQLite file is `do/uos-ai-rust-candidate-GatewayAuthority/<id>.sqlite` (WAL 1.9 MB)                                                                                                                                                                                         |
| 38  | Created API key authenticates after restart                                                           | PASS           | 200                                                                                                                                                                                                                                                                                                                                |
| 39  | Provider selection persists across restart                                                            | PASS           | Selection read 200. Decisions still 503 `decisions_provider_disabled`                                                                                                                                                                                                                                                              |
| 40  | `/health` after restart                                                                               | PASS           | 200 with the full SHA                                                                                                                                                                                                                                                                                                              |
| T1  | Socket reset with no response: `chat/completions` `gpt-oss-120b`                                      | PASS           | 502 `{"error":{"message":"The upstream transport failed.","type":"server_error","code":"upstream_transport_error"}}`                                                                                                                                                                                                               |
| T2  | Socket reset: `responses` stream `gpt-oss-120b`                                                       | PASS           | 502 `upstream_transport_error` (JSON, no SSE started)                                                                                                                                                                                                                                                                              |
| T3  | Socket reset: `chat/completions` stream `deepseek-flash`                                              | PASS           | 502 `upstream_transport_error`                                                                                                                                                                                                                                                                                                     |
| T4  | Socket reset: `responses` non-stream `deepseek-flash`                                                 | PASS           | 502 `upstream_transport_error`                                                                                                                                                                                                                                                                                                     |
| T5  | Mid-body reset: `chat/completions` stream `deepseek-flash`                                            | PASS           | 200 SSE: the partial chunk, then `data: {"error":{"code":"deepseek_upstream_stream_error","message":"Upstream Chat Completions stream failed."}}`                                                                                                                                                                                  |
| T6  | Mid-body reset: `responses` stream `deepseek-flash`                                                   | PASS           | 200 SSE: `response.created`, `response.in_progress`, `response.output_item.added`, `response.content_part.added`, `response.output_text.delta`, `response.failed` (`deepseek_upstream_stream_error`). `/health` 200 afterwards                                                                                                     |

Totals: 47 rows. 45 PASS (rows 21 and 22 prove only "no hang"), 1 BLOCKED (21b), 1 OBSERVED (29), 0 FAIL.

## Findings for promotion review

1. The release artifact builds reproducibly from the clean pinned tree. It boots under workerd 1.20261012.1 with the committed `entrypoint.mjs`, the SQLite Durable Object and the static assets. The exact release identity appears in the body and headers.
2. Auth gates, the Passport PKCE/state cookie shape, admin authorization, Decisions validation and its provider-disabled refusal, upstream-500/transport/mid-stream failure envelopes, Durable Object write serialization and restart persistence all behaved as expected locally.
3. As committed, `cloudflare.config.ts` declares no Passport bindings and no admin-token binding. A deployment from it answers `/auth/login` with 404 `passport_not_configured`, and has no bearer admin. Signed-in admin acceptance needs those bindings, or passkeys, to be provisioned separately.
4. Upstream cancellation after a client disconnect is unproven (row 21b). The local stack does not propagate it even for a control Worker. This needs evidence on the real Cloudflare runtime against an observable upstream.
5. `worker-build` downloads wasm-bindgen, wasm-opt and esbuild at build time unless `~/.cache/worker-build` is pre-seeded. Record or pre-seed them on the VPS build host.
6. The compressed WASM is 4.11 MB. By Cloudflare's published limits (not re-verified here) it needs a paid Workers plan.

## What this local run does NOT prove

- Real provider behaviour: Codex subscription, Surplus, OpenLux, Cerebras, DeepSeek, Lithos, OpenRouter, Claude and Anthropic wire compatibility, quota/capacity signals, the waterfall advancing on authoritative exhaustion, or real reasoning-tier catalogs. All upstreams were synthetic or denied, and the Codex pool was empty.
- Paid accounting and settlement correctness, or exactly-once settlement under cancellation. These were not inspected.
- Upstream abort propagation on client disconnect (row 21b).
- Cloudflare's production runtime: isolates, Durable Object placement and real storage, `request_signal_passthrough` semantics, CPU and memory limits, upload size accounting, the assets router, observability, `workers.dev` and routes.
- The `cf/config` and wrangler translation of `cloudflare.config.ts`/`wrangler.config.ts`, and `wrangler deploy` bundling. Miniflare was configured by hand to mirror them.
- Production or migrated data, the KV-to-Durable-Object migration, `InitialControlInstaller`, native-owner and supervisor sidebands, WebSocket/live transports, embeddings, images and compaction.
- Real Passport, passkey and GitHub sign-in completion, or a signed-in admin UI.
- DNS, Cloudflare proxy and Worker-exclusion routing, the public `https://ai.ubq.fi/health`, or an authenticated live inference receipt.
- The other contract gates (`cargo test`, `clippy`, `fmt`, native/WASM `cargo check`), CI, and ARM64 or macOS hosts.

## Binding follow-up (2026-11-05)

`cloudflare.config.ts` now also declares `DENO_DEPLOY_TOKEN` (the admin token source) and `UOS_AI_PASSPORT_CLIENT_ID`, `UOS_AI_PASSPORT_ORIGIN` and `UOS_AI_PASSPORT_REDIRECT_URI` as `bindings.secret()`, with no values. The pinned `cf` CLI treats every declared secret as required. Set all twelve with `cf secret` or `wrangler secret put` (or a `--secrets-file` on first deploy) before deploying, or the deploy refuses. `UOS_AI_PASSPORT_AUDIENCE` stays undeclared and defaults to `ai`. The Worker can also read other names that stay undeclared because declaring them would make them required: `UOS_AI_KERNEL_PUBLIC_KEY`, `OPENLUX_API_KEY`, `VOYAGEAI_API_KEY`, `CLAUDE_CODE_OAUTH_TOKEN`, `CLAUDE_CODE_WORKSPACE_ID`, `CLAUDE_CODE_ALLOWED_KEY_IDS`, `CODEX_AUTH_JSON_B64` and `SENTINEL_REPLAY_KEY`. The same goes for the native-owner bindings `UOS_NATIVE_CARRIER`, `UOS_CANDIDATE_NATIVE_V1` and `UOS_CANDIDATE_INITIAL_SEAL_V1`, which this isolated candidate must not carry, and for the Deno Deploy platform markers.
