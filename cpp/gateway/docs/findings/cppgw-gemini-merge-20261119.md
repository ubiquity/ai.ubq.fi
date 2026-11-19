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
