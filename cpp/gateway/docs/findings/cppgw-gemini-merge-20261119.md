# Gemini subscription integration

## Starting point

- The dispatched checkout branch is `agent-job`, at input-limits commit `74062589fbb209d376a39d119cc00b5a4140d694`, tree `19eaea54ab1158f096ecfd22cfb413259625e0c7`. The starting checkout was clean.
- `git fetch origin agent-jobs/cppgw-conformance-pin-20261119-44467964951-1 codex/antigravity-gemini-subscription-20261118` fetched both inputs. The conformance head is `2075bae6fffe7fb9bafa367ea65fb9e8c9521a88`. The Gemini head matches the requested `8110276d0f374ac64cc4edff32f5a1f02cfe3706`.
- `uname -m`, `cat /etc/os-release`, `nproc`, `clang++ --version` and `bun --version` identified Ubuntu 24.04.5 ARM64, four CPUs, system Clang 18 and Bun 1.4.0. GCC 14 is installed. The qualified build dependencies are being prepared outside the checkout.
- The scratch root for dependencies, commands and receipts is `/var/tmp/cppgw-gemini-merge-20261119.eiM1DZcH`.

## Conformance merge

`git merge --no-ff --no-commit origin/agent-jobs/cppgw-conformance-pin-20261119-44467964951-1` merged cleanly. The staged tree passed `python3 -B cpp/gateway/tools/links.py --check` with 451 registrations and `git diff --cached --check`. The merge retains production's DeepSeek compaction at 75% of usable context and the paired production oracle pin.

## Status

The security audit and executable qualification runs are pending. Tests will use the supported namespace drivers with a fresh home and temporary directory, a credential-free environment, and local fixtures.

## Gemini merge

`git merge --no-ff --no-commit origin/codex/antigravity-gemini-subscription-20261118` merged the pinned Gemini head. Resolved the DeepSeek conflict with the first parent's header and kept the retired manifest generator and four legacy files deleted. `git ls-files` found zero forbidden root paths, and `comm -3` on the sorted TypeScript and Rust path lists before and after the merge gave no difference. Compared the Gemini changes against the common ancestor under `tools/native-kv`, `ops`, and `**/permissions-native.c`. There were no changes to transplant. The operator prerequisites carried into `docs/RUNTIME.md`.

`python3 -B cpp/gateway/tools/cmake-sources.py "$scratch/sources.cmake"` validated 325 historical keys plus two native Gemini modules, 44 runtime files and 371 total sources. `python3 -B cpp/gateway/tools/links.py --check`, `bash cpp/gateway/tools/ci-build.sh --list` and `git diff --cached --check` passed with 452 test registrations.

The portable Docker source stage and CMake dependency tracking now include `PORTED.txt`, which the merged inventory checker reads. Build input hashing includes the same file. The native API-key vector header now identifies the Gemini rows as native additions. The provider-selection vector header already described its Gemini extensions correctly. Item **3h FIXED**.

## Confinement audit

- **3a VERIFIED in source, executable qualification pending.** The legacy confinement fixture is byte-identical to the starting branch. The new test compares read, write, immutable and execute grants with the pre-merge profile for five disabled settings and an installed CLI fixture. It compares the entire 408-byte ARM64 seccomp program with SHA-256 `6536d38a3e616f3fe229a8cef632c6cda5c15b3aff6caf75de2d7c527b1dd552`. The digest was extracted from `git show 74062589:cpp/gateway/src/runtime/permissions_linux.cpp` by compiling its original `installProcessFilter` with `clang++-21 -std=c++20` and replacing its syscall sink with a write of the filter instructions. The test still checks refusal of execute grants without opt-in.
- **3b RESIDUAL.** The pinned branch notes list glibc shared dependencies, so the CLI is treated as dynamically linked. The gateway seccomp exception is process-wide. Direct execution is limited to canonical `agy` and its loader. A dedicated spawning thread adds a stricter inheritable Landlock ruleset and closes inherited descriptors beyond standard I/O. The CLI sees individual runtime libraries, private CLI inputs and a per-turn workspace instead of the gateway data and general library directories. The loader can still read compatible ELF files from the gateway's broader read set when called by the gateway and from the CLI's own readable paths when called by the CLI. The new fixture tests direct spawn denial and loader verification of an approved ELF and controlled gateway-state ELF. Production `agy` was unavailable on this runner.

## CLI and request audit

- **3c VERIFIED in source, fixture run pending.** The fake `agy` prints the four explicitly supplied environment variables. The fixture also places synthetic markers in the actual parent OS environment to exercise the `posix_spawn` environment boundary.
- **3d FIXED.** Added the gateway's 120-second timer, concurrent bounded readers and stdin delivery, forceful process termination, pipe cancellation and awaited reaping. Stdout uses the central 16 MiB JSON budget, with 4 MiB per event and 128 KiB for stderr. New fake CLI cases exercise downstream abort, deadline, full pipes, each output limit and `waitpid` reaping.
- **3e VERIFIED in source, fixture run pending.** Both streaming response formats check optional usage before conversion. Retained the Responses regression and added a Chat Completions regression with `include_usage` and missing counts.
- **3f FIXED.** The API uses fixed messages for spawn and quota refresh failures and never forwards CLI diagnostics or denied-action details. Logging uses the redacted upstream diagnostic. Both 413 and local CLI startup or configuration 503 responses avoid provider-health failure observations. The fixtures cover log redaction and the unchanged healthy state after both local refusals.
- **3i FIXED.** CLI pipe tasks own their readers, cancellation removes its listener on every exit and drains the process before returning. The quota write awaits a named task. Fixture callbacks that borrow test state now finish that work before returning an owning task.
