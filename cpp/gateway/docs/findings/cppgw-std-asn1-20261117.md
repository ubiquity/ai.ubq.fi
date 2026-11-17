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
