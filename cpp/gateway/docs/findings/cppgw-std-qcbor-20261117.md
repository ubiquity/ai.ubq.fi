# Passkey CBOR replacement

## Work contract v1

Replace the translated tiny-cbor value model and codec with QCBOR 1.6.1. Keep WebAuthn verification decisions, credential storage and the original signed authenticator bytes at their existing boundaries. This change owns the CBOR region and necessary model call sites in `src/auth/passkeys.cpp`, the QCBOR vendor directory and its build registration, and the scoped CBOR tests, fuzz target and findings. ASN.1, X.509, TypeScript, other gateway modules, workflows and deployment are outside the change.

| Input                                                  | Required behavior                                                                                                                                                      |
| ------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Attestation object and stored COSE key                 | Decode exactly one canonical item and reject trailing data.                                                                                                            |
| COSE key and extensions embedded in authenticator data | Decode the first canonical item, advance by consumed wire bytes, and enforce the existing final boundary.                                                              |
| Untrusted CBOR                                         | Enforce minimal widths, definite lengths, CTAP2 key order, unique integer or text keys, valid UTF-8, the supported WebAuthn types, eight container levels and one MiB. |
| Existing Firefox 117 EdDSA attestation                 | Retain the exact known map-count correction before decoding and preserve the signed input bytes.                                                                       |

WebAuthn Level 3 requires its CBOR encoders to emit CTAP2 canonical encoding. CTAP2's ordering for integer and text keys is equivalent to ordering their canonical wire bytes lexicographically. Discovery found TPM differential fixtures with unsorted statement maps and malformed-input expectations that reflect the old codec. The existing fixture paths are outside the owned test directory. [WebAuthn Level 3](https://www.w3.org/TR/webauthn-3/#sctn-conforming-all-classes) [FIDO CTAP 2.2](https://fidoalliance.org/specs/fido-v2.2-ps-20250714/fido-client-to-authenticator-protocol-v2.2-ps-20250714.html#ctap2-canonical-cbor-encoding-form)

Acceptance uses the strict C++20 flags, app tests selected by `tools/links.py`, ASan/UBSan for the same closure, a 120-second libFuzzer smoke and a before/after timing and allocation comparison. Stop after scoped verification, the time-bounded broader sweep, and branch handoff. Exact commands, counts, measurements and open items follow as they become available.

## Initial evidence

- Base commit `55afcc8b13b2ddd10747355745da19e5264f7b76`, tree `5514adb01452ecaa20719fef73b6bf16c7e84b1a`.
- ARM64 Ubuntu 24.04 runner with GCC 14.2 and pinned Bun 1.4.0. `sudo -n unshare -n true` succeeded.
- The QCBOR codeload archive matched SHA-256 `286badcef7227533fcd5840386177ff20b1a53ca29f1544092b1fc36d020ade8`.
- The original registry has 441 binaries. `tools/links.py` resolves 58 existing test binaries with `src/auth/passkeys.cpp` in their closure.

## Continuation checkpoint

The inherited implementation at `3e496bc56494183e4ab2cab490864f9fd9bcf260` compiled and linked the two focused CMake binaries on Linux ARM64 with GCC 14.2.0, Boost 1.92.0 and the port's strict `-O2 -g0` flags. The vendored `inc/`, `src/` and `LICENSE` matched a fresh extraction of the pinned archive byte for byte. All 13 codec cases passed. The application replay ran 155 vectors in one case and reported 146 matches and nine mismatches.

| Mismatches                | Inherited fixture and strict result                                                                                                                                                 |
| ------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 2 TPM registrations       | The expected 200 responses became 400 because the synthetic `attStmt` maps begin with `ver` before `alg`. The replay needs canonical fixtures to reach the TPM verifier.            |
| 2 COSE text cases         | Registration uses `61 c3 a9` for the two-byte UTF-8 value `é`. The stored-key fixture uses a correct text length with positive key `07` after the negative keys. Both returned 400. |
| 1 authenticator extension | `a1 62 c3 a9 01` is canonical. The legacy re-encoding wrote the wrong text length before a second decode. QCBOR preserved the valid signed assertion, which returned 200.           |
| 4 diagnostic expectations | The array containing null, tagged extension, duplicate extension label and empty stored key still return 400. The warning messages now identify the strict decoder failures.        |

The existing passkeys parser fuzzer includes `src/auth/passkeys.cpp` directly. Its CMake object and link now select the same QCBOR target as the production translation unit. The generated test registry has 442 binaries, 59 with `src/auth/passkeys.cpp` in their closure. The remaining work is the release and ASan/UBSan closure, the new fuzz target's two-minute smoke, the broader release sweep, and the baseline measurement.

## Closure verification checkpoint

The inherited codec built at `28943ca1ca40dba9cf6901cbcbf8ac9d0a91aa4d` (`c9283e8ec2f48163acd92b6cb8b02b1d47b823d0`). The 59-test Links closure ran on ARM64 with GCC 14.2 at `-O2` and Clang 21 with ASan, UBSan and LeakSanitizer. Each configuration passed 58 binaries and 736 cases. The single failing binary contained the same nine WebAuthn replay mismatches listed above. The first Clang attempt stopped on headers in the existing simdutf vendor. A runner-local CMake setting treated those headers as system headers, with all first-party warning flags still enabled.

The portable `cmake/ci/driver.py` and legacy `tools/unit.mk` use a single C++ source per vendor. QCBOR needs five C objects and its include directory. The portable compile probe failed at the missing header, and `g++-14 -std=c++20 -fsyntax-only` with the include path confirmed that QCBOR's upstream C source requires the C frontend. The CMake and dedicated fuzz paths use that frontend. The portable driver and `tools/unit.mk` are outside this task's ownership.
