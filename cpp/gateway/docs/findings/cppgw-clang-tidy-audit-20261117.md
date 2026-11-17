# clang-tidy audit of cpp/gateway, 2026-11-17

## URGENT: request-handler children borrow a destroyed coroutine frame

**P1, confirmed lifetime defect class:** `src/admin/kernel.cpp:130,134`,
`handleAdminKernelPolicyQueueList`. If one KV read rejects while another is pending,
`rt::all` rejects without canceling the sibling. The handler unwinds and destroys
the sibling's lambda closure and referenced set; a later successful read accesses
freed memory. The TypeScript closures keep their lexical state alive.

`cppcoreguidelines-avoid-capturing-lambda-coroutines` identifies both sites. A reduced
local witness using the actual `rt::Promise`, `rt::all`, `Loop`, and microtask scheduler
reproduces **AddressSanitizer: heap-use-after-free**, exit 1, after logging
`parent completed; resuming remaining child`. This is a reduced lifetime witness,
not a claim that the real HTTP route was exercised. No external service was contacted.

Proposed minimal source diff for the owner (NOT applied). Each captureless child owns
the store and shared output set in its coroutine parameters; fail-fast behavior and
the existing await positions remain intact. Named keys retain the port's coroutine rules.

```diff
--- a/cpp/gateway/src/admin/kernel.cpp
+++ b/cpp/gateway/src/admin/kernel.cpp
@@
-  rt::OrderedSet<std::string> orgPolicyOwners; rt::OrderedSet<std::string> owners;
+  auto orgPolicyOwners = std::make_shared<rt::OrderedSet<std::string>>(); rt::OrderedSet<std::string> owners;
   for (const auto& record : *records) owners.add(record.owner);
   std::vector<rt::Promise<void>> pendingOwners;
-  auto readOwner = [kv, &orgPolicyOwners](std::string owner) -> rt::Task<void> { const auto entry = co_await kv->get(qv::kernelOrgLimitKey(owner)); if (entry.value.truthy()) orgPolicyOwners.add(owner); };
-  for (const auto& owner : owners) pendingOwners.emplace_back(readOwner(owner));
+  auto readOwner = [](std::shared_ptr<rt::Kv> store, std::shared_ptr<rt::OrderedSet<std::string>> output, std::string owner) -> rt::Task<void> { const auto key = qv::kernelOrgLimitKey(owner); const auto entry = co_await store->get(key); if (entry.value.truthy()) output->add(owner); };
+  for (const auto& owner : owners) pendingOwners.emplace_back(readOwner(kv, orgPolicyOwners, owner));
   co_await rt::all(std::move(pendingOwners)).wait();
-  rt::OrderedSet<std::string> repoPolicyPairs; std::vector<rt::Promise<void>> pendingRepos;
-  auto readRepo = [kv, &repoPolicyPairs](::uos::types::KernelPolicyQueueItem record) -> rt::Task<void> { const auto entry = co_await kv->get(qv::kernelLimitKey(record.owner, record.repo)); if (entry.value.truthy()) repoPolicyPairs.add(record.owner + "/" + record.repo); };
-  for (const auto& record : *records) if (!orgPolicyOwners.has(record.owner)) pendingRepos.emplace_back(readRepo(record));
+  auto repoPolicyPairs = std::make_shared<rt::OrderedSet<std::string>>(); std::vector<rt::Promise<void>> pendingRepos;
+  auto readRepo = [](std::shared_ptr<rt::Kv> store, std::shared_ptr<rt::OrderedSet<std::string>> output, ::uos::types::KernelPolicyQueueItem record) -> rt::Task<void> { const auto key = qv::kernelLimitKey(record.owner, record.repo); const auto entry = co_await store->get(key); if (entry.value.truthy()) output->add(record.owner + "/" + record.repo); };
+  for (const auto& record : *records) if (!orgPolicyOwners->has(record.owner)) pendingRepos.emplace_back(readRepo(kv, repoPolicyPairs, record));
   co_await rt::all(std::move(pendingRepos)).wait();
   json::array pending;
-  for (const auto& record : *records) if (!orgPolicyOwners.has(record.owner) && !repoPolicyPairs.has(record.owner + "/" + record.repo)) pending.push_back(json::value_from(record));
+  for (const auto& record : *records) if (!orgPolicyOwners->has(record.owner) && !repoPolicyPairs->has(record.owner + "/" + record.repo)) pending.push_back(json::value_from(record));
```

## Contract v2 and status

IN PROGRESS. Continuation on a fresh runner; previous temporary receipts were not retained.
The confirmed URGENT section above is retained verbatim. No full-run result is claimed yet.

Objective: run recent clang-tidy over every production and test/support C++ translation unit,
including first-party headers, and deliver a reproducible, source-bound, triaged hazard baseline.
The TypeScript/Bun 1.4.0 implementation remains the behavioral oracle. No source fixes, test
changes, credential operations, provider requests, deployment, CI changes or unrelated formatting.

Owned paths are exactly `.clang-tidy`, `analysis/tidy-baseline.json`, `tools/tidy.sh`, and this report,
all under `cpp/gateway`. All implementation defects are outside ownership and will be reported.
The production-server wrapper restrictions are explicitly overridden for this disposable runner.

Matrix: Linux x86_64; all `src/**/*.cpp` and `tests/**/*.cpp`; native-library profile; C++20 port
warning flags; separately classified production/test/header diagnostics. Other platforms, legacy
dynamic-loading compilation, runtime behavior and sanitizer execution are outside this static audit.

Acceptance: complete compile database and independent TU receipts; zero silently omitted or timed-out
units; curated installed check inventory and marked positive/negative controls; counts per check and
file; ranked defect classes with exact evidence and minimal proposed fixes; explicit false-positive
classes; portable runner; changed-document/configuration formatting and `git diff --check`.
No whole-repository TypeScript, Rust or deployment gates are selected for this tooling/report change.

Unknowns being resolved: newest installable LLVM, dependency availability, full-run cost, check noise,
analyzer limitations on coroutine frames, and authenticated Git publication/integration capability.
Stop condition: all units reconciled with source hashes and all reporting checks triaged, or explicit
terminal evidence of an external blocker. No incomplete run will be labeled a complete baseline.

Long-operation budget: pilot representative units first, then at most four workers; expected full-run
time 15–60 minutes, per-TU timeout and persistent progress/logs under `/tmp/cppgw-tidy-20261117`.
Inspect a confirmed-live handle at most once per minute; retain failures and adjust only with a recorded
reason. All builds, downloads, HOME/TMPDIR directories and raw evidence stay outside the checkout.

## Discovery checkpoint

- Starting commit: `141cb4d4`; branch `agent-job`; initially clean checkout.
- Runner: Ubuntu 26.04.1 x86_64, four CPUs, approximately 15 GiB RAM and 89 GiB free disk.
- Existing scaffold selects useful lifetime/copy checks and marks controls, but its tool/evidence paths,
  serialized ten-unit driver and production resource admission are host-specific. Reuse its check
  rationale and controls; implement the portable complete-tree runner only in the owned `tools/tidy.sh`.
- Dependencies, exact commands, diagnostic counts and triage will be appended as they become available.

## Live discovery and triage checkpoint

- Installed apt.llvm.org's newest available snapshot: Clang/clang-tidy 24.0.0,
  revision `51d7f08b16b6`, package timestamp `20261117085349`. The distribution's
  default is 21.1.8; 22.1.2 is also preinstalled. Do not infer release qualification
  from this snapshot's static-analysis results.
- Actual census is 369 production and 461 test/support translation units, 830 total.
  Discovery selects 245 installed checks. A four-unit pilot completed 4/4 without
  frontend failures, in 81.033 seconds; largest process RSS 699,196 KiB.
- The five marked defect controls and three marked coroutine controls all fire,
  and `analysis/controls/clean.cpp` has zero diagnostics. The tool emits ten total
  diagnostics because two controls also trigger an analyzer/move or string-view check.
- Full run is active under `/tmp/cppgw-tidy-20261117/discovery`, four workers,
  600-second per-unit timeout. Initial 50 units have zero frontend failures.
- Candidate serious class: `src/admin/kernel.cpp:130,134` creates coroutine lambdas
  borrowing handler-local sets, then awaits fail-fast `rt::all`. A rejected KV read
  unwinds the handler while another read may continue; its closure and output set
  would then be dead. Trace: `include/uos/rt/async.hpp:805` implements rejection
  without sibling cancellation. A reduced test using the actual runtime is being
  built before assigning a final severity. No source fix has been applied.
- Contrast: `admin/codex.cpp:103` copies its lambda into a by-value callback parameter
  in the awaited coroutine; the callback remains owned. The kernel usage helpers at
  160/174 catch ordinary dependency exceptions, so the same failure scenario must
  not be asserted indiscriminately for every capture warning.

## Continuation evidence (contract v2)

The continuation begins at commit `33e1539d74bc3b69566875b449a01c97b0cfca3d`,
HEAD tree `69b64971f1cf1c9877e7a29b6aa61742f9efcd4f`, on a fresh Ubuntu 26.04.1
x86_64 runner (four CPUs, 15 GiB RAM). Earlier `/tmp` artifacts are unavailable;
the URGENT reproduction above is retained prior-agent evidence, not a new execution claim.
All current-run evidence is under `/tmp/cppgw-tidy-20261117`.

Newest apt.llvm.org snapshot available is again 24.0.0, revision `51d7f08b16b6`,
package `1:24~++20261117085349+51d7f08b16b6-1~exp1~20261117085401.350`.
The existing runner's full source/test census and 245-check inventory were verified.
The runner now atomically publishes an explicitly incomplete baseline at startup,
then checkpoints counts, completed-unit identities and deduplicated diagnostics every
100 completed units. It never treats an interrupted partial run as complete.

Existing controls were rerun with the exact audit configuration and compile flags:
**5/5 defect markers, 3/3 coroutine markers, 0 clean-control diagnostics**; ten
unique emitted diagnostics include two overlapping analyzer/string-view reports.
All three control TUs returned zero (warnings are findings, not frontend failures).
Control wall time 1.260 seconds; maximum process RSS 153,864 KiB.

Git preflight: `gh auth status` reports no authenticated host;
`git push --dry-run origin HEAD:refs/heads/chore/cppgw-clang-tidy-audit-20261117b`
returns `fatal: could not read Username for 'https://github.com': No such device or address`.
No credential was printed, written, refreshed or uploaded. Publication and integration
must use the enclosing job's authorized delivery mechanism unless write access appears.

## Full-run checkpoints

| Completed | Frontend failures | Timeouts | Unique diagnostics | Wall seconds | Largest TU RSS (KiB) | Inputs unchanged |
| ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 100/830 | 0 | 0 | 938 | 756.181 | 845964 | yes |

The baseline remains explicitly incomplete until all 830 receipts reconcile. This table
records persisted runner checkpoints, not extrapolated pass counts. All units analyzed so far
emit shared-header warnings; a successful frontend is not a warning-free audit pass.

## Incremental check triage

Each named check has sampled source-path evidence below. Counts remain in the baseline;
classification is per defect class, not an assertion that a warning proves a bug.

### `bugprone-branch-clone`

False positives in `include/uos/rt/nullable.hpp:32,33` and `include/uos/rt/record.hpp:68`: if-constexpr branches deliberately share expression text while selecting different types/codecs; enum and string types both describe a string on the wire. This is not a duplicated runtime branch with missing validation.

### `bugprone-empty-catch`

Real but benign in sampled `src/admin/chart_source_proxy.cpp:43` and `src/admin/kernel.cpp:392,409`: optional fallback/JSON parse handling deliberately swallows an error before returning the source-compatible fallback or validation response. The TypeScript's catch-and-continue behavior is the oracle; do not globally convert these catches into request failures.

### `bugprone-exception-copy-constructor-throws`

Real but benign resource-exhaustion risk at `src/api_key_policy.cpp:502`: throwing the named const ApiKeyQuotaDispatchError copies JsError string fields, which may allocate. A failed allocation can replace the intended error; no ordinary request value makes this copy constructor explicitly throw. Minimal improvement is a non-const error moved at its last use, preserving all fields. Treat allocation-failure exception handling as hardening, not a proven request-path memory-safety defect.

### `bugprone-float-loop-counter`

Real but benign in `src/analytics.cpp:126` and `src/api_key_policy.cpp:237,310,346,398,669`: integer-valued double retry counters with fixed small limits and increments of one mirror TS Number. Values stay exactly representable, so there is no rounding-driven infinite loop. Do not change arithmetic merely to satisfy a C++ convention. Input-facing CBOR samples at `src/auth/passkeys.cpp:388,404` also remain bounded: decodeLength rejects lengths above 2^53-1 and every iteration consumes bytes or rejects exhausted input. PEM chunking at 2289 iterates over the actual string length. This additional sample does not reveal a rounding-stalled loop.

### `bugprone-implicit-widening-of-multiplication-result`

Real but benign at `include/uos/request.hpp:20` and `include/uos/sentinel/upstream_capture.hpp:43`: 32*1024*1024 and 4*1024*1024 fit int before conversion to int64_t. No overflow occurs. Use a suffixed first operand only if changing these constants in future.

### `bugprone-optional-value-conversion`

Real but benign at `src/admin/sentinel_incident.cpp:152`: limit has already passed validPageLimit and is checked before dereference. Assigning limit directly would be simpler; no unchecked input path or behavioral defect demonstrated.

### `bugprone-reserved-identifier`

Real but benign naming concern at `include/uos/provider/stream_relay.hpp:24`: the private parameter return__ contains a reserved double underscore. Renaming that parameter would improve portability, but no request-path lifetime failure is demonstrated and source renames are outside this hazard audit. CERT naming aliases were excluded initially, while bugprone-* independently enables the canonical check; disable the canonical name in the final configuration.

### `bugprone-return-const-ref-from-parameter`

False positive at `include/uos/rt/ordered.hpp:249`: SetKey is an internal projection called on table-owned live entries, not temporaries. Returning the original key reference is intentional; replacing it with a copy is unnecessary. A second sample, `src/auth/passkeys.cpp:1651`, validates and returns requireMap's input; every current caller passes a named decoded CBOR object or a caller-owned key reference, so no temporary escape is demonstrated.

### `bugprone-signed-bitwise`

Initial sample is benign: `include/uos/rt/number.hpp:85,226` shifts unsigned 64-bit operands by signed positive literal counts 63 and 20; `include/uos/rt/string.hpp:180` shifts an unsigned code point by literal 10. C++ permits these counts. This is not evidence of shifting a negative value. Later arithmetic sites remain to be sampled. Additional passkey CBOR/base64 samples at `src/auth/passkeys.cpp:316,487,488` use nonnegative promoted bytes or unsigned accumulators and positive constant shift counts. No negative left operand or out-of-range shift was demonstrated; literal-count warnings are disproportionately noisy.

### `bugprone-std-exception-baseclass`

Real but benign: `include/uos/rt/async.hpp:1036,1079` and `include/uos/rt/stream.hpp:548` throw the private `GeneratorReturn` token, caught by the generator's exact-type handler to implement iterator return. This is intentional control flow distinct from JavaScript errors; retain behavior rather than changing the exception hierarchy.

### `bugprone-throwing-static-initialization`

Real but benign startup risk: fixed inline KvKey/JSON/string constants (for example API_KEY_CHANGE_PREFIX at `include/uos/authority/native_api_key_audit.hpp:13`) allocate before main; malformed request data cannot enter these initializers. Allocation failure can terminate startup, also consistent with a process unable to allocate. Literal regex initialization is separate from request parsing. No request-path exception escape is established by this check (alias cert-err58-cpp).

### `bugprone-unchecked-optional-access`

Initial sample: false positives on runtime invariants at `include/uos/rt/async.hpp:221,377,516` (value follows successful settlement; rejected states rethrow), `include/uos/rt/regex.hpp:52-54` (successful regex always has participating group zero), and `include/uos/rt/nullable.hpp:100` (unchecked interface with caller precondition). `src/admin/capacity_observation_sync.cpp:37,44` follows `!step.done`, which guarantees a chunk. `src/admin/api_keys.cpp:131,140,141,147` follows a validated `name` and checked `quotaPolicy`; `rt::truthy(optional)` is not modeled. `src/admin/api_key_mutations.cpp:87` serializes a constructed KvObject, whose top-level conversion always returns a string or throws, never nullopt. Further production parser sites remain under review; no global safety claim yet. More input-facing samples: `src/authority/native_api_key.cpp:95,103-107` follows isSafeNonNegativeInteger helper predicates; `src/authority/native_api_key_policy.cpp:139-142` follows hasStrictPaidFallbackPolicy, which validates these exact numeric/bool fields. `src/authority/native_authority_api.cpp:148,149` explicitly requires an engaged lifetime and api before the flagged expression. These are unmodeled validation relationships, not new unchecked-request defects.

### `bugprone-unhandled-self-assignment`

False positive at `include/uos/rt/ordered.hpp:182`: copy assignment attaches the incoming table before detaching the outgoing one. On self-assignment this increments then decrements the same iterator count without reclaiming the table. The final table and position remain valid.

### `cert-err09-cpp`

Real but benign at `src/api_key_policy.cpp:502`: the named thrown value has the exact ApiKeyQuotaDispatchError type, so no slicing occurs. The related potentially throwing-copy warning is documented separately; naming it is not itself an incorrect exception value.

### `clang-analyzer-security.FloatLoopCounter`

Same benign bounded-double-counter class as bugprone-float-loop-counter; sampled `src/analytics.cpp:126` and API-key retry loops. No unbounded or fractional-step request-controlled counter was found in this sample. Input-facing CBOR samples at `src/auth/passkeys.cpp:388,404` also remain bounded: decodeLength rejects lengths above 2^53-1 and every iteration consumes bytes or rejects exhausted input. PEM chunking at 2289 iterates over the actual string length. This additional sample does not reveal a rounding-stalled loop.

### `cppcoreguidelines-avoid-capturing-lambda-coroutines`

Mixed: confirmed P1 defect at `src/admin/kernel.cpp:130,134`, plus legitimately retained callbacks. The complete one-line site verdict table below is the authoritative maintainer list. The checker detects syntax and cannot recognize rt::async, generator body retention, Promise callback keepAlive, SourceAlgorithm, or owning callback parameters; capture-by-value alone is not sufficient. No source fixes applied.

### `cppcoreguidelines-avoid-reference-coroutine-parameters`

Initial sample is real but benign borrowing under structured await. `src/cache/kv_migration.cpp:37` borrows progress from the migration caller's live frame; the sole caller starts a Promise and awaits its completion at 161-163. Private collectors at `src/cache/kv_migration_paid_fallback.cpp:143,169,194,216,235` borrow knownKeyIds/errors from inspectPaidFallbackV3; callers at 445-452 await each task sequentially and keep all referents in the parent frame. No task is detached or raced in these calls, so ordinary failure destroys the active child before its parent state. Prefer owning state for APIs that can escape; these internal helpers do not establish a UAF. Further reported coroutine-reference sites remain to be classified. Further structured-borrow samples: migration projection repairOnePaidFallbackWindow at `src/cache/kv_migration_projection.cpp:484` is immediately awaited at 535 with a named local context; usage collectStrictApiKeyPairs at `src/cache/kv_migration_usage.cpp:716` is awaited at 882 while hashEntries/errors remain in scope. Migration validation auditApiKeyBoundedAggregates at 151 and auditApiKeyPolicy at 169 are similarly sequential. Catalog store APIs explicitly require the Kv object to outlive their tasks/heartbeat (PORT-NOTES); their reference signatures are an API precondition, not automatic evidence that current callers violate it.

### `performance-expensive-value-or`

Real but benign copy at `src/admin/kernel.cpp:329`: lvalue optional<vector<rollup>>.value_or(empty vector) copies all rows even when present. Bind an empty vector and select `rollupRows ? *rollupRows : emptyRows` by const reference before groupPaidFallbackUsageRollups. This is admin analytics, not a demonstrated inference latency defect.

### `performance-faster-string-find`

Real but benign micro-optimization at `src/auth/passkeys.cpp:284,1250`: string += a one-character literal can use the char overload. No oracle-visible behavior or measurable request latency failure is established; this is outside the hot-path ownership focus.

### `performance-inefficient-string-concatenation`

Real but benign allocation cost at `src/authority/native_authority_sql.cpp:80-82`: CREATE INDEX strings repeatedly concatenate the same table name during schema setup. Reserve/append could reduce temporaries without changing SQL, but this is startup work, not a demonstrated hot request path. Keep identifiers and exact SQL behavior if optimizing.

### `performance-inefficient-vector-operation`

Real but benign allocation debt: `src/auth/local_admin.cpp:58` pushes four parsed IPv4 octets; `src/admin/codex.cpp:53` and kernel account/row loops build Promise vectors of known size. Minimal improvement: reserve the exact input size before the loop. This changes allocation cost only; no correctness failure or measured hot-path regression is demonstrated.

### `performance-move-const-arg`

Initial sample is benign: `include/uos/rt/websocket.hpp:290` moves a trivially copyable SendOptions. The move has no effect and incurs no expensive copy. Real const string/JSON moves will be separated if later diagnostics report them. Additional real but benign copies: `src/authority/authority_rpc.cpp:170` passes std::string into Boost.JSON, which must copy into its own storage; `src/authority/native_codex_capacity.cpp:71,277` uses a conditional whose const slot arm prevents moving the other arm. Split that conditional into explicit branches to enable the intended owning move; no correctness failure demonstrated.

### `performance-no-automatic-move`

Real but benign optimization debt: `src/admin/api_keys.cpp:224`, `src/admin/kernel.cpp:61`, and `src/agent_messages.cpp:72,84,101` return const local strings/optionals and may copy into the optional result. Minimal improvement: remove const from the local and retain automatic move on return. No latency claim is made without measurement; do not alter oracle-visible values.

### `performance-unnecessary-copy-initialization`

Mixed false positives and benign optimization debt. `include/uos/rt/async.hpp:859,891` makes shared-state copies before installing by-value async callbacks; the copies express ownership and must not be blindly replaced by borrowed storage. `src/admin/sentinel_incident.cpp:131` copies an immutable optional string; `src/auth/passport.cpp:316` copies request.method() in a synchronous validator, where a const reference is safe. Neither is a measured latency defect. Template specializations at the same source line are distinct diagnostic messages and remain separately counted.

### `performance-unnecessary-value-param`

Mixed real benign copies and lifetime-conservative design. Sample `src/admin/codex.cpp:102` copies std::function into the owned reader; `src/admin/defaults.cpp:130` is a plain forwarding function that can move req into its by-value target. `include/uos/rt/async.hpp:692` deliberately owns a callback while invoking code that can reenter; do not replace owning coroutine/state parameters with references. `src/authority/authority_rpc.cpp:168` moves operation into Boost.JSON but that assignment still copies characters, so the warning is not proof of a dangling or broken move. Optimize only plain synchronous boundaries after an ownership audit.

### `performance-use-std-move`

Real but benign last-use copy suggestions at `src/admin/claude_code.cpp:168` (credential into patch) and `src/agent_messages.cpp:309` (JSON nextSince into response). A move at the proven last use removes one copy; ownership and oracle behavior must remain unchanged. No source edit applied.

## Capturing coroutine lambda site verdicts (incremental)

Every reported site gets one row. Real but benign means the current call path retains the
closure/referents; copies of captures alone do not provide that guarantee. No source fix
is applied. Test verdicts describe fixture lifetimes, not runtime test-pass claims.

### Production and headers

| Site | Verdict |
| --- | --- |
| `src/admin/codex.cpp:103` | Real but benign: forwarding overload copies `reader` into the awaited handler's by-value `ReadBaselineJson` parameter, which owns the closure until completion. |
| `src/admin/kernel.cpp:130` | TRUE DEFECT, P1/URGENT: fail-fast `all` can destroy the handler's closure/set while a sibling KV read still resumes; proposed owning-parameter diff above. |
| `src/admin/kernel.cpp:134` | TRUE DEFECT, P1/URGENT: same escaping sibling lifetime for repository reads and `repoPolicyPairs`; proposed owning-parameter diff above. |
| `src/admin/kernel.cpp:160` | Real but benign for ordinary KV errors: getKernelOrgUsage catches dependency errors and returns nullopt, so all waits for every child; closure/map remain in the parent frame. Allocation failure or a throwing logging observer still needs exceptional-path ownership hardening. |
| `src/admin/kernel.cpp:174` | Real but benign for ordinary KV errors: same caught-error behavior in getKernelUsage; no independent escaping-read defect demonstrated. Owning child parameters would harden allocation/logging failure paths. |
| `src/chat/envelope.cpp:180` | Real but benign: ResponsesStreamIterator/AsyncGenerator copies the body into runGenerator's by-value body parameter and keeps first alive through yields and cleanup. |
| `src/chat/stream_translation.cpp:359` | Real but benign: AsyncGenerator owns the closure in runGenerator; captured shared prepared-state survives all yields and iterator cleanup. |
| `src/chat/stream_translation.cpp:369` | Real but benign on current production calls: cancel is stored in StreamState.source; SourceAlgorithm retains the outer state-owning callback until StreamState::cancel has awaited it. Calling a detached task from a temporary adapter would require additional ownership. |
| `src/codex/account_routing.cpp:303` | Real but benign: withCodexAdmission takes action by value and awaits it; captured KV/account values and closure survive the admission lock and operation. |
| `src/codex/overage_settings.cpp:73` | Real but benign: each parallel child is wrapped in rt::async, whose shared holder owns the closure even if all rejects before other children finish. |

## Ranked confirmed defect classes

1. **P1 memory safety, authenticated admin request path:**
   `src/admin/kernel.cpp:130,134`, the two `handleAdminKernelPolicyQueueList` children.
   One rejected KV read makes fail-fast `rt::all` reject while a sibling still runs;
   unwinding frees the closure/output set the sibling accesses after its read.
   Minimal fix: captureless child coroutines with by-value KV/shared-output parameters,
   preserving fail-fast semantics and await positions (exact proposed diff in URGENT).
   Current-run clang-tidy warning is reproduced; the ASan witness is retained earlier-agent
   evidence, not a fresh run or real HTTP request. No source change applied.

Other warnings below are currently classified as benign real costs/conventions or false positives;
additional true defects will be ranked here as the remaining units complete. Resource-exhaustion-only
hardening and unmeasured optimization opportunities are not promoted into confirmed request bugs.

## Current reproduction commands

Commands run from the repository root. All dependency downloads and raw audit artifacts
are outside the checkout; system packages are installed on this disposable runner only.

```sh
sudo apt-get update -qq
sudo apt-get install -y -qq libboost-json-dev libboost-container-dev libbrotli-dev python3-yaml time gnupg curl
curl --fail --silent --show-error https://apt.llvm.org/llvm-snapshot.gpg.key -o /tmp/cppgw-tidy-20261117/llvm-key.asc
sudo gpg --dearmor --batch --yes -o /usr/share/keyrings/llvm-snapshot.gpg /tmp/cppgw-tidy-20261117/llvm-key.asc
printf '%s\n' 'deb [signed-by=/usr/share/keyrings/llvm-snapshot.gpg] https://apt.llvm.org/resolute/ llvm-toolchain-resolute/main amd64 Packages' | sudo tee /etc/apt/sources.list.d/cppgw-llvm.list
sudo apt-get update -qq
sudo apt-get install -y -qq clang-24 clang-tidy-24

cpp/gateway/tools/tidy.sh --compiler clang++-24 --jobs 4 \
  --build-dir /tmp/cppgw-tidy-20261117/controls \
  --source analysis/controls/defects.cpp --source analysis/controls/coroutines.cpp --source analysis/controls/clean.cpp

cpp/gateway/tools/tidy.sh --compiler clang++-24 --jobs 4 --timeout 600 \
  --build-dir /tmp/cppgw-tidy-20261117/discovery \
  --baseline cpp/gateway/analysis/tidy-baseline.json --checkpoint-every 100
```

The last command uses the broad configuration retained in the baseline's
`configuration_yaml`. Once the final default configuration is curated, reproduce this
same audit with `--audit` and a fresh build directory; use `--resume` only with existing
matching receipts. `--source` is for controls/pilots and never claims full coverage.
Warnings do not fail the command by default; `--fail-on-findings` makes findings fatal.
Frontend errors, timeouts and changed source/config identities always fail it.

Every database entry uses the selected Clang driver with these exact port flags,
plus absolute include/source/output paths resolved from the current checkout:

```text
-std=c++20 -O0 -g0 -Wall -Wextra -Wshadow -Wconversion -Werror
-DBOOST_JSON_NO_LIB -DBOOST_CONTAINER_NO_LIB -DBOOST_SYSTEM_NO_LIB
-DUOS_GATEWAY_NATIVE_LIBRARIES -pthread -I<gateway>/include -I<gateway>
-c <source> -o <external-build-dir>/obj/<source>.o
```

clang-tidy parses/analyzes each TU; it does not emit/link those object files.
Exact per-TU invocation is retained in each receipt:

```text
clang-tidy-24 <absolute-source> -p=<external-build-dir> --config-file=<configuration>
--quiet --export-fixes=<external-build-dir>/units/<source>.yaml
```

`--export-fixes` exports diagnostics/proposed replacements only. No `--fix` was used
and no replacement is applied. Compiler and analyzer versions/hashes, all selected
checks, flags, package versions, and hashes of every first-party C++ input are stored
in the baseline. Repeated header diagnostics are deduplicated by check, file, line,
column and message; differing template-specialization messages remain distinct.
Raw TU emissions are counted separately. Clang's canonical DiagnosticName is counted
once even if console output also names a CERT alias.

Scope limits: this is the native Linux/libstdc++ header profile, not libc++, Darwin,
legacy dlopen, runtime execution or a linked dependency-ABI test. The analyzer cannot
prove the lifetime of arbitrary coroutine/member objects or model every helper predicate,
exceptional callback and async state transition. A zero count is not a memory-safety proof.
The supplied controls establish that representative move, optional, dangling-view,
heap-use-after-free, null and coroutine checks execute on this compiler.

## Final handoff summary

Continuation checkpoint: runner now atomically saves the partial baseline and deduplicated
diagnostics at startup and every 100 completed units. The full audit, triage, final configuration
and publication receipts remain pending. No earlier partial count is reused as current evidence.
No application source has been changed. No C++ test binary has been executed by this audit yet.
