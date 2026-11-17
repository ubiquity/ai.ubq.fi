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

## Contract v1 and status

IN PROGRESS. No full-run result or clean baseline is claimed at this checkpoint.

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

## Final handoff summary

Pending: configuration, portable runner, full audit, triage, baseline and publication receipts.
No application source has been changed. No C++ test binary has been executed by this audit yet.
