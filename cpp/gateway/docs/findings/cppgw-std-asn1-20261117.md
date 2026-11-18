# Passkey ASN.1 and X.509 replacement

## Work contract v1

Replace the BER reader and the WebAuthn certificate, CRL, signature and extension decoders with the OpenSSL 3 APIs already linked by the gateway. The C++ passkey handler API and the CBOR, WebAuthn policy, transport, storage, and trust-anchor code are outside this replacement. The integration branch is `claude/cpp-gateway-port`, with job base `093a9e0511e9d0deae5495dc324e53ff4dccd719`.

| Surface | Required result |
| --- | --- |
| ECDSA assertions and attestation signatures | Valid DER integers retain the same verification result. OpenSSL framing or integer errors produce ordinary gateway errors. |
| X.509 chain and COSE projection | Keep the supported key and signature algorithms, signed certificate data, validity and trust decisions. |
| Standard X.509 extensions | Preserve the registration, TPM and CRL decisions, including AAGUID equality and extension criticality. |
| Android Key, Apple and FIDO extension payloads | Decode the private ASN.1 shapes through OpenSSL and enforce the existing challenge, nonce and application-scope checks. |
| CRLs | Preserve revocation checks, serial identity, caching and fetch selection. A decoding difference must never allow a certificate through an otherwise applicable revocation check. |

Production ownership covers the ASN.1 and X.509/CRL regions and their decoder call sites in `cpp/gateway/src/auth/passkeys.cpp`. The evidence and test ownership covers this report, the component's deviation section, a legacy regression target with its link declaration, and the component fuzz target and corpus. The CBOR slice and all TypeScript, CI and tool sources are outside ownership. No new vendor or application dependency is required.

The acceptance surface is a strict Linux ARM64 build, the `tools/links.py` passkey closure in release and ASan/UBSan profiles, the WebAuthn replay corpus, a 120-second sanitizer fuzz campaign, and an optimized comparison against the job base with allocation counts. Tests run in isolated processes with an empty home and temporary directory. The scope also permits running additional unmodified test targets using the shared objects as time allows. Parser constraints and fixture compatibility will be checked against the previous source, the installed OpenSSL, and Bun 1.4.0 when a JavaScript result needs confirmation.

Builds use fresh scratch space outside the checkout with up to four compiler jobs. The expected time for cold dependency setup and the closure builds is 10–25 minutes, with link failures and test results recorded per target. A failed build is diagnosed from its first compiler or linker error before a scoped retry. The fuzz runner has one timed campaign plus build time. The stop condition is a committed source, test and fuzz change with a final report of measured results and any unresolved integration or verification work.

## Continuation and work contract v2

The continuation starts at `ddd77afaa9f2e91a1de816765dbda33b409cf728` (tree `84161a0240dad495bce9f15e83a0b3698e324d15`). The inherited implementation, private-decoder tests and fuzz target built with the strict warnings under Clang 21.1.8 and Boost 1.92.0 on Linux ARM64. The first test batch passed 12 of 13 harness cases from 3 binaries. The failing harness replayed 155 WebAuthn vectors. Sixteen malformed ECDSA inputs differed only in the decoder warning. All HTTP responses, headers, throws, storage and fetch observations matched. The replay vector file is outside the listed writable paths, so an exception for those 16 expectations is pending.

The acceptance matrix in v1 still applies. The continuation also checks that X.509 Name projection cannot silently discard an empty RDN, and that a rejected CRL encoding cannot bypass a revocation check through a non-2xx response. These checks stay in the existing owned decoding regions. Bun 1.4.0 and OpenSSL 3.0.13 confirmed the Name discrepancy. `30 02 31 00` has one empty RDN in `@peculiar/asn1-x509`; OpenSSL's name iterator returns zero entries and its cached DER retains the empty set. A strict preflight can reject that encoding before the gateway projects the name.

The portable checks resolve their test list with `tools/links.py` and reuse the compile, vendor and OS fixture isolation methods from `cmake/ci/driver.py`. The small smoke closure has 61 first-party translation units and two vendor units. The full passkey closure selects 59 binaries. It is the next release and ASan/UBSan gate, followed by more of the full release inventory as time allows. The first link attempt failed before the test harnesses started because the private `/tmp` mount hid the SQLite shared object. The relink places the SQLite library beside the binary in the existing bind mount and uses an `$ORIGIN` search path. The acceptance runs use this isolated layout.
