# Native gateway lifetime fixes, area D

## Contract v1

Fix or reclassify all 29 findings in the [area D lifetime audit](cppgw-lifetime-d-20261118.md) against the
integrated source at `aae39d0ea73e7b5678dc92c437f7da6fb32f0c06`. The audit-only merge is
`dc3abc2f904b8d93a8053840c05b772938841beb`. The first source unit addresses the six private race losers, followed by
startup shutdown ordering, the OpenRouter index, callback ownership, cycles and await expressions.

| Surface | Required behavior |
| --- | --- |
| Body reads, Responses, telemetry, provisioning, replay and health | Preserve the deciding result, deadlines and late operation observation. Dispose of synthetic losers once the race settles. |
| Startup and catalog | Server startup can overlap migration. Finish the migration before KV closure. Publish each catalog snapshot with its own index. |
| Admin, auth and terminal hooks | Preserve callback inputs, failure types, rejection timing, settlement, request drain and accounting. A pending callback must own its callable. |
| Await expressions and release | Follow the GCC 12.2 comparison rules. Test the supported Linux ARM64 build and publish the scoped change for review. |

The owned surface is the direct `cpp/gateway/src/` files, the assigned handler, admin, auth, authority, Sentinel,
models, catalog, lib, compaction, conversion, systemone and decisions trees and their headers, affected gateway
tests and this report. The other C++ areas, TypeScript, workflows, credentials, live provider traffic, deployment
and changes to shared runtime Promise semantics are outside scope. Wire output, storage, quota and accounting
behavior are invariants. Tests use a fresh home and temporary directory, a credential-free environment and the
portable driver's isolated loopback fixture network.

Before each source unit, resolve the current link closures for the changed files and run their release and sanitizer
gates. Final acceptance covers four affected sanitizer shards, one complete release shard,
`python3 cpp/gateway/tools/links.py --check` and `git diff --check`. The driver builds all production objects on a
cold run. The integrated runner measured about 46 minutes for cold release and 69 minutes for cold sanitizers.
Expect incremental runs to rebuild the changed objects and run the affected tests. Build only in external scratch,
use at most four compilation workers total, and keep the driver's incremental results and terminal receipts. If a
gate fails, retain the diagnostic and rerun only the affected gate after a fix. Stop when every row has an evidenced
verdict and the selected gates pass, or commit the building subset and record any outstanding work.

The current unknowns are which claims still hold after integration, the availability of the toolchain and pinned
libraries, GitHub write access and the cost of cold verification. Dependency setup is bounded to 20 minutes before
reassessment. Report rows use the first line number from each audit ledger entry as a stable identifier. The commit
column identifies a source commit by its unique subject so the report can travel in that same commit.

## Defect ledger

| Audit ID | Verdict | Commit | Test |
| --- | --- | --- | --- |
| `src/bounded_response_body.cpp:41` | LEFT · Pending re-verification. | | |
| `src/responses_stream.cpp:287` | LEFT · Pending re-verification. | | |
| `include/uos/optional_telemetry_queue.hpp:258` | LEFT · Pending re-verification. | | |
| `src/auth/local_development_key.cpp:97` | LEFT · Pending re-verification. | | |
| `src/sentinel/replay_retention_bootstrap.cpp:469` | LEFT · Pending re-verification. | | |
| `src/health.cpp:306` | LEFT · Pending re-verification. | | |
| `src/serve.cpp:123` | LEFT · Pending re-verification. | | |
| `src/models/openrouter_models.cpp:158` | LEFT · Pending re-verification. | | |
| `src/admin/codex.cpp:207` | LEFT · Pending re-verification. | | |
| `src/admin/codex.cpp:216` | LEFT · Pending re-verification. | | |
| `src/admin/codex.cpp:238` | LEFT · Pending re-verification. | | |
| `src/admin/defaults.cpp:53` | LEFT · Pending re-verification. | | |
| `src/handler/terminal_log.cpp:61` | LEFT · Pending re-verification. | | |
| `src/handler/terminal_route.cpp:523` | LEFT · Pending re-verification. | | |
| `src/handler/terminal_log.cpp:519` | LEFT · Pending re-verification. | | |
| `src/inference_admission.cpp:170` | LEFT · Pending re-verification. | | |
| `src/kv.cpp:56` | LEFT · Pending re-verification. | | |
| `src/jev_compaction/compaction.cpp:238` | LEFT · Pending re-verification. | | |
| `src/lib/jev_compaction/jev_codex_smoke.cpp:180` | LEFT · Pending re-verification. | | |
| `src/serve.cpp:159` | LEFT · Pending re-verification. | | |
| `src/handler/serve_handler.cpp:13` | LEFT · Pending re-verification. | | |
| `src/admin/defaults.cpp:99` | LEFT · Pending re-verification. | | |
| `src/auth/passkeys.cpp:4313` | LEFT · Pending re-verification. | | |
| `src/admin/codex.cpp:71` | LEFT · Pending re-verification. | | |
| `src/admin/kernel.cpp:380` | LEFT · Pending re-verification. | | |
| `src/auth/index.cpp:137` | LEFT · Pending re-verification. | | |
| `src/models/codex_models_whitelist.cpp:134` | LEFT · Pending re-verification. | | |
| `src/responses_failover_stream.cpp:944` | LEFT · Pending re-verification. | | |
| `include/uos/responses_stream.hpp:125` | LEFT · Pending re-verification. | | |

## Verification and handoff

The audit records zero UNCLEAR rows. Pending results and out-of-area ownership will be recorded here as each
source unit is checked.
