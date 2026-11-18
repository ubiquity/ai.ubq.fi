# Native gateway cleanup inventory findings

## Work contract v1

Inventory the C++ gateway's live dependencies, checked-in replay inputs, optional reference tools, and owner-controlled operations. Produce a tested keep list and an executable removal plan. This job owns this report and `cpp/gateway/docs/CLEANUP-PLAN.md`. All source, tests, workflows, credentials, and production state are outside the edit scope. Provider-dependent checks use local mocks in an empty-home, fresh-temp, credential-free environment.

| Surface | Evidence required |
| --- | --- |
| Build, release shard, runtime, static files, KV, confinement and startup | A temporary checkout copied by an explicit allowlist, the requested release shard, and focused checks outside that shard |
| Fuzzing and CI workflows | Entry points, transitive reads and relevant isolated probes |
| Portable package | Native source and dependency provenance, required repository inputs, and package/runtime checks |
| TypeScript comparison | Recorder and oracle call sites, historical pin and recovery commands |
| Repository handoff | Every tracked root entry and workflow classified, measured before/after counts, ordered cleanup steps and acceptance gates |

Open questions at kickoff include exact fixture boundaries, whether any package probe reads the Rust workspace, and which features require the host namespace and native addons. Stop after the closure and named tests are verified and the plan is sufficiently specific to execute, or record the exact failing command and host constraint if a gate cannot be run.

## Baseline

- The checkout starts on local branch `agent-job`, equal to `origin/agent-jobs/cppgw-integrate-f1-20261118-44343309770-1` at `2b23fb6837e2b5c0dc9ffa50771f52d322dd413a` (tree `d4f9c3beb10816358c85e92c1c231421fa35fa48`). `git status --porcelain=v1` was empty. This is the source snapshot for the inventory, measurements and oracle worktree.
- Host observation via `uname -m`, `cat /etc/os-release`, `nproc`, `free -h`, `gcc-14 --version` and `bun --version` returned Ubuntu 24.04.5 ARM64, four CPUs, 15 GiB RAM, GCC 14.2.0 and Bun 1.4.0. The default Clang is 18.1.3.
- Initial discovery used `git ls-tree --name-only HEAD`, `git ls-files .github/workflows .githooks`, `rg --files -g AGENTS.md -g CI.md --hidden` and `sed -n '1,240p' cpp/gateway/docs/CI.md`.
