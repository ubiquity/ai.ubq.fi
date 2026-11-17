# Passkey CBOR replacement findings

## Work contract v1

Replace the row 12 passkey CBOR parser, value model and serializer with the verified libcbor 0.15.0 release. The
baseline is `093a9e0511e9d0deae5495dc324e53ff4dccd719` (tree `cb6c6415cfd603f5aac070d424bc42b4642349e1`).
Keep the public passkey handler API, stored COSE credentials, and signature verification inputs compatible.
Application routing, provider behavior, credential changes, certificate parsing and deployment are out of scope.

| Surface | Required behavior |
| --- | --- |
| Attestation and stored COSE input | Decode the first complete CBOR item. Preserve the accepted key and value types and reject ambiguous maps and malformed input. |
| Authenticator data | Parse the credential and extensions at their wire offsets. Preserve the Firefox EdDSA handling, leftover-byte rejection and signature inputs. |
| Encoding | Use libcbor serialization for CBOR items. Standard primitive encodings take precedence over incidental JavaScript conversions. |
| Resource bounds | Reject unbounded nesting and impossible declared sizes. Keep new acceptances out of untrusted WebAuthn paths. |

Writes are limited to the passkey CBOR slice and its model consumers, the libcbor vendor and the three registration
files, the CBOR fuzz target and seeds, owned tests and link declarations, the deviations section and this report.
Acceptance covers warning-fatal C++20 builds, every resolved passkey consumer under plain and ASan/UBSan, an additional
suite sweep as time permits, a 120-second sanitizer fuzz run, and old/new latency and allocation measurements. The
contract closes when the exact tree and results are recorded and committed for the port integration. Remaining discovery
includes width and cursor differences on the recorded fixtures and C-source support in the existing vendor build paths.

## Initial observations

- The runner is Ubuntu 24.04 ARM64 with Clang 21.1.8, GCC 14.2, and Bun `1.4.0+34cbb9a40`. The official Boost 1.92.0
  archive built the JSON and Container static libraries under scratch.
- The libcbor tag archive matched `c94145d3c22e67cff9a3325dc036b4545a49a42231553995be4947d52cf2ef57`.
- The runner has read access to the C++ port branch. The GitHub push dry run failed because no write credential was
  available to Git. The checkout is the isolated `agent-job` branch at the port baseline.
