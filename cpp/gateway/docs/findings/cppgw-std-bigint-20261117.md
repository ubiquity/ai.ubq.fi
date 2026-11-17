# Boost.Multiprecision BigInt replacement

## Contract v1

Replace the limb implementation of `uos::rt::BigInt` with the pinned, header-only Boost 1.92.0 `cpp_int` on C++ port base `093a9e0511e9d0deae5495dc324e53ff4dccd719` (tree `cb6c6415cfd603f5aac070d424bc42b4642349e1`). Preserve the C++ API, canonical KV key and value bytes, integer ordering, low-64-bit casts and the 1,048,576-bit size limit. Number formatting and `Math` are outside the BigInt replacement. This unit has no provider or deployment changes.

| Surface                                                             | Acceptance                                                                                                                      |
| ------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| Arithmetic, comparison, signed bit operations and double conversion | Boost owns the operations. Existing arithmetic and conversion vectors are checked against the replacement.                      |
| Text and fixed-width casts                                          | Keep the application numeric formats and total input validation. Document each changed Bun-only expectation.                    |
| Storage, authority and remaining consumers                          | Use the existing test link registry with strict release and ASan/UBSan builds. Every existing binary links `src/rt/number.cpp`. |
| Robustness and cost                                                 | Run the registered BigInt libFuzzer entry point for 120 seconds and measure paired old/new release workloads.                   |

Owned production changes are confined to the BigInt regions in `src/rt/number.cpp` and `include/uos/rt/number.hpp`. Report, deviation notes, the BigInt regression and its link entry, and the BigInt fuzz target and seeds are in scope. Boost already ships through the native profile. The starting registry contains 441 binaries. The affected test paths are selected with `python3 -B cpp/gateway/tools/links.py --list-tests`. Verification uses `cpp/gateway/tools/ci-build.sh --config release` and `--config asan`, a bounded fuzz smoke, changed-document formatting and `git diff --check`. Gates and benchmarks use external scratch, with a clean environment and Linux fixture isolation for test execution.

The Bun 1.4.0 parser accepts `"-"`, `"+"` and `"0x "` as zero. The existing recorded vectors for those leniencies are in `tests/rt/number_test.cpp`, outside the assigned write paths. Authorization for a narrow expectation update is pending. Library boundary behavior, full-suite outcome and performance are still being measured.

The stop condition is a buildable committed replacement, recorded deviations, reproducible verification, cost and source-line counts, and an exact list of any unmet gates for the C++ port integrator.

## Runner preparation

Ubuntu 24.04.5 Linux ARM64 with four CPUs, GCC 14.2.0, Clang 21.1.8 and Bun 1.4.0. Boost.JSON and Boost.Container static archives were built in `/tmp` from the official Boost 1.92.0 release archive. Its SHA-256 matched the upstream release metadata. [Archive metadata](https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2.json)

```text
5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c  boost_1_92_0.tar.bz2
```
