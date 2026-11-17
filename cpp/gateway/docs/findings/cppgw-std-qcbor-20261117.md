# Passkey CBOR replacement

## Work contract v1

Replace the translated tiny-cbor value model and codec with QCBOR 1.6.1. Keep WebAuthn verification decisions, credential storage and the original signed authenticator bytes at their existing boundaries. This change owns the CBOR region and necessary model call sites in `src/auth/passkeys.cpp`, the QCBOR vendor directory and its build registration, and the scoped CBOR tests, fuzz target and findings. ASN.1, X.509, TypeScript, other gateway modules, workflows and deployment are outside the change.

| Input | Required behavior |
| --- | --- |
| Attestation object and stored COSE key | Decode exactly one canonical item and reject trailing data. |
| COSE key and extensions embedded in authenticator data | Decode the first canonical item, advance by consumed wire bytes, and enforce the existing final boundary. |
| Untrusted CBOR | Enforce minimal widths, definite lengths, CTAP2 key order, unique integer or text keys, valid UTF-8, the supported WebAuthn types, eight container levels and one MiB. |
| Existing Firefox 117 EdDSA attestation | Retain the exact known map-count correction before decoding and preserve the signed input bytes. |

WebAuthn Level 3 requires its CBOR encoders to emit CTAP2 canonical encoding. CTAP2's ordering for integer and text keys is equivalent to ordering their canonical wire bytes lexicographically. Discovery found TPM differential fixtures with unsorted statement maps and malformed-input expectations that reflect the old codec. The existing fixture paths are outside the owned test directory. [WebAuthn Level 3](https://www.w3.org/TR/webauthn-3/#sctn-conforming-all-classes) [FIDO CTAP 2.2](https://fidoalliance.org/specs/fido-v2.2-ps-20250714/fido-client-to-authenticator-protocol-v2.2-ps-20250714.html#ctap2-canonical-cbor-encoding-form)

Acceptance uses the strict C++20 flags, app tests selected by `tools/links.py`, ASan/UBSan for the same closure, a 120-second libFuzzer smoke and a before/after timing and allocation comparison. Stop after scoped verification, the time-bounded broader sweep, and branch handoff. Exact commands, counts, measurements and open items follow as they become available.

## Initial evidence

- Base commit `55afcc8b13b2ddd10747355745da19e5264f7b76`, tree `5514adb01452ecaa20719fef73b6bf16c7e84b1a`.
- ARM64 Ubuntu 24.04 runner with GCC 14.2 and pinned Bun 1.4.0. `sudo -n unshare -n true` succeeded.
- The QCBOR codeload archive matched SHA-256 `286badcef7227533fcd5840386177ff20b1a53ca29f1544092b1fc36d020ade8`.
- The original registry has 441 binaries. `tools/links.py` resolves 58 existing test binaries with `src/auth/passkeys.cpp` in their closure.
