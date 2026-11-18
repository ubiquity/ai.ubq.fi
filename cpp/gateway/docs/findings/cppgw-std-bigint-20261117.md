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

## Continuation and contract v2

The resumed checkout started clean at `e6c2a002d954318e10b52b411f871f3d9d48dbcd` (tree `6f637163a39cf068e4d5a3626c5261b7758ff240`). The inherited commits contain the arithmetic replacement, a three-case BigInt regression and the registered fuzz target. The registry now contains 442 targets. The runner restored the verified Boost archive and installed Clang 21.1.8. The old and new implementations use the same compiler for paired measurements. The full test driver uses SQLite 3.54.0 with `SQLITE_DQS=3` per the portable CI instructions. [Portable CI requirements](../CI.md)

The inherited source compiled with the strict port flags at `-O2`. The new BigInt test passed 3/3 cases and the Number test passed 20/21. The only failure was the recorded BigInt parser case. Its three zero expectations for incomplete signed or prefixed tokens are outside the explicit write scope. Bun 1.4.0 independently confirmed all three zero results and the same prefix-only behavior with octal and binary. Its full-width unsigned cast also differs from the gateway as recorded in the deviations. [BigInt deviations](../DEVIATIONS-FROM-BUN.md#bigint)

The suite and fuzz campaigns use empty home and temporary directories, a credential-free environment and Linux namespaces. The selected gates are the two C++ build profiles for all 442 link closures, the 120-second BigInt fuzz smoke, paired measurements, changed-document formatting and `git diff --check`. The verification runs and measurements are pending. The stop condition is the terminal result of these runs, including any differences in test fixtures or unavailable handoff prerequisites.

## Resumed qualification

The handoff contained the v2 notes and deviation update in the index. It also contained a completed libFuzzer log. The registered harness ran 901,036 inputs in 121 seconds with a peak RSS of 411 MiB and zero findings. The checked-in corpus has five seeds. No new vendor or build registration is needed because the pinned Boost headers are already part of the gateway's build.

Boost owns arithmetic, comparison, signed bitwise operations, shifts, exponentiation, conversion to and from binary64, and decimal, octal and hexadecimal arithmetic and formatting. The adapter validates complete input, normalizes decimal digits for Boost's parser, uses Boost bit import and export for the power-of-two formats that need them, and groups other output radices into native `to_chars` chunks using Boost division. It retains the public BigInt API, existing error classes, bounds on allocation, fixed-width wrapping and signed conversion from the low 64 bits. These preserve the integer representations used by KV, typed values, authority and the FNV-1a hash.

Relative to `093a9e0511e9d0deae5495dc324e53ff4dccd719`, the production diff deletes 448 lines and adds 146 across `src/rt/number.cpp` and `include/uos/rt/number.hpp`, a net reduction of 302 lines. The new implementation replaces the limb vector, carry and borrow operations, arbitrary-precision multiply and divide, two's-complement limb translation and binary64 rounding algorithm.

### Paired measurements

Both benchmark binaries used Clang 21.1.8 `-O2` with the strict port flags on the same Linux ARM64 runner. Each linked its own `number.cpp` and used the corresponding `number.hpp`. The baseline files came from `git show 093a9e05:<path>`. Each reported value is the median of seven runs using `taskset -c 3` and thread CPU time. Runs alternated old-first and new-first, warmed up each operation with 100 iterations and counted calls to global C++ allocation operators during the timed loop. Inputs and result consumption were identical between binaries.

| Operation                                | Iterations per sample | Old ns/op | Boost ns/op | Old allocations/op | Boost allocations/op |
| ---------------------------------------- | --------------------: | --------: | ----------: | -----------------: | -------------------: |
| Increment counter starting at 10,000,000 |             1,000,000 |     23.20 |       15.08 |              1.000 |                0.000 |
| FNV-1a byte step with 64-bit wrapping    |             1,000,000 |    113.48 |       91.46 |              5.996 |                0.000 |
| Parse signed decimal `-123456789`        |               500,000 |    138.26 |       60.79 |              6.000 |                0.000 |
| Parse 77 decimal nines, 256 bits         |               100,000 |    728.04 |      237.51 |             12.000 |                2.000 |
| Parse 1,233 decimal nines, 4,096 bits    |                 2,000 | 69,518.67 |    4,180.94 |             20.000 |                4.000 |
| Multiply 4,097- and 2,048-bit operands   |                 5,000 |  8,416.72 |    2,728.94 |              1.000 |                1.000 |
| Divide 4,097- by 2,048-bit operands      |                 5,000 |  5,655.06 |    3,464.41 |              4.000 |                3.000 |
| Format 4,097 bits in decimal             |                 2,000 | 43,528.02 |   34,513.08 |              9.000 |               70.000 |
| Format 4,097 bits in base 36             |                 2,000 | 39,148.08 |   36,048.08 |              8.000 |                9.000 |
| Convert 256 bits to binary64             |             1,000,000 |     27.58 |        9.73 |              1.000 |                0.000 |

The two wide operands are `(1 << 4095) + (10^1233 - 1)` and `(1 << 2047)` plus 616 decimal sevens. The 256-bit conversion uses the same 77 nines as the parser workload. `sizeof(BigInt)` is 32 bytes in each build. Boost's decimal formatting took less CPU time on the wide operand and allocated about eight times as often.

The full suites are still running. The first portable-driver invocation found that `tools/ci-build.sh` is mode 0644, so the succeeding invocation used `bash` to run the script. The Boost headers are in the extracted archive and the compiled archives are in the separate `boost/lib` prefix.
