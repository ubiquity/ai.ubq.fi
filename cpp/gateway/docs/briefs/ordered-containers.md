# Use native membership sets and stable ordered storage

Plan row 46. Target 50–90 net first-party lines. Risk 3. Start with local membership variables before changing the shared table.

Own the membership-only sets at `src/models/catalog.cpp:46–50,193–195,252–260` and `src/models/openrouter_models.cpp:266–280`. Own the table and iterators in `include/uos/rt/ordered.hpp:65–239` and any necessary internal storage wiring at `261–371`. Use `tests/rt/ordered_test.cpp` for the current behavior. Keep changes to callers out of SQLite, HTTP request/socket tracking, durable KV and account routing.

1. Move the named local membership sets to `std::unordered_set<std::string>`. Keep output order in the existing vectors or JSON arrays. Preserve the present duplicate policies. For example, the seen set in `openRouterModelsFromPayload` gets updated as each model is appended, while the set in `appendClaudeModels` represents the pre-existing list. Leave `uniqueStrings` in `openrouter_models.cpp:54–57` with ordered deduplication.
2. Store the remaining ordered entries in `std::list` nodes and index live keys in `std::unordered_map` using stable list iterators. Preserve transparent string lookup and floating-key normalization/equality. Keep stable references by marking an erased node during live traversal and unlinking once the last traverser releases it. Keep lookup of erased entries false immediately and preserve a new insertion at the tail. Reclaim without a full scan on mutations that leave no retired entries.
3. Maintain ordered snapshots, copy independence, value updates in place and append/delete/clear during traversal, including appended entries visited during the same traversal. Preserve current constraints on iterator lifetime. Keep the fixture cases for SameValueZero and the heavy-churn reference model.

```bash
for test in tests/rt/ordered_test.cpp tests/rt/sqlite_test.cpp tests/openrouter_models_test.cpp \
  tests/admin_models_catalog_test.cpp tests/codex_catalog_reasoning_order_test.cpp \
  tests/runtime_http_lifetime_test.cpp tests/serve_delivery_http_test.cpp; do
  bash cpp/gateway/tools/ci-build.sh --config release "$test" || exit 1
done
```

Accept when model order and duplicate policy are intact, shared traversal fixtures pass unchanged, and slot indexing and relocation are gone. Measure the actual net deletion and ARM64 churn cost. [Existing table](../../include/uos/rt/ordered.hpp#L65) · [Isolated driver](../CI.md)
