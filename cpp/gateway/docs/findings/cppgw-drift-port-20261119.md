# Native gateway production drift

The starting C++ commit is `e97ed985ef6c39377783e0263b10d15767de1432`, the tip of
`agent-jobs/cppgw-combine-1-20261119-44443689630-1`. The production target is
`9353aa208a336da2c1516d604e0be16dc8af0ade` on `development`.

```bash
git fetch origin development
git update-ref refs/remotes/origin/development FETCH_HEAD
git log --oneline HEAD..origin/development -- src serve.ts lib static
```

## Production commits

- `a26c80947bf2aaecec5253d4e8fc36017fbdd046` `agent job agy-admin-meters-20261118a`.
  SKIP. The Antigravity branch owns the Gemini quota route, snapshot, admin assets and their tests.
- `105264f3ff90dbec4e395d9c12ea4a01eff64a46` `fix(admin): preserve Antigravity sign-in and reset evidence`.
  SKIP. The Antigravity branch owns the sign-in and quota reset evidence changes and their tests.
- `f114395740de98c951d29d92f41a7183fe722354` `fix(deepseek): compact at 75% of usable context`.
  PORT. `include/uos/deepseek/waterfall.hpp` derives the original 747,110-token threshold for the Codex
  record and configured model capabilities. `tests/deepseek_waterfall_test.cpp` checks 75%, 747,110
  tokens and the projected limit in the two existing cases. This intermediate policy is superseded by
  the next production commit.
- `0d66be90a50b8cf78a1697a1fb94eba4afb4d0c6` `fix(deepseek): use one global compaction rule`.
  PORT. `src/recent_model_context.cpp` and `include/uos/recent_model_context.hpp` compute the earlier of
  90% or a 50,000-token reserve and clamp declared limits to that ceiling. `src/catalog/store.cpp` uses
  the shared clamp on stored rows. For the gateway waterfall, `include/uos/deepseek/waterfall.hpp`,
  `src/catalog/models.cpp` and `src/input_normalization.cpp` publish 800,000 tokens with compaction at
  720,000. `include/uos/models/metadata.hpp` documents the global rule.
  `tests/recent_model_context_test.cpp`, `tests/deepseek_waterfall_test.cpp`,
  `tests/codex_catalog_test.cpp`, `tests/model_metadata_test.cpp` and `tests/openrouter_catalog_test.cpp`
  port the new boundary, the changed enrichment and cached catalog values, and the preserved early limit.
  `tests/admin_models_catalog_test.cpp` checks the new admin window and ports the two OpenAI capability cases,
  including 900,000 tokens for the official DeepSeek aliases. The catalog store, metadata and input normalization
  vector fixtures and the recent context differential expectations were regenerated from the production target.
- `f2768b2a38761d4d42312c0d49fe7874b0560b03` `fix(catalog): keep gateway DeepSeek window across metadata refresh`.
  PORT, pending. This covers same-ID Codex rows and OpenRouter enrichment of the admin catalog.
- `9353aa208a336da2c1516d604e0be16dc8af0ade` `Sentinel repair: issue 1281`.
  PORT, pending. The Passport callback classifies exchange 4xx responses as authentication failures.

## Verification

Runner scratch is `/var/tmp/cppgw-drift-port-20261119.Td3tgNyr`. Dependency preparation and all checks are pending.

## Bun recordings

```bash
git worktree add --detach "$scratch/development" origin/development
# In that worktree, with a fresh home and temp and an allowlisted environment.
bun --no-env-file install --frozen-lockfile --ignore-scripts --backend=copyfile
"$scratch/isolated-command" rerecord-compaction
```

The wrapper runs `$scratch/rerecord-compaction.mts` using Bun 1.4.0 and the native CI namespace helper.
The recorder repeats the checked-in inputs against the TypeScript exports with OpenRouter and KV mocks.
It recorded 209 metadata resolutions, 69 snapshot projections, 12 warmed-cache resolutions plus the cold
lookup, 20 catalog parses, 10 catalog store/load round trips, 167 input normalization calls and 21 compaction
boundary calls. The catalog vectors retain their 18 historical gzip fixtures. The decoded vector comparison
found changes only to compaction limits, DeepSeek windows and the served catalog body and its hash.
