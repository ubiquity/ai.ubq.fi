# TypeScript changes after the C++ branch point

Compared `0a6e31a5ca` through `typescript-final-development` against native `development` at `37c1fa27`.

## `6fb30e8ea8` Passport issuer

Ported in `src/auth/passport.cpp` and `src/auth/passport_sessions.cpp`. The fixed origin for JWT `iss`, browser authorization, code exchange, JWKS, app-session introspection and revocation is `https://auth.ubq.fi`. Verification requires RS256 against the fixed JWKS, a valid lifetime, client and audience `ai`, and the existing claim and cookie checks. The app origin is `https://ai.ubq.fi`, with `/auth/callback` and PKCE S256. Persistent session records and upstream app-session bindings use the same fields. Exchange, JWKS, introspection and revocation reject redirects and have 10-second deadlines. A JWKS outage returns 502 when a key must be fetched. A persistent session introspection outage returns 503 and retains the local record. Logout removes the local record and reports an unavailable upstream revocation separately. The exchange transport repair is tracked under `87a2ecac9f`.

When Landlock TCP filtering is enabled, the production profile permits DNS fallback and HTTPS on TCP ports 53 and 443. The service-manager fallback allows unicast and restricts socket families. `auth.ubq.fi` uses the existing HTTPS permission. Side-by-side observation is restricted to IPv4 loopback, and the isolated tests use fetch mocks. [Startup policy](../../runtime/startup.cpp#L367) · [Service-manager policy](../SECURITY-LIMITS.md#service-manager-confinement) · [Side-by-side operation](../SIDE-BY-SIDE.md)

`passport_test` checks the authorize and exchange contract, canonical signature validation and rejection of a signed token carrying the previous issuer. `passport_sessions_test` checks introspection and local-record handling. The 68 recorded cases in `passport_differential_test` retain their response and storage assertions, with the issuer URLs and signatures updated using a new in-memory synthetic RSA key.

## `87a2ecac9f` Sentinel issue 1282

Ported in `src/auth/passport.cpp`. A rejected exchange fetch, including a network failure or a timeout, returns 502 `passport_unavailable`. A successful HTTP exchange with an invalid token returns 401 `invalid_api_key`. `passport_test` translates both transport failures and the invalid-token case, checking one POST with an abort signal, the exact state and verifier cookie deletions and `Cache-Control: no-store`.


## `9353aa208a` Sentinel issue 1281

Already equivalent in native commit `e509992d`. The callback cancels an exchange HTTP 4xx body and returns 401 `invalid_api_key`. Other unsuccessful exchange HTTP responses continue to return 502 `passport_unavailable`. In `passport_test`, the rejected-exchange and upstream-failure cases exercise 400, 401, 403, 422, 429 and 500, 502, 503, 504, with one exchange and the exact transient cookie deletions for each.


## `012c1b7dda` Sentinel issue 1377

Not applicable. The TypeScript Antigravity status-line hook, `scripts/export-antigravity-quota.ts`, treated identical raw reset time and countdown values as duplicate observations during its 30-second write window. Its tests checked that concurrent identical countdowns of zero and a positive value preserved the file inode and observation timestamp, then checked a fresh write after 31 seconds and an immediate write for a changed countdown.

The native gateway has no status-line hook or event-driven writer. `src/admin/gemini_quota.cpp` obtains provider data from the explicit CLI `/usage` command, shares a pending refresh and reuses a recent snapshot for ordinary reads and refreshes. The admin force-refresh path intentionally records a new observation. The native quota refresh case in `gemini_subscription_test` covers this path.


## `0d66be90a5` Global compaction rule

Already equivalent in native commit `f69df650`. `src/recent_model_context.cpp` uses the earlier of 90% of the model window and the 50,000-token reserve, with a percentage fallback for windows at or below the reserve. A valid declared limit can move compaction earlier. The synthetic DeepSeek route advertises an 800,000-token window and a derived 720,000-token threshold in its versioned Codex row and model capabilities. Its 95% effective window is 760,000 tokens. The separate Rust projection is outside the native tree.

`recent_model_context_test` covers the arithmetic, invalid window and source-limit clamp. `deepseek_waterfall_test` checks the synthetic window and threshold. `codex_catalog_test` and `model_metadata_test` check 943,718 and 945,000 for enriched rows. `openrouter_catalog_test` covers revalidation at 945,000 and 784,800, preservation of a 200,000 source limit, the original stored upstream body and ETag, and the changed client ETag for corrected cached rows. `admin_models_catalog_test` covers the 222,000-token limit for a 272,000-token Codex window and the 900,000-token limit for the direct DeepSeek route.


## `f2768b2a38` DeepSeek window across refreshes

Already equivalent in native commit `689b828f`. `src/catalog/models.cpp` replaces a same-ID upstream row in place and appends a gateway row when absent. `src/models/catalog.cpp` uses gateway-owned metadata for the synthetic ID, so the rolling OpenRouter alias cannot widen it. `deepseek_waterfall_test` checks that the replacement keeps its neighbors in order. `admin_models_catalog_test` supplies a 1,048,576-token OpenRouter alias and checks the configured-hop and paid-hop cases for the 800,000-token current and maximum windows, the 720,000-token trigger and the existing reasoning tiers.

## Verification

The tested commit was `5f2bf8b885d8efd04d1a8e4dd38e12d125cc5430` with tree `f2529880e578b714802ec51042baf99a5da25c78`. Every driver receipt reported stable inputs and the input SHA-256 `1cd77b9858ad4242cb241c25eccdf1f52093f35dd2681e53aeabf3979f817e19`.

The Ubuntu 26.04 ARM64 runner used the gateway CI workflow's distribution dependencies, Clang 21.1.8 and pinned SQLite 3.54.0. The driver proved loopback reachability and outward route denial before running each binary with a fresh home, private temporary filesystem and credential-free environment. Both Release and ASan, UBSan and LSan passed 14 binaries and 209 cases per profile.

```bash
python3 -B cpp/gateway/tools/links.py --check
python3 -B cpp/gateway/tools/cmake-sources.py /var/tmp/cppgw-post-sources.cmake
git diff --check origin/development...HEAD

for filter in passport deepseek_waterfall_test admin_models_catalog_test codex_catalog_test \
    openrouter_catalog_test recent_model_context model_metadata_test models_metadata_differential_test \
    catalog_store_differential_test transport_normalization_coverage_test gemini_subscription_test; do
  CC=clang CXX=clang++ UOS_CI_JOBS=2 bash cpp/gateway/tools/ci-build.sh --config release "$filter"
  CC=clang CXX=clang++ UOS_CI_JOBS=4 bash cpp/gateway/tools/ci-build.sh --config asan "$filter"
done
```

| Driver filter | Binaries per profile | Cases passed per profile |
| --- | ---: | ---: |
| `passport` | 3 | 24 |
| `deepseek_waterfall_test` | 1 | 13 |
| `admin_models_catalog_test` | 1 | 35 |
| `codex_catalog_test` | 1 | 27 |
| `openrouter_catalog_test` | 1 | 9 |
| `recent_model_context` | 2 | 6 |
| `model_metadata_test` | 1 | 17 |
| `models_metadata_differential_test` | 1 | 4 |
| `catalog_store_differential_test` | 1 | 11 |
| `transport_normalization_coverage_test` | 1 | 36 |
| `gemini_subscription_test` | 1 | 27 |

The source inventory validated 323 modules and 45 runtime sources. The registration check passed all 453 entries. Driver logs and JSON receipts are under `/var/tmp/cppgw-post-gates.dWIWEJ` on the runner.

The local native Codex 0.171.0 review used base `37c1fa27ff2639779200fa1b420be111ed2cf6d6`, the tested head above and the repository's previously documented `gpt-6.1-sol` review model. It exited 0 with no actionable findings. The review log is `/var/tmp/cppgw-post-review.5tPyEe/review.log`.

```bash
timeout -k 5s 240s codex review --base origin/development \
  -c 'model="gpt-6.1-sol"' -c 'review_model="gpt-6.1-sol"' \
  -c 'sandbox_mode="read-only"' -c 'shell_environment_policy.inherit="none"' \
  -c 'shell_environment_policy.set={PATH="/usr/bin:/bin"}'
```
