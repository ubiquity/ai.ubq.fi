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
  PORT, pending. This supersedes the previous rule and advertises 800,000 tokens for the gateway waterfall.
- `f2768b2a38761d4d42312c0d49fe7874b0560b03` `fix(catalog): keep gateway DeepSeek window across metadata refresh`.
  PORT, pending. This covers same-ID Codex rows and OpenRouter enrichment of the admin catalog.
- `9353aa208a336da2c1516d604e0be16dc8af0ade` `Sentinel repair: issue 1281`.
  PORT, pending. The Passport callback classifies exchange 4xx responses as authentication failures.

## Verification

Runner scratch is `/var/tmp/cppgw-drift-port-20261119.Td3tgNyr`. Dependency preparation and all checks are pending.
