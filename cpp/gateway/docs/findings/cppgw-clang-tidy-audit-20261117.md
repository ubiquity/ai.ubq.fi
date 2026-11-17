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

Long-operation budget: pilot representative units first, then at most four workers; initial expected full-run
time 15–60 minutes, revised to roughly 110–140 minutes after measuring source and test TUs, per-TU timeout and persistent progress/logs under `/tmp/cppgw-tidy-20261117`.
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
| 200/830 | 0 | 0 | 1720 | 1342.68 | 845964 | yes |
| 300/830 | 0 | 0 | 2447 | 2079.729 | 906484 | yes |
| 400/830 | 0 | 0 | 3797 | 3056.977 | 906484 | yes |
| 500/830 | 0 | 0 | 6500 | 4328.626 | 906484 | yes |
| 600/830 | 0 | 0 | 8577 | 5303.014 | 906484 | yes |
| 700/830 | 0 | 0 | 10815 | 6524.375 | 1019216 | yes |
| 800/830 | 0 | 0 | 13112 | 7806.283 | 1019216 | yes |

The baseline remains explicitly incomplete until all 830 receipts reconcile. This table
records persisted runner checkpoints, not extrapolated pass counts. All units analyzed so far
emit shared-header warnings; a successful frontend is not a warning-free audit pass.

Production phase reconciliation: **369/369 source TUs analyzed**, zero frontend
failures/timeouts. The baseline's next 400-unit checkpoint includes this completed
production phase and the beginning of test/support analysis. Twenty-nine capturing
coroutine sites were reported by the production phase and all received verdicts;
additional production-header template sites can still be instantiated by tests.

## Incremental check triage

Each named check has sampled source-path evidence below. Counts remain in the baseline;
classification is per defect class, not an assertion that a warning proves a bug.

### `bugprone-bitwise-pointer-cast`

Real but benign within the audited POSIX ABI at `src/rt/gzip.cpp:105`: memcpy transfers a dlsym address into a function-pointer slot. POSIX dynamic linking requires callable symbol conversion and these supported Linux pointer representations have matching size. This is not portable ISO-only code, but no malformed request or current-ABI lifetime failure is demonstrated. A different function/object-pointer ABI would require a new loader contract, outside this Linux profile.

### `bugprone-branch-clone`

False positives in `include/uos/rt/nullable.hpp:32,33` and `include/uos/rt/record.hpp:68`: if-constexpr branches deliberately share expression text while selecting different types/codecs; enum and string types both describe a string on the wire. This is not a duplicated runtime branch with missing validation.

### `bugprone-derived-method-shadowing-base-method`

Real but benign fixture behavior at `tests/helpers/admin_auth_harness.hpp:34`: TestKvStore::clear intentionally extends OrderedMap::clear with resetting failure seams/recorded calls, matching the TypeScript Map subclass. Calls use the known derived fixture type; no virtual base override contract is lost. Renaming the source-mirrored API is outside the audit.

### `bugprone-empty-catch`

Real but benign in sampled `src/admin/chart_source_proxy.cpp:43` and `src/admin/kernel.cpp:392,409`: optional fallback/JSON parse handling deliberately swallows an error before returning the source-compatible fallback or validation response. The TypeScript's catch-and-continue behavior is the oracle; do not globally convert these catches into request failures.

### `bugprone-exception-copy-constructor-throws`

Real but benign resource-exhaustion risk at `src/api_key_policy.cpp:502`: throwing the named const ApiKeyQuotaDispatchError copies JsError string fields, which may allocate. A failed allocation can replace the intended error; no ordinary request value makes this copy constructor explicitly throw. Minimal improvement is a non-const error moved at its last use, preserving all fields. Treat allocation-failure exception handling as hardening, not a proven request-path memory-safety defect. A second sample at `src/codex/responses_runners.cpp:435` throws an engaged transportFailure error object of its declared type; the same allocation-copy concern applies without evidence of slicing or an input-dependent throwing copy body.

### `bugprone-exception-escape`

Mixed benign resource-exhaustion risk and false positives. ~ScopeExit at `include/uos/rt/async.hpp:564`, instantiated from `src/rt/fetch_transport.cpp:274-277`, has an exported chain reader.cancel -> rejected<void> -> awaitOwned frame allocation -> boost::wrapexcept<std::bad_alloc>. Allocation/scheduling failure there can escape an implicitly noexcept destructor and terminate the process, but no non-allocation exception or ordinary-input failure is established. The exact LLVM 24 source (revision 51d7f08b16b6) explicitly calls Tracer.ignoreBadAlloc(true); its filter recognizes only the std::bad_alloc tag, not Boost's wrapper. Reclassify the initial P2 candidate as real but benign resource-exhaustion hardening, not a confirmed request correctness defect. Local improvement: catch synchronous failure around that specific best-effort cancel/spawn expression after closing the connection, matching its existing asynchronous ignoreRejection. Do not globally swallow ScopeExit callbacks. Other sampled diagnostics are false positives under current type-state invariants: `src/rt/kv_js.cpp:380` checks isString/isNumber/isBigInt/isBool before std::get; `src/rt/kv_value.cpp:358,372` visits valid stored-value variants. A deliberately valueless public raw variant can terminate these noexcept APIs, but no current request path constructing that state was found. The libstdc++ never-valueless trait is false, so this is a caller invariant, not a compiler guarantee.

### `bugprone-float-loop-counter`

Real but benign in `src/analytics.cpp:126` and `src/api_key_policy.cpp:237,310,346,398,669`: integer-valued double retry counters with fixed small limits and increments of one mirror TS Number. Values stay exactly representable, so there is no rounding-driven infinite loop. Do not change arithmetic merely to satisfy a C++ convention. Input-facing CBOR samples at `src/auth/passkeys.cpp:388,404` also remain bounded: decodeLength rejects lengths above 2^53-1 and every iteration consumes bytes or rejects exhausted input. PEM chunking at 2289 iterates over the actual string length. This additional sample does not reveal a rounding-stalled loop.

### `bugprone-forward-declaration-namespace`

False positive at `include/uos/rt/http.hpp:155`: the opaque rt::detail::BodyState is defined in src/rt/http.cpp, which is intentionally a separate TU. The loopback test transport has a different anonymous-namespace BodyState. A same-spelled fixture type is not evidence of a misplaced runtime forward declaration; moving or exposing the private definition would be wrong.

### `bugprone-implicit-widening-of-multiplication-result`

Real but benign at `include/uos/request.hpp:20` and `include/uos/sentinel/upstream_capture.hpp:43`: 32*1024*1024 and 4*1024*1024 fit int before conversion to int64_t. No overflow occurs. Use a suffixed first operand only if changing these constants in future. The dynamic regex offsets at `src/rt/regex.cpp:157,169,170,179,253,254`, regex_internal.hpp:260, and regex_match.cpp use twice a uint32 capture index. The compiler caps captures at 32768 (`src/rt/regex_compile.cpp:105,632`), so these multiplications stay <=65536 before widening. No overflow occurs within that validated bytecode invariant.

### `bugprone-integer-division`

Real but benign assertion at `tests/rt/fs_test.cpp:738`: the case compares floor(mtimeMs) to an integer nanosecond-to-millisecond conversion for a freshly created file's positive timestamp. Truncation is the property being checked. Moving the cast before division would remove that floor check. Negative/extreme timestamps have separate fixtures and are not evidence that this positive-file assertion is wrong.

### `bugprone-macro-parentheses`

False positive at `tests/admin_api_key_audit_test.cpp:113`: AWAIT's expression argument is already parenthesized, while name is a declaration identifier and token-paste operand. Parenthesizing that identifier is not an expression-safety fix. The macro deliberately binds the task/result separately to satisfy the port's coroutine rules.

### `bugprone-misplaced-widening-cast`

Real but benign at `src/rt/websocket_frame.cpp:320`: index ranges from 0 through 7, so 2+index is 2..9 and cannot overflow int before conversion to size_t. The corresponding uint64 shift count is 0..56. No narrowing/truncation or untrusted-length overflow is demonstrated.

### `bugprone-multi-level-implicit-pointer-conversion`

False positive for intended byte-copy API use at `src/rt/gzip.cpp:105`: addresses of function-pointer slots and the dlsym void* value are passed to memcpy's void*/const void* parameters. The code copies representations and never dereferences a converted pointer-to-pointer as an incompatible pointee. Adding explicit void casts would silence syntax without changing behavior; no source fix is needed for this class.

### `bugprone-optional-value-conversion`

Real but benign at `src/admin/sentinel_incident.cpp:152`: limit has already passed validPageLimit and is checked before dereference. Assigning limit directly would be simpler; no unchecked input path or behavioral defect demonstrated.

### `bugprone-random-generator-seed`

Real but benign deterministic property testing at `tests/rt/json_test.cpp:2063`: mt19937(12345) intentionally makes inconsistent-comparator sort cases reproducible. It is not provider/token/session entropy. Keep the fixed seed and all permutation/memory-safety assertions; a cryptographic RNG change would harm reproduction without addressing a runtime defect.

### `bugprone-reserved-identifier`

Real but benign naming concern at `include/uos/provider/stream_relay.hpp:24`: the private parameter return__ contains a reserved double underscore. Renaming that parameter would improve portability, but no request-path lifetime failure is demonstrated and source renames are outside this hazard audit. CERT naming aliases were excluded initially, while bugprone-* independently enables the canonical check; disable the canonical name in the final configuration.

### `bugprone-return-const-ref-from-parameter`

False positive at `include/uos/rt/ordered.hpp:249`: SetKey is an internal projection called on table-owned live entries, not temporaries. Returning the original key reference is intentional; replacing it with a copy is unnecessary. A second sample, `src/auth/passkeys.cpp:1651`, validates and returns requireMap's input; every current caller passes a named decoded CBOR object or a caller-owned key reference, so no temporary escape is demonstrated. `src/provider/claude_code_quota.cpp:293` newerWindow chooses an existing or observed window reference; all four callers at 308,313,562,568 immediately copy it into a value local while the observed/window owners remain alive. This is the same safe selector-reference class.

### `bugprone-signed-bitwise`

Initial sample is benign: `include/uos/rt/number.hpp:85,226` shifts unsigned 64-bit operands by signed positive literal counts 63 and 20; `include/uos/rt/string.hpp:180` shifts an unsigned code point by literal 10. C++ permits these counts. This is not evidence of shifting a negative value. Later arithmetic sites remain to be sampled. Additional passkey CBOR/base64 samples at `src/auth/passkeys.cpp:316,487,488` use nonnegative promoted bytes or unsigned accumulators and positive constant shift counts. No negative left operand or out-of-range shift was demonstrated; literal-count warnings are disproportionately noisy.

### `bugprone-std-exception-baseclass`

Real but benign: `include/uos/rt/async.hpp:1036,1079` and `include/uos/rt/stream.hpp:548` throw the private `GeneratorReturn` token, caught by the generator's exact-type handler to implement iterator return. This is intentional control flow distinct from JavaScript errors; retain behavior rather than changing the exception hierarchy. Additional production samples in `src/provider/anthropic_chat_responses.cpp:429,441`, `anthropic_chat_stream.cpp:214`, and `anthropic_responses.cpp:711,758` are the same GeneratorReturn token propagated to the exact generator handler, not arbitrary non-error exception leakage.

### `bugprone-string-constructor`

Real but benign test input at `tests/images_test.cpp:229`: the 20 MiB string intentionally exceeds the official image_url limit and must be preserved to test rejection before dispatch. `tests/rt/kv_jsc_deserialize_vectors.inc:23,77` uses explicit zero-length string constructors in recorded binary-codec vectors; empty strings/keys are intentional. These are not accidental argument swaps or buffer lengths; do not shrink/delete the test expectations.

### `bugprone-suspicious-include`

Real but benign test construction at `tests/jev_codex_smoke_test.cpp:39` and `tests/jev_compaction_smoke_test.cpp:39`: these standalone test TUs intentionally include the corresponding implementation to exercise private pure smoke helpers. The test link closures account for that source inclusion. This is not an accidental header typo or a first-party runtime include of an implementation. Moving/reformatting those helpers is outside audit ownership.

### `bugprone-suspicious-missing-comma`

False positive at `src/codex/supervisor_brief.cpp:56`: adjacent string literals intentionally continue one prompt paragraph, ending in a space before the next literal; the surrounding vector separates paragraphs with actual commas. Inserting a comma would change the prompt's newline structure. Preserve the source-compatible string.

### `bugprone-suspicious-stringview-data-usage`

False positive at `src/rt/regex.cpp:79`: matcher.search receives bytes_.data() followed by the explicit length_ argument; ASCII input uses equal byte/UTF-16 length and non-ASCII input takes the separate units_ branch. The matcher is length-aware and does not require NUL termination. The heuristic does not recognize the separately stored size member.

### `bugprone-switch-missing-default-case`

Real but benign fixture DSL at `tests/rt/ordered_test.cpp:236`: apply interprets a closed recorded operation alphabet for map/set differential vectors. Unknown opcodes are not request input and the tests' expected rows cover the recorded cases. A default assertion could harden fixture editing; changing runtime behavior or oracle rows is unnecessary.

### `bugprone-throwing-static-initialization`

Real but benign startup risk: fixed inline KvKey/JSON/string constants (for example API_KEY_CHANGE_PREFIX at `include/uos/authority/native_api_key_audit.hpp:13`) allocate before main; malformed request data cannot enter these initializers. Allocation failure can terminate startup, also consistent with a process unable to allocate. Literal regex initialization is separate from request parsing. No request-path exception escape is established by this check (alias cert-err58-cpp).

### `bugprone-unchecked-optional-access`

Initial sample: false positives on runtime invariants at `include/uos/rt/async.hpp:221,377,516` (value follows successful settlement; rejected states rethrow), `include/uos/rt/regex.hpp:52-54` (successful regex always has participating group zero), and `include/uos/rt/nullable.hpp:100` (unchecked interface with caller precondition). `src/admin/capacity_observation_sync.cpp:37,44` follows `!step.done`, which guarantees a chunk. `src/admin/api_keys.cpp:131,140,141,147` follows a validated `name` and checked `quotaPolicy`; `rt::truthy(optional)` is not modeled. `src/admin/api_key_mutations.cpp:87` serializes a constructed KvObject, whose top-level conversion always returns a string or throws, never nullopt. Further production parser sites remain under review; no global safety claim yet. More input-facing samples: `src/authority/native_api_key.cpp:95,103-107` follows isSafeNonNegativeInteger helper predicates; `src/authority/native_api_key_policy.cpp:139-142` follows hasStrictPaidFallbackPolicy, which validates these exact numeric/bool fields. `src/authority/native_authority_api.cpp:148,149` explicitly requires an engaged lifetime and api before the flagged expression. These are unmodeled validation relationships, not new unchecked-request defects.

### `bugprone-unhandled-self-assignment`

False positive at `include/uos/rt/ordered.hpp:182`: copy assignment attaches the incoming table before detaching the outgoing one. On self-assignment this increments then decrements the same iterator count without reclaiming the table. The final table and position remain valid.

### `bugprone-unused-local-non-trivial-variable`

Real but benign fixture dead local at `tests/prompt_cache_telemetry_gate_differential_test.cpp:128`: release is read from a vector but the following test intentionally uses an explicit mixed-case release string. No request behavior or expected assertion depends on this local. Removing unused scaffolding is outside this audit's source ownership; disable this cleanup/style check in the routine configuration.

### `bugprone-unused-return-value`

Real but benign cleanup/optional socket-option handling at `src/rt/fetch_transport.cpp:138` (TCP_NODELAY setup) and `src/rt/fetch_transport_net.cpp:53` (close). Both use the explicit error_code overload with a named ignored variable; connection failure/ownership are handled elsewhere, and teardown is best effort. Do not generalize this verdict to ignored persistence/settlement results. Additional false positives at `src/runtime/http.cpp:1264-1268` are Asio operations whose error_code output parameter is explicitly checked before proceeding; their returned result duplicates that handled error. WebSocket shutdown/close/no-delay calls use explicit best-effort error_code paths as well.

### `bugprone-use-after-move`

False positives in the two runtime JSON sites: `src/rt/json.cpp:116` moves insertion value only when monotonically increasing cursor equals a single fixed position; that branch cannot execute twice for a valid container. At 745, last was moved into a child at 748/757, but every path returning to the enclosing completion loop either assigns last from frame.container before popping the frame, or breaks to parse and assign the next token value. The checker does not preserve these loop/state relationships. No request-input use of a moved-out JSON value is established by these reports; later sites remain individually reviewable.

### `cert-err09-cpp`

Real but benign at `src/api_key_policy.cpp:502`: the named thrown value has the exact ApiKeyQuotaDispatchError type, so no slicing occurs. The related potentially throwing-copy warning is documented separately; naming it is not itself an incorrect exception value. A second sample at `src/codex/responses_runners.cpp:435` throws an engaged transportFailure error object of its declared type; the same allocation-copy concern applies without evidence of slicing or an input-dependent throwing copy body. Remaining production samples construct named errors to add fd/code metadata or close/reset SQLite handles before throwing (`src/rt/fs.cpp:472,623`, `sqlite.cpp:583,638,715`, `websocket_client.cpp:101,105`). They throw the exact declared type; no slicing is demonstrated. Keeping the named value preserves the captured error before cleanup mutates native error state.

### `cert-err33-c`

Initial real but benign best-effort diagnostics: `src/rt/async.cpp:277,278` and `src/rt/clock.cpp:48,51,53` ignore fprintf/fflush results when reporting uncaught errors or Node-compatible timer warnings. Failed stderr logging is not a durable request write and should not become a second thrown error. Further filesystem/SQLite/process return-value warnings will be reviewed separately; this sample does not justify ignoring durability errors. Further benign bounded formatting: fetch_transport.cpp:178 uses a 32-byte buffer for <=16 hex digits plus CRLF; kv_key.cpp:505 uses 24 bytes for 16 hex digits plus four zeros and NUL; tls.cpp:36,41 uses buffers larger than maximal IPv4/group output. process.cpp:108 flushes best effort immediately before _Exit; process_loop.cpp:409 installs SIG_IGN for the fixed valid SIGPIPE signal. No dropped durability-write result was found in these samples.

### `clang-analyzer-core.CallAndMessage`

False positive at `src/codex/supervisor_inventory.cpp:405`: the trace passes null into quotaBucketsOf and then assumes its external isRecord(null) guard succeeds. The defining supervisor_config isRecord pointer overload explicitly rejects null; clang-tidy's per-TU analyzer does not import that function body. The path is infeasible. Separate SDK-primary warnings in Boost.Asio awaitable internals are retained in raw logs but excluded from first-party counts; they are not silently reclassified as gateway defects. Exact guard chain: `src/codex/supervisor_config.cpp:43` delegates to `src/rt/json.cpp:957`, `return value && value->is_object();`. A second traced class at `src/provider/anthropic_chat_responses.cpp:368,369` assumes record(nullptr) is true after a missing choices/delta. Its local record wrapper at line 39 also delegates to rt::isRecord, so the identical null-rejecting guard makes those paths infeasible.

### `clang-analyzer-core.NonNullParamChecker`

False positive at `src/codex/supervisor_inventory.cpp:422`: the trace initializes resetCredits to null and assumes isRecord(resetCredits) is true. supervisor_config::isRecord(const json::value*) returns false on null, so the branch forming *resetCredits cannot execute. This is a cross-TU guard-model limitation, not a request-controlled null dereference. Keep the broad audit evidence even if disabling this noisy check in the default profile. Exact guard chain: `src/codex/supervisor_config.cpp:43` delegates to `src/rt/json.cpp:957`, `return value && value->is_object();`. A second traced class at `src/provider/anthropic_chat_responses.cpp:368,369` assumes record(nullptr) is true after a missing choices/delta. Its local record wrapper at line 39 also delegates to rt::isRecord, so the identical null-rejecting guard makes those paths infeasible. The new `src/provider/metered.cpp:601` envelopeItems warning is also guarded by an isRecord pointer helper; the path must not assume that null is a record. `src/upstream_wire.cpp:236` likewise follows utils::isRecord(value), whose pointer overload rejects nullptr; no null-reference path is established there.

### `clang-analyzer-core.NullDereference`

False positives in the sampled Boost.JSON array range loops at `src/images.cpp:112,410,552`. The latter two explicitly reject a null output array first. Exported traces take a continue branch, then invent a null __begin iterator while assuming it differs from __end, without a source mutation of the container. Valid Boost.JSON begin/end iterators cannot become null between elements this way; empty arrays have begin == end and do not enter the loop. The first trace even enters an untouched default-empty references array. This is an iterator-model limitation, not a proven malformed-image-input dereference. Later null diagnostics remain under review; no blanket safety claim.

### `clang-analyzer-core.uninitialized.Assign`

False positive in `tests/storage_native_kv_differential_test.cpp:63-67`, reported at `include/uos/rt/kv.hpp:567`: take moves a live Step variant out of deque.front, pops the old entry, then copies its KvCommitResult alternative into optional. KvCommitResult::ok has a false default initializer and every scripted result is constructed with an initialized bool. The trace loses the field value through deque/variant movement; it does not identify a source construction leaving ok uninitialized. No source fix proposed.

### `clang-analyzer-cplusplus.NewDeleteLeaks`

False positives at `src/runtime/http.cpp:1878,1879`: both traces stop after std::function copies allocate their heap functor. IncomingListener immediately owns that callable through shared_ptr<function<...>>; listener copies share ownership and std::function's manager destroys the functor on final release. The listeners are also copied into once registrations and the owning handleRequest frame, with explicit incoming listener removal at its end. No raw allocation is dropped at these construction sites. This does not assert that every unrelated callback retention cycle is absent.

### `clang-analyzer-cplusplus.StringChecker`

Fixture-only OS-setup hardening at `tests/rt/env_test.cpp:189`: the case first sets UOS_RT_ENV_TEST_INITIAL to from-os, then verifies rt::env's snapshot changes do not mutate native getenv. That invariant holds after successful setenv in this single-threaded case; however, native setup failure could leave getenv null before std::string construction. No ordinary request path is involved. Minimal owner improvement: check the native setenv result and ASSERT a nonnull getenv pointer before constructing the expected string; preserve the from-os assertion.

### `clang-analyzer-security.ArrayBound`

False positives under the current WebSocket state invariant at `src/rt/websocket.cpp:228`: the private notOpenMessage helper indexes READY_STATES using the internal readyState. Its only call passes state.readyState; all production assignments in websocket.cpp/internal.hpp use CONNECTING, OPEN, CLOSING, CLOSED (0..3). The analyzer treats the helper's integer as unconstrained and reports both negative and >=4 paths. This is not a wire-controlled arbitrary index. A C++ extension that corrupts the exposed internal state would violate the library precondition.

### `clang-analyzer-security.FloatLoopCounter`

Same benign bounded-double-counter class as bugprone-float-loop-counter; sampled `src/analytics.cpp:126` and API-key retry loops. No unbounded or fractional-step request-controlled counter was found in this sample. Input-facing CBOR samples at `src/auth/passkeys.cpp:388,404` also remain bounded: decodeLength rejects lengths above 2^53-1 and every iteration consumes bytes or rejects exhausted input. PEM chunking at 2289 iterates over the actual string length. This additional sample does not reveal a rounding-stalled loop.

### `clang-analyzer-unix.StdCLibraryFunctions`

Real but benign fixture resource-failure path at `tests/rt/console_test.cpp:49,53`: captureFd assumes dup(fd) succeeds; descriptor exhaustion could produce saved == -1 and make restoration's dup2 invalid. Minimal owner improvement: check pipe/dup/dup2 setup and restore/close through a guard, failing the fixture on setup failure. No request-path API receives an unchecked descriptor in this report. This is unranked test-harness hardening, not a reproduced runtime failure.

### `concurrency-mt-unsafe`

Real API constraints but benign current uses: `src/rt/fs.cpp:348,365` uses readdir only on a fresh function-local DIR handle, consuming each entry before the next read; no handle is shared between concurrent calls. `src/rt/gzip.cpp:123` uses dlerror in synchronous loader error handling; glibc's error slot is thread-local and the gateway loop is single-threaded. The checker names APIs without proving a shared-handle or global-state race. Threaded SQLite workers do not share these DIR objects. `src/rt/process_loop.cpp:200` reads getenv(PATH) synchronously; gateway environment mutations and process preparation run on the single loop, and SQLite workers do not mutate that environment. A future multi-threaded environment mutator would require synchronization; this audit does not qualify such a use.

### `cppcoreguidelines-avoid-capturing-lambda-coroutines`

Mixed: confirmed P1 defect at `src/admin/kernel.cpp:130,134`, plus legitimately retained callbacks. The complete one-line site verdict table below is the authoritative maintainer list. The checker detects syntax and cannot recognize rt::async, generator body retention, Promise callback keepAlive, SourceAlgorithm, or owning callback parameters; capture-by-value alone is not sufficient. No source fixes applied.

### `cppcoreguidelines-avoid-reference-coroutine-parameters`

Initial sample is real but benign borrowing under structured await. `src/cache/kv_migration.cpp:37` borrows progress from the migration caller's live frame; the sole caller starts a Promise and awaits its completion at 161-163. Private collectors at `src/cache/kv_migration_paid_fallback.cpp:143,169,194,216,235` borrow knownKeyIds/errors from inspectPaidFallbackV3; callers at 445-452 await each task sequentially and keep all referents in the parent frame. No task is detached or raced in these calls, so ordinary failure destroys the active child before its parent state. Prefer owning state for APIs that can escape; these internal helpers do not establish a UAF. Further reported coroutine-reference sites remain to be classified. Further structured-borrow samples: migration projection repairOnePaidFallbackWindow at `src/cache/kv_migration_projection.cpp:484` is immediately awaited at 535 with a named local context; usage collectStrictApiKeyPairs at `src/cache/kv_migration_usage.cpp:716` is awaited at 882 while hashEntries/errors remain in scope. Migration validation auditApiKeyBoundedAggregates at 151 and auditApiKeyPolicy at 169 are similarly sequential. Catalog store APIs explicitly require the Kv object to outlive their tasks/heartbeat (PORT-NOTES); their reference signatures are an API precondition, not automatic evidence that current callers violate it. Request-path KV-reference samples in `src/embeddings/voyage.cpp:55,144,258,272` and `src/embeddings/jobs.cpp:83` are awaited by handlers/jobs that retain the owning shared_ptr in kv or params.kv; child failures are observed before that owner leaves scope. This establishes current-call lifetime, not safety for arbitrary detached calls with a short-lived Kv.

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

### `performance-string-view-conversions`

Real but benign small allocation at `src/deepseek/waterfall_handler.cpp:116`: a constant provider string_view is converted to std::string and immediately viewed again by isProviderEnabled. Pass the constant directly to the synchronous view-taking API to remove the copy; do not apply that rewrite to views that cross coroutine suspension. No measured latency regression is established.

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
| `include/uos/codex/supervisor_config.hpp:256` | Real but benign: each concurrency worker uses rt::async and owns shared State; sibling rejection cannot free another worker's closure or items/run callback. |
| `include/uos/kv_conversion/convert.hpp:68` | Real but benign: erased SnapshotRead is moved into the by-value snapshot-operation parameter, which awaits it; read and shared result remain owned until completion. |
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
| `src/codex/routing_serial.cpp:303` | Real but benign: withCodexAdmission owns the action parameter and waits for its Promise before releasing admission and returning. |
| `src/handler/terminal_log.cpp:481` | Real but benign: the local owner coroutine is passed to rt::async before constructing its running Promise, retaining the state-owning closure for the buffered response. |
| `src/kernel/quota_reservations.cpp:227` | Real but benign: withKernelQuotaLock owns operation by value and awaits it through success/error settlement; all captures are values. |
| `src/kernel/quota_reservations.cpp:282` | Real but benign: settlement operation is moved into withKernelQuotaLock's owning parameter and completes before that frame releases it. |
| `src/kernel/quota_reservations.cpp:586` | Real but benign: scope-policy cleanup operation owns copied target fields/KV and is retained by withKernelQuotaLock while awaited. |
| `src/kernel/quota_reserve.cpp:241` | Real but benign: withKernelQuotaLock owns the copied reservation operation/options across all retry awaits. |
| `src/kernel/quota_write.cpp:186` | Real but benign: policy-write closure owns request/keys/KV and is moved into the awaited lock helper's by-value operation parameter. |
| `src/kernel/quota_write.cpp:282` | Real but benign: policy-delete closure owns scope/owner/repo/keys and is retained in the awaited lock helper. |
| `src/kv_conversion/profile_json.cpp:373` | Real but benign: SourceAlgorithm::runAsync retains the pull callable and its shared iterator until the pull settles, including close clearing algorithms. |
| `src/kv_conversion/profile_json.cpp:383` | Real but benign: SourceAlgorithm owns the cancel callable through awaited iterator.return_; clearing the stream's algorithms cannot destroy an active invocation. |
| `src/provider/claude_code_usage_refresh.cpp:43` | Real but benign: work is wrapped in rt::async and then awaited inside withFinally, retaining the owning closure/KV/binding/options across refresh suspension. |
| `src/provider/claude_code_usage_refresh.cpp:74` | Real but benign: withFinally copies cleanup into its by-value parameter and awaits it after work settles, retaining key/owner/KV until cleanup completes. |
| `src/responses_attempts.cpp:54` | Real but benign: ResponsesStreamIterator/AsyncGenerator owns the sanitizing coroutine body and captured iterator through yields and error cleanup. |
| `src/responses_buffered.cpp:78` | Real but benign for the closure: AsyncGenerator retains copied prepared/options for the complete buffered iteration; pointer-valued options retain their separate caller lifetime contract. |
| `src/responses_handler.cpp:239` | Real but benign: fire-and-forget probe cleanup explicitly uses rt::async before spawn, retaining the delivery/type closure through mark/release awaits. |
| `src/sentinel/upstream_capture.cpp:580` | Real but benign: SourceAlgorithm retains the pull callable and its copied attempt/reader/finish/retain callbacks across reads, including paths that close or error the stream. |
| `src/storage/kv.cpp:129` | Real but benign: the open operation is moved into retrySqliteBusy's by-value callback parameter, which retains it across retries and awaits. |

### Tests and support

| Site | Verdict |
| --- | --- |
| `tests/admin_codex_coverage_test.cpp:103` | Real but benign fixture: reader is copied into the awaited handler's by-value parameter; its captured diagnostic string remains owned through the expected rejection. |
| `tests/admin_codex_scope_experiment_test.cpp:81` | Real but benign fixture: beforeGet is stored on the live fixture KV; the awaited handler consumes the throwing callback before fixture teardown. |
| `tests/admin_codex_scope_experiment_test.cpp:85` | Real but benign fixture: the local Fetch/GlobalFetchOverride retains the closure; referenced resolvers/call counter stay in the ASYNC_TEST frame until the first pending request is released and awaited. |
| `tests/admin_native_owner_test.cpp:108` | Real but benign fixture: installed readAuth hook is retained until awaited calls finish; Fixture and its nativeReads counter outlive the callback, and teardown removes the hook. |
| `tests/admin_native_owner_test.cpp:109` | Real but benign fixture: refresh hook is retained while the Fixture/nativeRefreshes counter lives, throws synchronously on execution, and is removed at teardown. |
| `tests/api_key_policy_differential_test.cpp:223` | Real but benign fixture: rt::async owns copied store/time/token inputs for authentication. |
| `tests/api_key_policy_differential_test.cpp:289` | Real but benign fixture: rt::async owns the closure/KV/policy; referenced reservations belongs to the enclosing test, which awaits the reserve/replay work. |
| `tests/api_key_policy_differential_test.cpp:303` | Real but benign fixture: rt::async retains name/work; referenced record callback remains in the parent frame until the awaited replay completes. |
| `tests/api_key_policy_differential_test.cpp:325` | Real but benign fixture: rt::async owns a copied deferred reservation through its awaited dispatch. |
| `tests/api_key_policy_differential_test.cpp:336` | Real but benign fixture: rt::async owns a copied deferred reservation through its awaited release. |
| `tests/api_key_policy_differential_test.cpp:353` | Real but benign fixture: rt::async retains copied policy/store while reading usage. |
| `tests/api_key_policy_differential_test.cpp:366` | Real but benign fixture: rt::async retains the copied reservation through dispatch/marking. |
| `tests/api_key_policy_differential_test.cpp:384` | Real but benign fixture: rt::async retains the KV/time values through the live-reservation check. |
| `tests/api_key_policy_differential_test.cpp:400` | Real but benign fixture: rt::async retains the shared KV through awaited reclamation. |
| `tests/api_key_policy_differential_test.cpp:414` | Real but benign fixture: rt::async retains the KV/policy through window reclamation. |
| `tests/api_key_policy_differential_test.cpp:464` | Real but benign fixture: rt::async retains the closure; referenced authenticate/KV locals remain in the enclosing test while both sequential calls and replay are awaited. |
| `tests/api_key_policy_differential_test.cpp:476` | Real but benign fixture: rt::async retains the shared KV through awaited usage deletion. |
| `tests/bounded_response_body_test.cpp:184` | Real but benign fixture: SourceAlgorithm retains the cancellation coroutine while its Promise is stalled; copied Promise/shared counters remain owned independently of the test's stream handle. |
| `tests/c1_provider_handlers_differential_test.cpp:104` | Real but benign fixture: registry owns the outer callback and blockOn awaits rt::async's owned row-capturing coroutine to completion on the local loop. |
| `tests/chat_completions_differential_test.cpp:47` | Real but benign fixture: AsyncGenerator owns copied events/input/abort controllers for the full simulated stream. |
| `tests/chat_completions_differential_test.cpp:56` | Real but benign fixture: cancel is copied into the retained source; execute and the owning completion/stream adapter keep it alive through awaited consumption/cancellation. |
| `tests/chat_completions_differential_test.cpp:129` | Real but benign fixture: AsyncGenerator owns remaining/finished; its scope guard only increments the owned completion counter. |
| `tests/claude_capacity_analytics_test.cpp:84` | Real but benign fixture: fetcher/configured options remain in the loop's parent frame while refresh is awaited; KV/account/change captures are owned values. |
| `tests/claude_code_accounts_test.cpp:424` | Real but benign fixture: the provider-dispatch seam explicitly wraps its owning store/counter closure in rt::async. |
| `tests/claude_header_meter_test.cpp:131` | Real but benign fixture: server ServeState owns the handler and is retained by handleRequest while awaiting it; captured reply state is a shared owner. |
| `tests/codex_account_routing_part2_test.cpp:264` | Real but benign fixture: every parallel probe passes attempt through rt::async before constructing its Promise, retaining the copied expiry closure after the loop iteration. |
| `tests/codex_account_routing_part3_test.cpp:95` | Real but benign fixture: returned rt::Fetch owns the marker/dispatch callable; its fetch invocation retains that callable through the awaited cloned body read. |
| `tests/codex_auth_cache_part1_test.cpp:92` | Real but benign fixture: SourceAlgorithm owns the closure; cancellationStarted is accessed only before the permanent suspension, and the pending remainder never reads that reference. |
| `tests/codex_auth_cache_part1_test.cpp:210` | Real but benign fixture: Fetch::keepAlive retains the callback, and the parent awaits all requests while providerCalls remains in scope. |
| `tests/codex_auth_cache_part1_test.cpp:498` | Real but benign fixture: Fetch::keepAlive retains the gated callback; referenced gates/counters remain in the parent until requests are released/awaited, with cleanup releasing gates. |
| `tests/codex_auth_cache_part2_test.cpp:173` | Real but benign fixture: Fetch::keepAlive owns the coroutine callback; the test retains its counters/status through the awaited outer and nested requests. |
| `tests/codex_auth_cache_part3_test.cpp:90` | Real but benign fixture: beforeDispatch is retained in options/request state; its referenced counters/KV remain live until the awaited fenced dispatch completes. |
| `tests/codex_auth_cache_part3_test.cpp:179` | Real but benign fixture: the scripted reset provider retains onInventory and its copied rotated account while the awaited reset/inventory call runs. |
| `tests/codex_banked_reset_part2_test.cpp:243` | Real but benign fixture: rt::async owns the inner callback independently of the replacement slot it clears/restores; referenced Fixture/gates remain live until both pool requests are released and awaited. |
| `tests/codex_banked_reset_part2_test.cpp:277` | Real but benign fixture: rt::async owns the key-capturing child; referenced settings/KV state stays in the parent while duplicate evaluation is awaited. |
| `tests/codex_banked_reset_provider_test.cpp:198` | Real but benign fixture: named body is passed to the awaited step helper by value, retaining the copied case through redemption checks. |
| `tests/codex_banked_reset_provider_test.cpp:228` | Real but benign fixture: the awaited step helper retains body and its copied response until redemption completes. |
| `tests/codex_banked_reset_provider_test.cpp:281` | Real but benign fixture: the awaited step helper retains the body/payload through the expected inventory rejection. |
| `tests/codex_catalog_test.cpp:443` | Real but benign fixture: Fetch::keepAlive retains the callback; counter/Promise references are used while the test is live, and its pending refresh is released before case completion. |
| `tests/codex_catalog_test.cpp:444` | Real but benign fixture: Fetch::keepAlive retains the callback; the referenced surplus gate is released in the parent case and no reference is accessed after its wait. |
| `tests/codex_manual_reset_test.cpp:57` | Real but benign fixture: return conversion builds an owning rt::Fetch, whose keepAlive retains the copied underlying fetcher/modifier across both awaits. |
| `tests/codex_manual_reset_test.cpp:104` | Real but benign fixture: copied reset dependencies retain the callback and its shared fixture/fetcher through the awaited preflight mutation and request. |
| `tests/codex_manual_reset_test.cpp:133` | Real but benign fixture: retained reset callback owns its fixture/fetcher across the post-consumption KV write and awaited reset outcome. |
| `tests/codex_responses_error_test.cpp:45` | Real but benign fixture: ResponsesStreamIterator/AsyncGenerator owns the events-capturing body through every yield. |
| `tests/codex_serial_routing_http_test.cpp:95` | Real but benign fixture: ServeState retains the handler; the captured Fixture stays alive through awaited HTTP calls and shuts down its servers during teardown. |
| `tests/codex_serial_routing_http_test.cpp:105` | Real but benign fixture: rt::Fetch keeps the copied providerOrigin closure alive across the forwarded loopback fetch. |
| `tests/codex_supervisor_config_differential_test.cpp:302` | Real but benign fixture: mapWithConcurrency owns run, and the parent awaits every successful worker before active/peak/order leave scope. |
| `tests/codex_supervisor_config_differential_test.cpp:323` | Real but benign fixture: workers retain run; after fail-fast rejection the test explicitly sleeps 20 ms and checks all remaining workers finished before referenced vectors leave scope. |
| `tests/codex_supervisor_handlers_differential_test.cpp:300` | Real but benign fixture: the named sessions closure and referenced check/root locals stay in the parent frame across each sequential direct await. |
| `tests/codex_supervisor_handlers_differential_test.cpp:307` | Real but benign fixture: named output closure is awaited sequentially before the parent destroys its referenced checker/root state. |
| `tests/codex_supervisor_inventory_coverage_test.cpp:245` | Real but benign fixture: mapWithConcurrency retains run and the parent awaits all successful workers before active/peak leave scope. |
| `tests/compaction_summary_forwarding_test.cpp:71` | Real but benign fixture: Fetch::keepAlive owns the throwing no-dispatch sentinel; its referenced counter lives through the awaited handler. |
| `tests/decisions_test.cpp:221` | Real but benign fixture: deps' rt::Fetch retains the aborting callback; the parent controller remains alive until the awaited bounded request settles. |
| `tests/embeddings_ledger_differential_test.cpp:291` | Real but benign fixture: named acquire closure stays in the parent; each step is awaited while referenced KV/lease map/recorder remain live. |
| `tests/embeddings_ledger_differential_test.cpp:312` | Real but benign fixture: named dispatch closure is awaited before its parent lease map/recorder leave scope. |
| `tests/embeddings_ledger_differential_test.cpp:317` | Real but benign fixture: named release closure and referenced lease/record state remain live through each awaited step. |
| `tests/embeddings_ledger_differential_test.cpp:322` | Real but benign fixture: named indeterminate closure is awaited while parent lease map/recorder persist. |
| `tests/embeddings_ledger_differential_test.cpp:327` | Real but benign fixture: named storeSuccess closure is awaited; response is a by-value coroutine parameter and referenced recorder/leases remain live. |
| `tests/embeddings_ledger_differential_test.cpp:332` | Real but benign fixture: named seed closure remains in the parent during awaited KV writes; key/value coroutine parameters own their data. |
| `tests/failover_stress_http_test.cpp:101` | Real but benign fixture: ServeState retains the handler; Fixture remains alive while barriers are released, calls are awaited, and server shutdown drains requests. |
| `tests/failover_stress_http_test.cpp:115` | Real but benign fixture: rt::Fetch retains the copied loopback origin across the forwarded fetch coroutine. |
| `tests/helpers/codex_account_routing_harness.cpp:128` | Real but benign fixture: each loop-local identify closure is copied into rt::async, so it survives the iteration and parallel hashing. |
| `tests/helpers/codex_auth_cache_harness.cpp:67` | Real but benign fixture: Promise::then retains hooks through its returned Task; current callers retain the AuthKv owner while getMany is pending, satisfying the captured-this lifetime. |
| `tests/helpers/embeddings_harness.hpp:66` | Real but benign fixture: makeFetchMock's owning rt::Fetch callback retains the adapted handler while the forwarded async task runs. |
| `tests/helpers/live_relay_harness.hpp:71` | Real but benign fixture: Fetch::keepAlive retains the callback; Fixture member accesses occur before the delegated wait, and current test calls keep the Fixture live when starting it. |
| `tests/helpers/loopback_http.cpp:246` | Real but benign fixture: SourceAlgorithm retains the pull callable and shared body/connection state through nextChunk, including a close that clears stream algorithms. |
| `tests/helpers/openai_compat_harness.cpp:100` | Real but benign fixture: rt::Fetch keeps the copied FetchHandler closure alive while the delegated Task/Promise settles. |
| `tests/helpers/responses_conformance_client.cpp:77` | Real but benign fixture: the detached read body is explicitly moved into rt::async before spawn; all capture/journal/response state is owned by value. |
| `tests/helpers/responses_conformance_fixture.cpp:116` | Real but benign fixture: ConformanceMock retains stop; its caller keeps the mock through awaited close and allSettled task drain, and the closure owns shared state. |
| `tests/helpers/responses_conformance_fixture.cpp:178` | Real but benign fixture: createServeHandler copies the delivery-aware callback into retained server/request state, owning the shared Fixture through both awaits. |
| `tests/images_test.cpp:110` | Real but benign fixture: handler options retain dispatch, and the parent awaits the generation before referenced seen leaves scope. |
| `tests/images_test.cpp:124` | Real but benign fixture: owned dispatch options and awaited handleImages keep the callback/nested reference live through body parsing. |
| `tests/images_test.cpp:133` | Real but benign fixture: dispatch is retained in handler options and the parent awaits edits before nested leaves scope. |
| `tests/images_test.cpp:196` | Real but benign fixture: retained dispatch options are awaited through the edit result while the nested vector remains live. |
| `tests/images_test.cpp:235` | Real but benign fixture: dispatch options retain the closure; the first gate is released and pending fan-out is awaited before seen/resolvers leave scope. |
| `tests/images_test.cpp:250` | Real but benign fixture: handleImages aborts and allSettled-joins children at src/images.cpp:465 before propagating failure, keeping referenced guard flags/counter live. |
| `tests/images_test.cpp:287` | Real but benign fixture: coordinator retains before-dispatch; the test resolves the gate and awaits leader/follower/cancel operations while referenced counters remain live. |
| `tests/images_test.cpp:314` | Real but benign fixture: dispatch options and the parent generation await retain the callback and seen vector. |
| `tests/images_test.cpp:386` | Real but benign fixture: each table iteration awaits handleImages/body/error completion; child work is joined while row/calls/seen references remain live. |
| `tests/jev_compaction_fallback_quota_test.cpp:125` | Real but benign fixture: Fetch::keepAlive retains the weak-capturing closure; it locks a strong current owner into a coroutine local before any suspension. |
| `tests/kernel_coverage_test.cpp:320` | Real but benign fixture: named failureMessage closure and referenced keys/base values remain in the parent while each attestation check is awaited. |
| `tests/kernel_coverage_test.cpp:338` | Real but benign fixture: rt::async owns argument copies; referenced failureMessage stays alive in the parent until the wrapped check completes. |
| `tests/kv_budget_part1_test.cpp:112` | Real but benign fixture: the named waitForHealth closure is awaited within the parent case while healthKey/fixture references remain live. |
| `tests/kv_consistent_snapshot_test.cpp:133` | Real but benign fixture: snapshot operation owns its read callback by value; the shared call counter stays owned through the expected rejection. |
| `tests/kv_consistent_snapshot_test.cpp:168` | Real but benign fixture: snapshot operation retains the source-capturing callback until it returns, and source is an owned string capture. |
| `tests/kv_durable_interop_test.cpp:358` | Real but benign fixture: blockOn awaits rt::async, which owns the copied file/TTL closure through the KV writes and close. |
| `tests/kv_durable_suite_test.cpp:573` | Real but benign fixture: named writer is awaited with Asio operator&&, which joins/cancels both operations before the parent KV owners leave scope. |
| `tests/kv_durable_suite_test.cpp:580` | Real but benign fixture: named committer is joined by Asio operator&&; expected type errors are caught while referenced KV/fences/outcomes remain live. |
| `tests/kv_durable_suite_test.cpp:640` | Real but benign fixture: named commitChecks closure is directly awaited while its parent DurableKv remains live; argument data is owned by value. |
| `tests/kv_durable_suite_test.cpp:679` | Real but benign fixture: named commitOf is directly awaited; temporary build callbacks live through the co_await full expression and are consumed before commit suspension. |
| `tests/kv_durable_suite_test.cpp:790` | Real but benign fixture: named checkAbsent is directly awaited inside the parent KV/keyOf scope. |
| `tests/kv_durable_suite_test.cpp:795` | Real but benign fixture: named sumAt is directly awaited while parent KV/keyOf live. |
| `tests/kv_durable_suite_test.cpp:800` | Real but benign fixture: named listPrefix is directly awaited; its iterator is a coroutine local and parent KV/keyOf remain live. |
| `tests/kv_durable_suite_test.cpp:804` | Real but benign fixture: named listRange is directly awaited while parent KV/keyOf remain live. |
| `tests/kv_durable_suite_test.cpp:846` | Real but benign fixture: named payload helper is awaited before parent KV/keyOf leave scope; scalar/optional arguments own their values. |
| `tests/kv_durable_suite_test.cpp:875` | Real but benign fixture: named attempt is directly awaited; build's temporary lives through the complete await expression and is used before suspension. |
| `tests/kv_durable_suite_test.cpp:910` | Real but benign fixture: named page helper is awaited in the parent KV scope; selector/options and iterator are owned by its frame. |
| `tests/kv_durable_suite_test.cpp:1033` | Real but benign fixture: named rest helper is awaited while KV lives, and cursor text/iterator are owned by value. |
| `tests/kv_durable_suite_test.cpp:1076` | Real but benign fixture: named roundTrip is directly awaited while KV lives; input KvValue is a by-value coroutine parameter. |
| `tests/lithos_branch_coverage_test.cpp:76` | Real but benign fixture: MeteredFetch is an owning rt::Fetch and retains the copied rejection message until the callback throws. |
| `tests/lithos_branch_coverage_test.cpp:533` | Real but benign fixture: the owning Fetch retains copied controller/reason values through the immediate abort/rejection. |
| `tests/lithos_branch_coverage_test.cpp:707` | Real but benign fixture: owning MeteredFetch retains shared call count/gate state; the test releases and awaits stale discovery before teardown. |
| `tests/lithos_branch_coverage_test.cpp:735` | Real but benign fixture: owning Fetch retains the shared counter through its immediate rejection, including replacement of the global fetch slot. |
| `tests/lithos_tier_windows_test.cpp:146` | Real but benign fixture: withHttpFixture owns run by value and awaits it; the parent nowMs reference stays live until helper cleanup returns. |
| `tests/lithos_tier_windows_test.cpp:175` | Real but benign fixture: withHttpFixture retains the copied chat-mode closure and awaits it before server shutdown. |
| `tests/lithos_wiring_test.cpp:126` | Real but benign fixture: Fetch::keepAlive retains the callback; referenced counters/signals remain live while the enclosing request/deadline case is awaited. |
| `tests/live_calls_http_test.cpp:61` | Real but benign fixture: retained ServeState handler borrows HttpFixture, which remains in scope through the awaited refresh HTTP call and cleanup. |
| `tests/live_calls_http_test.cpp:66` | Real but benign fixture: server owns the callback and the parent HttpFixture remains live through upstream request/body processing. |
| `tests/live_calls_http_test.cpp:72` | Real but benign fixture: Fetch::keepAlive owns the copied refreshUrl closure through the forwarded loopback request. |
| `tests/live_calls_http_test.cpp:151` | Real but benign fixture: refresh hook is stored on the live Fixture and its account-mutation/request path is awaited before the case ends. |
| `tests/live_calls_http_test.cpp:160` | Real but benign fixture: responder stays on the live fixture for the awaited differential request; fixture reference is used before any suspension. |
| `tests/live_sideband_http_test.cpp:96` | Real but benign fixture: server retains the upgrade closure; current cases keep SocketFixture alive through the controlled handshake and connection cleanup. |
| `tests/live_sideband_http_test.cpp:108` | Real but benign fixture: gateway retains the upgrade wrapper; SocketFixture remains in scope through awaited upgrade/relay completion. |
| `tests/live_sideband_http_test.cpp:215` | Real but benign fixture: installed KV opener retains shared failure-state/store captures; it returns synchronously on coroutine execution and is reset with the fixture. |
| `tests/local_development_key_test.cpp:150` | Real but benign fixture: policy initialization explicitly returns rt::async, retaining shared KV/observation state through writes and reads. |
| `tests/metered_quota_test.cpp:546` | Real but benign fixture: Fetch::keepAlive retains copied gate/base fetcher across the gated forward. |
| `tests/metered_quota_test.cpp:587` | Real but benign fixture: Fetch::keepAlive retains copied start resolver/gate/base fetcher until the forwarded request settles. |
| `tests/metered_test.cpp:276` | Real but benign fixture: owning MeteredFetch retains shared observation state while its signal-driven Promise is awaited. |
| `tests/metered_test.cpp:381` | Real but benign fixture: owning MeteredFetch retains the shared observation state through the abort-driven request. |
| `tests/models_metadata_differential_test.cpp:122` | Real but benign fixture: options' owning rt::Fetch retains the copied model payload through the awaited metadata refresh. |
| `tests/openai_compat_part11_test.cpp:182` | Real but benign fixture: paidAttempt retains the responder through the owning fetch mock/request; the loop's copied index remains owned across the abort Promise. |
| `tests/openai_telemetry_differential_test.cpp:531` | Real but benign fixture: runWithResponseTelemetry owns the run callback while awaited; its scalar/optional captures and UsageContext parameter are copied by value. |
| `tests/openrouter_models_test.cpp:155` | Real but benign fixture: owning OpenRouterFetch retains the callback; mode/counter are read while parent scope lives, before any suspension. |
| `tests/openrouter_models_test.cpp:190` | Real but benign fixture: owning OpenRouterFetch retains the rejection callback; calls is incremented during the awaited force refresh before scope exit. |
| `tests/openrouter_models_test.cpp:210` | Real but benign fixture: Fetch::keepAlive retains the sleeping callback; calls is touched before suspension and the test awaits coalesced refreshes. |
| `tests/oss_gateway_http_test.cpp:89` | Real but benign fixture: ServeState retains the handler and the parent fixture stays live through awaited HTTP calls and producer/server cleanup. |
| `tests/oss_gateway_http_test.cpp:97` | Real but benign fixture: SourceAlgorithm retains the async start callable; its gate/cancelled state and controller parameter are owned values across suspension. |
| `tests/oss_gateway_http_test.cpp:108` | Real but benign fixture: Fetch::keepAlive retains the copied loopback origin while forwarding the request. |
| `tests/oss_stream_lifecycle_test.cpp:180` | Real but benign fixture: SourceAlgorithm retains the pull closure and copied shared state/producer through its awaited production step. |
| `tests/oss_stream_lifecycle_test.cpp:426` | Real but benign fixture: AsyncGenerator owns shared produced/released counters throughout yield and return cleanup. |
| `tests/oss_stream_lifecycle_test.cpp:535` | Real but benign fixture: forAwait owns the forward callback and its shared state/Yield captures through delegation. |
| `tests/oss_stream_lifecycle_test.cpp:606` | Real but benign fixture: consume is copied into rt::async; cleanup gate is released and output is awaited before the parent completes. |
| `tests/oss_telemetry_overhead_test.cpp:528` | Real but benign fixture: terminal options retain the callback and the wrapped result waits for the inline sink before sinkResults leaves scope. |
| `tests/oss_telemetry_overhead_test.cpp:582` | Real but benign fixture: queue owns write and the test awaits queue.close before sinkResults leaves scope. |
| `tests/oss_telemetry_overhead_test.cpp:591` | Real but benign fixture: retained terminal callback is awaited for enqueue; referenced queuedResults remains live through wrapped-response completion. |
| `tests/oss_telemetry_overhead_test.cpp:695` | Real but benign fixture: terminal callback and queue flush complete while handlerAnalytics remains in the parent scope. |
| `tests/paid_fallback_index_local_development_key_differential_test.cpp:295` | Real but benign fixture: rt::async owns request; referenced reservations remains live through the awaited reserve/settle step. |
| `tests/paid_fallback_index_local_development_key_differential_test.cpp:377` | Real but benign fixture: rt::async retains the copied reservation during upstream-response recording. |
| `tests/paid_fallback_index_local_development_key_differential_test.cpp:389` | Real but benign fixture: rt::async owns the copied reservation during awaited terminal recording. |
| `tests/paid_fallback_index_local_development_key_differential_test.cpp:400` | Real but benign fixture: rt::async owns reservation/settlement/serialized-usage values through accounting. |
| `tests/paid_fallback_index_local_development_key_differential_test.cpp:440` | Real but benign fixture: rt::async retains the copied reservation through the deliberately invalid settlement-id check. |
| `tests/paid_fallback_index_local_development_key_differential_test.cpp:461` | Real but benign fixture: rt::async owns the optional reservation through cancellation and output conversion. |
| `tests/paid_fallback_index_local_development_key_differential_test.cpp:484` | Real but benign fixture: rt::async owns the copied reservation through ambiguous-failure recording. |
| `tests/paid_fallback_index_local_development_key_differential_test.cpp:496` | Real but benign fixture: rt::async owns the copied reservation through prefetch cancellation. |
| `tests/paid_fallback_index_local_development_key_differential_test.cpp:634` | Real but benign fixture: rt::async retains shared target/options while ensuring the local key. |
| `tests/paid_fallback_index_local_development_key_differential_test.cpp:641` | Real but benign fixture: rt::async retains the shared target through policy resolution. |
| `tests/passport_differential_test.cpp:87` | Real but benign fixture: rt::Fetch owns copied issuer plan/log state across request-body reads. |
| `tests/passport_sessions_test.cpp:154` | Real but benign fixture: Fetch::keepAlive retains shared session state while reading the request body. |
| `tests/passport_sessions_test.cpp:322` | Real but benign fixture: replacement Fetch retains shared release/reached resolvers across the delayed response. |
| `tests/passport_test.cpp:103` | Real but benign fixture: owning Fetch retains its callback; current requests are awaited while the captured Fixture remains live. |
| `tests/prompt_cache_analytics_admin_test.cpp:60` | Real but benign fixture: awaited admin handler owns refusedReader; referenced unexpectedReads stays in the parent through invalid-input checks. |
| `tests/prompt_cache_analytics_admin_test.cpp:68` | Real but benign fixture: awaited admin handler retains the copied diagnostic-string callback through expected rejection. |
| `tests/prompt_cache_scope_experiment_part1_test.cpp:124` | Real but benign fixture: Fetch::keepAlive retains the callback; the test releases and awaits its first request while referenced counters/resolvers remain live. |
| `tests/prompt_cache_telemetry_gate_differential_test.cpp:60` | Real but benign fixture: atomic operation retains the commit callable; the test retains its KV object through awaited counter commits, satisfying captured-this lifetime. |
| `tests/provider_capacity_differential_test.cpp:134` | Real but benign fixture: owning Fetch retains copied shared FetchState across request processing. |
| `tests/provider_capacity_differential_test.cpp:248` | Real but benign fixture: named reset closure is awaited sequentially while parent database/KV state remains live. |
| `tests/provider_capacity_differential_test.cpp:290` | Real but benign fixture: named viewStep is directly awaited; referenced checker/state and literal-backed label remain live. |
| `tests/provider_capacity_differential_test.cpp:306` | Real but benign fixture: named voidStep is directly awaited while parent checker/state persist. |
| `tests/provider_capacity_differential_test.cpp:310` | Real but benign fixture: named responseStep is awaited through response parsing before parent checker/state leave scope. |
| `tests/provider_capacity_history_events_differential_test.cpp:252` | Real but benign fixture: named setRaw closure remains live while directly awaited, and its copied shared KV owns the store. |
| `tests/provider_removed_provider_circuit_differential_test.cpp:47` | Real but benign fixture: the running Promise is constructed from rt::async, which owns the shared order-vector closure. |
| `tests/provider_selection_health_differential_test.cpp:438` | Real but benign fixture: named recordStep is awaited while parent KV/checker state and literal-backed label remain live. |
| `tests/provider_selection_health_differential_test.cpp:444` | Real but benign fixture: named viewStep is awaited through read/list/compare before parent state leaves scope. |
| `tests/provider_stream_usage_failure_test.cpp:50` | Real but benign fixture: AsyncGenerator retains copied usage data through microtask/yield suspension. |
| `tests/removed_provider_differential_test.cpp:62` | Real but benign fixture: adapter starts rt::async with owned event/body/options captures before constructing its Promise. |
| `tests/responses_failover_branch_coverage_test.cpp:46` | Real but benign fixture: AsyncGenerator owns the events-capturing body through asynchronous reads/yields. |
| `tests/responses_failover_branch_coverage_test.cpp:60` | Real but benign fixture: AsyncGenerator owns the copied exception pointer until its body rejects. |
| `tests/responses_failover_stream_test.cpp:38` | Real but benign fixture: AsyncGenerator retains the events-capturing body throughout delegated stream iteration. |
| `tests/responses_failover_stream_test.cpp:503` | Real but benign fixture: AsyncGenerator owns the returned flag through post-commit validation/iterator cleanup. |
| `tests/responses_native_headers_test.cpp:61` | Real but benign fixture: server retains the handler and parent observed-body/request locals remain live through the awaited HTTP/cleanup sequence. |
| `tests/responses_native_headers_test.cpp:107` | Real but benign fixture: server retains its handler while the parent admission counter remains in scope for the awaited request. |
| `tests/responses_native_headers_test.cpp:110` | Real but benign fixture: usage context retains the immediate-rejection callback; its admission counter lives through the awaited dispatch failure. |
| `tests/responses_native_headers_test.cpp:158` | Real but benign fixture: server retains copied context/status/code; parent original-response state stays live through the awaited request and shutdown. |
| `tests/responses_native_headers_test.cpp:224` | Real but benign fixture: usage context retains the reservation callback; originalReceipt stays in parent scope while admission/cancellation are awaited. |
| `tests/responses_stream_http_test.cpp:90` | Real but benign fixture: ServeState retains the handler's value captures through response construction; stream callbacks own their scheduling state. |
| `tests/responses_stream_test.cpp:345` | Real but benign fixture: SourceAlgorithm owns the pull callable and copied shared flags/frames/gate through terminal suspension. |
| `tests/responses_stream_test.cpp:444` | Real but benign fixture: MaybeAsyncCallback retains onEvent while its deliberately unresolved Promise is pending; captures are owned values. |
| `tests/responses_stream_test.cpp:464` | Real but benign fixture: MaybeAsyncCallback retains onCancel and its shared counter/Promise through the permanent suspension. |
| `tests/rt/abort_test.cpp:276` | Real but benign fixture: blockOn runs rt::async's owned log-capturing coroutine to completion or the caught rejection. |
| `tests/rt/abort_test.cpp:365` | Real but benign fixture: named never closure remains in the parent through the pre-aborted abortable await; its shared flag is owned by value. |
| `tests/rt/async_test.cpp:26` | Real but benign fixture: detached ticks explicitly uses rt::async, owning log/count/prefix through every microtask. |
| `tests/rt/async_test.cpp:72` | Real but benign fixture: rt::async retains the copied log/settled Promise through both waits. |
| `tests/rt/async_test.cpp:79` | Real but benign fixture: rt::async retains the shared log through both microtasks. |
| `tests/rt/async_test.cpp:96` | Real but benign fixture: await helper is copied into each rt::async caller; its mode capture stays owned through the delegated Task. |
| `tests/rt/async_test.cpp:105` | Real but benign fixture: rt::async retains shared log and copied await helper during the inner task. |
| `tests/rt/async_test.cpp:109` | Real but benign fixture: rt::async retains shared log/await helper while the inner-await case runs. |
| `tests/rt/async_test.cpp:113` | Real but benign fixture: rt::async retains log/await helper through the returned-Promise case. |
| `tests/rt/async_test.cpp:182` | Real but benign fixture: Promise::finally_ retains its async callback through completion. |
| `tests/rt/async_test.cpp:223` | Real but benign fixture: Promise::then retains the async callback; value is a by-value coroutine parameter. |
| `tests/rt/async_test.cpp:266` | Real but benign fixture: each spawned consumer uses rt::async with owned log/index/Promise captures. |
| `tests/rt/async_test.cpp:289` | Real but benign fixture: fire-and-forget task is explicitly wrapped in rt::async with shared log ownership. |
| `tests/rt/async_test.cpp:323` | Real but benign fixture: slow sibling uses rt::async and shared log, so fail-fast all cannot free its capture state. |
| `tests/rt/async_test.cpp:449` | Real but benign fixture: AsyncGenerator owns the log-capturing rejecting body through yields and failure. |
| `tests/rt/async_test.cpp:463` | Real but benign fixture: AsyncGenerator retains the outer body/log through delegated iteration and scope cleanup. |
| `tests/rt/async_test.cpp:468` | Real but benign fixture: forAwait owns loopBody and its shared log through early break/iterator return. |
| `tests/rt/async_test.cpp:487` | Real but benign fixture: AsyncGenerator owns the body and shared log even after the local generator handle is copied. |
| `tests/rt/async_test.cpp:491` | Real but benign fixture: rt::async retains copied generator/log while awaiting next. |
| `tests/rt/async_test.cpp:499` | Real but benign fixture: AsyncGenerator owns the shared log body through both yields. |
| `tests/rt/async_test.cpp:504` | Real but benign fixture: rt::async retains copied generator/log through both next calls. |
| `tests/rt/async_test.cpp:515` | Real but benign fixture: rt::async retains generator/log until return_ completes. |
| `tests/rt/async_test.cpp:524` | Real but benign fixture: rt::async owns generator/log across repeated completed next calls. |
| `tests/rt/async_test.cpp:534` | Real but benign fixture: AsyncGenerator owns the log body and scope cleanup through suspension. |
| `tests/rt/async_test.cpp:538` | Real but benign fixture: rt::async owns generator/log through next and return cleanup. |
| `tests/rt/async_test.cpp:548` | Real but benign fixture: AsyncGenerator retains the log-capturing rejecting body. |
| `tests/rt/async_test.cpp:553` | Real but benign fixture: rt::async owns generator/log through the caught next rejection. |
| `tests/rt/async_test.cpp:564` | Real but benign fixture: AsyncGenerator owns its log-capturing return-value body. |
| `tests/rt/async_test.cpp:568` | Real but benign fixture: rt::async owns copied generator/log through next and returnValue inspection. |
| `tests/rt/async_test.cpp:588` | Real but benign fixture: forAwait owns throwing callback/log through expected body rejection and iterator cleanup. |
| `tests/rt/async_test.cpp:601` | Real but benign fixture: forAwait owns collect and its shared log through full iteration. |
| `tests/rt/async_test.cpp:612` | Real but benign fixture: AsyncGenerator retains its body/log until frame destruction runs cleanup. |
| `tests/rt/async_test.cpp:631` | Real but benign fixture: withFinally owns cleanup1 and its log capture through the cleanup microtask. |
| `tests/rt/async_test.cpp:655` | Real but benign fixture: the lifetime regression explicitly returns rt::async, retaining the local string after the creating scope ends. |
| `tests/rt/async_test.cpp:703` | Real but benign fixture: blockOn awaits rt::async's owned shared-log body despite reported callback errors. |
| `tests/rt/async_test.cpp:722` | Real but benign fixture: rt::async retains shared log through the loop's caught uncaught-error outcome. |
| `tests/rt/async_test.cpp:741` | Real but benign fixture: blockOn synchronously awaits rt::async while referenced Loop remains on the caller stack. |
| `tests/rt/async_test.cpp:776` | Real but benign fixture: rt::async owns log/Promise; loop teardown destroys the frame and its guard without resuming references. |
| `tests/rt/async_test.cpp:793` | Real but benign fixture: Promise is constructed from rt::async, retaining the shared log across the default-loop microtask. |
| `tests/rt/async_test.cpp:827` | Real but benign fixture: rt::async owns the token capture while returning the counted aggregate. |
| `tests/rt/async_test.cpp:836` | Real but benign fixture: AsyncGenerator owns token/body through the counted aggregate yield. |
| `tests/rt/clock_test.cpp:253` | Real but benign fixture: blockOn awaits rt::async while referenced clock/log/timing locals remain on the caller stack. |
| `tests/rt/fetch_test.cpp:63` | Real but benign fixture: Fetch::keepAlive retains the shared seen-log callback while the mocked fetch is awaited. |
| `tests/rt/fetch_transport_test.cpp:749` | Real but benign fixture: SourceAlgorithm retains the delayed pull closure and shared produced counter until the pull settles. |
| `tests/rt/fetch_transport_test.cpp:1161` | Real but benign fixture: the live Peer retains its open callback while its connection task awaits it; closeAfter is copied by value. |
| `tests/rt/fetch_transport_test.cpp:1230` | Real but benign fixture: Peer retains respond through its awaited connection handling, owning shared calls and copied step kind. |
| `tests/rt/fetch_transport_test.cpp:1266` | Real but benign fixture: retained Peer response callback owns the shared counter through the fragmented send. |
| `tests/rt/fetch_transport_test.cpp:1289` | Real but benign fixture: Peer retains the response callback and shared counter while connection handling awaits it. |
| `tests/rt/fetch_transport_test.cpp:1382` | Real but benign fixture: blockOn awaits rt::async while the referenced outer transport variable remains on the test stack. |
| `tests/rt/fetch_transport_test.cpp:1407` | Real but benign fixture: Peer/connection handling retains respond and its shared secondSent flag through delayed chunk delivery. |
| `tests/rt/gzip_test.cpp:367` | Real but benign fixture: detached turn counter explicitly uses rt::async with shared counter ownership. |
| `tests/rt/kv_sqlite_test.cpp:502` | Real but benign fixture: blockOn awaits rt::async while referenced directory/holder/thread/outcome locals remain in the outer synchronous test scope. |
| `tests/rt/kv_sqlite_test.cpp:619` | Real but benign fixture: the subprocess writer uses blockOn(rt::async) with an owned filename capture for its lifetime. |
| `tests/rt/kv_test.cpp:921` | Real but benign fixture: named writer is joined by Asio operator&& before the parent KV owners leave scope. |
| `tests/rt/kv_test.cpp:928` | Real but benign fixture: named committer is joined/cancelled by Asio operator&&, keeping fences/outcomes/KV live through completion. |
| `tests/rt/kv_test.cpp:988` | Real but benign fixture: named commitChecks is directly awaited while parent MemoryKv remains live; checks/value parameters are owned. |
| `tests/rt/kv_test.cpp:1027` | Real but benign fixture: commitOf is directly awaited; temporary build callback lives through the await full expression and is used before commit suspension. |
| `tests/rt/kv_test.cpp:1138` | Real but benign fixture: named checkAbsent is awaited in the parent KV/keyOf scope. |
| `tests/rt/kv_test.cpp:1143` | Real but benign fixture: named sumAt is awaited while parent KV/keyOf remain live. |
| `tests/rt/kv_test.cpp:1148` | Real but benign fixture: named listPrefix is directly awaited and owns its iterator while parent KV remains live. |
| `tests/rt/kv_test.cpp:1152` | Real but benign fixture: named listRange is awaited with parent KV/keyOf still in scope. |
| `tests/rt/kv_test.cpp:1194` | Real but benign fixture: named payload is directly awaited before its referenced KV/keyOf state leaves scope. |
| `tests/rt/kv_test.cpp:1223` | Real but benign fixture: attempt is directly awaited; temporary build callbacks remain alive through the full await expression and are used before suspension. |
| `tests/rt/kv_test.cpp:1258` | Real but benign fixture: named page is directly awaited while parent MemoryKv lives; selector/options/iterator are frame-owned. |
| `tests/rt/kv_test.cpp:1381` | Real but benign fixture: named rest helper is awaited while KV lives; cursor text and iterator are owned by its frame. |
| `tests/rt/kv_test.cpp:1424` | Real but benign fixture: named roundTrip is awaited in parent KV scope and owns its input value parameter. |
| `tests/rt/stream_test.cpp:28` | Real but benign fixture: detached ticks explicitly wraps copied log/count in rt::async. |
| `tests/rt/stream_test.cpp:205` | Real but benign fixture: SourceAlgorithm retains async cancel and its shared log through delayed settlement. |
| `tests/rt/stream_test.cpp:268` | Real but benign fixture: SourceAlgorithm retains shared pull count across microtask/enqueue/close. |
| `tests/rt/stream_test.cpp:355` | Real but benign fixture: forAwait owns body/log through early break and cancellation. |
| `tests/rt/stream_test.cpp:371` | Real but benign fixture: forAwait owns collect/log through complete stream iteration. |
| `tests/rt/stream_test.cpp:385` | Real but benign fixture: SourceAlgorithm retains shared frames/index across the delayed pull. |
| `tests/rt/stream_test.cpp:452` | Real but benign fixture: startTicker returns shared state owned by its detached rt::async body. |
| `tests/rt/stream_test.cpp:552` | Real but benign fixture: detached action task uses rt::async with owned log/tick/controller/action captures. |
| `tests/rt/stream_test.cpp:588` | Real but benign fixture: detached abort task uses rt::async with owned shared controller/log/tick/resolver captures. |
| `tests/rt/websocket_test.cpp:1704` | Real but benign fixture: named body is awaited by blockOn while its closure and referenced seen vector remain in the enclosing synchronous test scope. |
| `tests/runtime_early_sse_test.cpp:72` | Real but benign fixture: ServeState retains handler value captures through the request-body and release gate waits. |
| `tests/runtime_early_sse_test.cpp:117` | Real but benign fixture: server owns the handler and shared observed request through body-read/early-header validation. |
| `tests/runtime_early_sse_test.cpp:151` | Real but benign fixture: ServeState retains the handler's copied selection/finish gates through response selection. |
| `tests/runtime_early_sse_test.cpp:157` | Real but benign fixture: SourceAlgorithm owns the async start callable and copied finish Promise through the split SSE body. |
| `tests/runtime_early_sse_test.cpp:208` | Real but benign fixture: ServeState retains copied request/delivery/gate values through body reading and release. |
| `tests/runtime_http_differential_test.cpp:368` | Real but benign fixture: timeout Promise is constructed from rt::async, retaining the copied deadline until sleep completes. |
| `tests/runtime_http_sse_headers_test.cpp:98` | Real but benign fixture: ServeState retains the value-capturing handler through receipt/stream construction. |
| `tests/runtime_http_sse_headers_test.cpp:102` | Real but benign fixture: SourceAlgorithm retains copied resolvers/shared flags/release Promise through the delayed pull. |
| `tests/runtime_http_test.cpp:159` | Real but benign fixture: ServeState retains the handler and its copied observation resolver through response construction. |
| `tests/runtime_http_test.cpp:164` | Real but benign fixture: SourceAlgorithm retains the shared index through delayed chunk production and close. |
| `tests/runtime_http_test.cpp:204` | Real but benign fixture: server owns the copied receipt/cancel resolvers and pending Promise through handler completion. |
| `tests/runtime_http_test.cpp:208` | Real but benign fixture: SourceAlgorithm retains the pending Promise capture for the deliberately suspended pull. |
| `tests/runtime_http_test.cpp:288` | Real but benign fixture: server retains copied release/start state while constructing its async body stream. |
| `tests/runtime_http_test.cpp:290` | Real but benign fixture: SourceAlgorithm retains copied release/start state through delayed enqueue/close. |
| `tests/runtime_http_test.cpp:323` | Real but benign fixture: ServeState retains the handler and copied delivery-write gate/resolver. |
| `tests/runtime_http_test.cpp:324` | Real but benign fixture: Promise::then retains the async write callback until its release gate settles. |
| `tests/runtime_http_test.cpp:375` | Real but benign fixture: ServeState owns the upgrade callable and copied acceptance resolver through authorization/upgrade handling. |
| `tests/runtime_http_test.cpp:437` | Real but benign fixture: retained upgrade callback owns the shared message counter through socket acceptance. |
| `tests/runtime_http_test.cpp:479` | Real but benign fixture: server owns copied observation resolver/shared pull count through response construction. |
| `tests/runtime_http_test.cpp:520` | Real but benign fixture: server retains copied body/resolver across awaited oversized-input cancellation. |
| `tests/runtime_http_test.cpp:569` | Real but benign fixture: ServeState retains copied observation resolver while echoing the awaited request text. |
| `tests/runtime_http_test.cpp:597` | Real but benign fixture: ServeState retains the copied observation resolver and request handler through awaited body handling. |
| `tests/sentinel_replay_bootstrap_retry_test.cpp:975` | Real but benign fixture: named chunksLeft is directly awaited while the parent store/closure remain live. |
| `tests/sentinel_replay_bootstrap_retry_test.cpp:1003` | Real but benign fixture: rt::async owns the interposed callback; parent handle remains live while batch requests and fixture cleanup settle. |
| `tests/sentinel_replay_bootstrap_retry_test.cpp:1047` | Real but benign fixture: rt::async retains copied shared result state; borrowed handle stays in parent scope through the interposed batch. |
| `tests/sentinel_replay_bootstrap_retry_test.cpp:1078` | Real but benign fixture: named request closure is retained/awaited by the parent while its borrowed HoldNextList handle remains live. |
| `tests/sentinel_replay_bootstrap_retry_test.cpp:1135` | Real but benign fixture: named cursor/list helper is directly awaited before the parent KV store leaves scope. |
| `tests/sentinel_replay_bootstrap_retry_test.cpp:1348` | Real but benign fixture: named sweepUntil remains in the parent during awaited sweeps; store/handle callers retain their KV objects. |
| `tests/sentinel_replay_bootstrap_retry_test.cpp:1504` | Real but benign fixture: named request is awaited while the parent's pausing handle remains live through admission/cleanup. |
| `tests/sentinel_replay_bootstrap_retry_test.cpp:1589` | Real but benign fixture: parent retains/awaits request and its borrowed handle; admissions is copied shared state. |
| `tests/sentinel_replay_coverage_test.cpp:995` | Real but benign fixture: named pageKeys is directly awaited while the parent KV stub remains in scope. |
| `tests/sentinel_replay_read_differential_test.cpp:312` | Real but benign fixture: RECORD awaits rt::async before cursor/window/KV locals leave the case block. |
| `tests/sentinel_replay_read_differential_test.cpp:320` | Real but benign fixture: rt::async owns the callable and RECORD awaits it while referenced page cursor/options/KV remain live. |
| `tests/sentinel_replay_read_differential_test.cpp:453` | Real but benign fixture: RECORD awaits the owned rt::async body through incident listing/decryption in the live parent scope. |
| `tests/sentinel_replay_read_differential_test.cpp:465` | Real but benign fixture: RECORD awaits rt::async before cursor/query/KV/dangling-key locals leave scope. |
| `tests/sentinel_replay_read_differential_test.cpp:541` | Real but benign fixture: RECORD awaits the owned rt::async export/decryption body while parent KV remains live. |
| `tests/sentinel_replay_retention_metadata_differential_test.cpp:375` | Real but benign fixture: STEP awaits the owned rt::async bootstrap loop in the parent KV scope. |
| `tests/sentinel_replay_retention_metadata_differential_test.cpp:450` | Real but benign fixture: rt::async owns the race callback; reconciliation/STEP completes while the borrowed store remains in parent scope. |
| `tests/sentinel_replay_retention_metadata_differential_test.cpp:459` | Real but benign fixture: rt::async owns the interposed marker-write callback; parent store remains live through the awaited reconciliation. |
| `tests/sentinel_replay_retention_metadata_differential_test.cpp:472` | Real but benign fixture: STEP awaits rt::async's maintenance body before parent KV/wrapper locals leave scope. |
| `tests/sentinel_replay_retention_metadata_test.cpp:331` | Real but benign fixture: shared CommitObservation retains before and its value captures; parent store stays live through the awaited observed commit. |
| `tests/sentinel_replay_retention_metadata_test.cpp:580` | Real but benign fixture: observation retains the callback while reconciliation awaits it; parent store outlives the observed admission. |
| `tests/sentinel_replay_retention_metadata_test.cpp:936` | Real but benign fixture: shared RaceState retains race through the awaited wrapper commit; parent store remains live. |
| `tests/sentinel_replay_retention_metadata_test.cpp:1038` | Real but benign fixture: shared RaceState retains the maintenance callback; parent store lives through maintenance and marker observation. |
| `tests/sentinel_replay_retention_test.cpp:1581` | Real but benign fixture: shared BootstrapCommitObservation retains beforeCommit and copied manifest/change state; borrowed raw KV stays live through the test's awaited bootstrap. |
| `tests/sentinel_replay_staging_test.cpp:286` | Real but benign fixture: shared Observation retains before callback and value state; parent store stays live through staged commit/race assertions. |
| `tests/sentinel_replay_staging_test.cpp:304` | Real but benign fixture: shared Observation retains after callback through cleanup checks while the parent store remains in scope. |
| `tests/sentinel_replay_staging_test.cpp:342` | Real but benign fixture: Observation retains its before callback/key copy through the awaited staging commit; parent KV remains live. |
| `tests/sentinel_replay_staging_test.cpp:390` | Real but benign fixture: Observation owns the copied injected exception pointer through its deliberate second-batch rejection. |
| `tests/sentinel_upstream_providers_test.cpp:385` | Real but benign fixture: Fetch::keepAlive retains the moved message capture through its deliberate rejection. |
| `tests/sentinel_upstream_providers_test.cpp:455` | Real but benign fixture: SourceAlgorithm retains shared stream state through pull/close, including clearing algorithms. |
| `tests/sentinel_upstream_providers_test.cpp:518` | Real but benign fixture: owning Fetch retains the no-dispatch sentinel; parent dispatchAttempts remains live through the awaited refusal. |
| `tests/sentinel_upstream_providers_test.cpp:969` | Real but benign fixture: Fetch::keepAlive owns replay/token data; the parent replay counter stays in scope while replay requests settle. |
| `tests/serve_delivery_http_test.cpp:71` | Real but benign fixture: createServeHandler retains the delivery-aware callback; parent cancellation/signal observations live through awaited delivery/interrupt cleanup. |
| `tests/serve_delivery_http_test.cpp:80` | Real but benign fixture: SourceAlgorithm retains the copied pending Promise for the suspended pull. |
| `tests/serve_delivery_http_test.cpp:247` | Real but benign fixture: SourceAlgorithm owns the pending Promise capture while cancellation/terminal handling completes. |
| `tests/storage_bun_kv_test.cpp:1071` | Real but benign fixture: writer is wrapped in rt::async and owns its shared KV independently of fail-fast sibling completion. |
| `tests/storage_bun_kv_test.cpp:1078` | Real but benign fixture: rt::async owns shared KV/fences/outcomes captures, so sibling failure cannot destroy referenced local state. |
| `tests/storage_bun_kv_test.cpp:1111` | Real but benign fixture: subprocess writer uses blockOn(rt::async) with an owned database path capture. |
| `tests/storage_bun_kv_test.cpp:1157` | Real but benign fixture: blockOn awaits rt::async's owned path-capturing transaction body. |
| `tests/storage_native_kv_differential_test.cpp:377` | Real but benign fixture: retrySqliteBusy owns alwaysLocked by value; parent attempts counter lives through the awaited retry failure. |
| `tests/storage_native_kv_differential_test.cpp:388` | Real but benign fixture: retrySqliteBusy owns eventually by value; parent syncAttempts remains live through the awaited success. |
| `tests/surplus_test.cpp:263` | Real but benign fixture: the dispatch object retains cancelBeforeTransport; parent events remains live throughout the awaited authenticated fetch/cancellation contract. |
| `tests/usage_optimization_measurement_test.cpp:150` | Real but benign fixture: Fetch::keepAlive retains the callback; referenced KV/reply queue/counters live through each awaited measurement step. |
| `tests/usage_optimization_measurement_test.cpp:165` | Real but benign fixture: named prepare is awaited sequentially while referenced measurement state remains in parent scope; optional key data is a by-value parameter. |
| `tests/worker_boundary_test.cpp:534` | Real but benign fixture: dispatch override starts rt::async with owned upstream/shared state/input/attempt/signal/callback copies. |

## Ranked confirmed defect classes

1. **P1 memory safety, authenticated admin request path:**
   `src/admin/kernel.cpp:130,134`, the two `handleAdminKernelPolicyQueueList` children.
   One rejected KV read makes fail-fast `rt::all` reject while a sibling still runs;
   unwinding frees the closure/output set the sibling accesses after its read.
   Minimal fix: captureless child coroutines with by-value KV/shared-output parameters,
   preserving fail-fast semantics and await positions (exact proposed diff in URGENT).
   Current-run clang-tidy warning is reproduced; the ASan witness is retained earlier-agent
   evidence, not a fresh run or real HTTP request. No source change applied.

### Resource-exhaustion cleanup hardening (unranked)
   `src/rt/fetch_transport.cpp:274-277`, instantiated `include/uos/rt/async.hpp:564`.
   An aborted transaction closes its socket, then its ScopeExit destructor constructs
   and spawns a reader-cancellation task. The exported exception-escape trace proves that
   coroutine-frame allocation may throw `boost::wrapexcept<std::bad_alloc>`; escaping the
   implicitly noexcept destructor invokes terminate. No ordinary-input or runtime
   reproduction is claimed. Keep the cancellation best effort at its synchronous boundary,
   consistent with the existing ignoreRejection for asynchronous failure:

```diff
--- a/cpp/gateway/src/rt/fetch_transport.cpp
+++ b/cpp/gateway/src/rt/fetch_transport.cpp
@@
-        if (writer->reader && signal.aborted()) spawn(writer->reader->cancel(signal.reason()), ignoreRejection);
+        if (writer->reader && signal.aborted()) {
+          try { spawn(writer->reader->cancel(signal.reason()), ignoreRejection); }
+          catch (...) {}
+        }
```

This is an optional owner-applied hardening proposal; no source edit is applied.
The initial P2 candidate was downgraded after inspecting the exact LLVM 24 source:
ExceptionEscapeCheck deliberately ignores std::bad_alloc, but the filter matches
unqualified tag names and misses Boost's wrapexcept<std::bad_alloc>. No non-allocation
exception is shown by this trace. Do not globally swallow ScopeExit callbacks or
treat a noexcept annotation as a fix.

Other warnings below are classified as benign real costs/conventions, hardening
opportunities or false positives; additional true defects will be ranked if found.
No additional confirmed ordinary request-path defect is established at this checkpoint.

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

## Tooling verification (current checkpoint)

Changed paths are the four owned audit files. The shell/Python runner uses `sh -n`,
ShellCheck, Python syntax compilation and five negative CLI controls; configuration uses
Clang's `--verify-config`, inventory and positive/negative C++ controls; generated JSON uses
JSON parsing/count reconciliation; changed Markdown/JSON/YAML use the pinned formatter and
`git diff --check`. No application TypeScript, Rust, native build or deployment gate is relevant.

- `sh -n cpp/gateway/tools/tidy.sh`: pass.
- `shellcheck cpp/gateway/tools/tidy.sh`: pass.
- Embedded Python `compile(..., 'exec')`: pass, without an in-checkout pycache.
- CLI rejection controls: **5/5 pass**, exit 2 for nonpositive jobs/timeout/checkpoint
  interval, mutually exclusive `--audit`/`--config`, and in-checkout build output.
  Controls run with fresh HOME/TMPDIR and an explicit credential-free environment.
- Audit `--verify-config`: pass; 245 installed checks selected.
- Marked hazard controls: **8/8 expected markers**, clean control zero diagnostics;
  validation JSON SHA-256 `55a5bc8ed88dc036c0dc04807c1279698f402d488d0bcb9d5cede1d4285046bd`.
- Full 830-unit analysis, final formatting and source/receipt reconciliation: pending.

The root and quality-tool lockfiles exist and remain unchanged. Pinned Bun 1.4.0 and
frozen quality dependencies were prepared under `/tmp/cppgw-tidy-20261117/lint` so the
checkout contains no dependency downloads. No application tests are executed by this
static-analysis task; analyzed test TUs are not runtime pass counts.

## Final handoff summary

Continuation checkpoint: runner now atomically saves the partial baseline and deduplicated
diagnostics at startup and every 100 completed units. The full audit, triage, final configuration
and publication receipts remain pending. No earlier partial count is reused as current evidence.
No application source has been changed. No C++ test binary has been executed by this audit yet.
