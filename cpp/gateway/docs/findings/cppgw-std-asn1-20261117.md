# Passkey ASN.1 and X.509 replacement

## Work contract v1

Replace the BER reader and the WebAuthn certificate, CRL, signature and extension decoders with the OpenSSL 3 APIs already linked by the gateway. The C++ passkey handler API and the CBOR, WebAuthn policy, transport, storage, and trust-anchor code are outside this replacement. The integration branch is `claude/cpp-gateway-port`, with job base `093a9e0511e9d0deae5495dc324e53ff4dccd719`.

| Surface                                        | Required result                                                                                                                                                                  |
| ---------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| ECDSA assertions and attestation signatures    | Valid DER integers retain the same verification result. OpenSSL framing or integer errors produce ordinary gateway errors.                                                       |
| X.509 chain and COSE projection                | Keep the supported key and signature algorithms, signed certificate data, validity and trust decisions.                                                                          |
| Standard X.509 extensions                      | Preserve the registration, TPM and CRL decisions, including AAGUID equality and extension criticality.                                                                           |
| Android Key, Apple and FIDO extension payloads | Decode the private ASN.1 shapes through OpenSSL and enforce the existing challenge, nonce and application-scope checks.                                                          |
| CRLs                                           | Preserve revocation checks, serial identity, caching and fetch selection. A decoding difference must never allow a certificate through an otherwise applicable revocation check. |

Production ownership covers the ASN.1 and X.509/CRL regions and their decoder call sites in `cpp/gateway/src/auth/passkeys.cpp`. The evidence and test ownership covers this report, the component's deviation section, a legacy regression target with its link declaration, and the component fuzz target and corpus. The CBOR slice and all TypeScript, CI and tool sources are outside ownership. No new vendor or application dependency is required.

The acceptance surface is a strict Linux ARM64 build, the `tools/links.py` passkey closure in release and ASan/UBSan profiles, the WebAuthn replay corpus, a 120-second sanitizer fuzz campaign, and an optimized comparison against the job base with allocation counts. Tests run in isolated processes with an empty home and temporary directory. The scope also permits running additional unmodified test targets using the shared objects as time allows. Parser constraints and fixture compatibility will be checked against the previous source, the installed OpenSSL, and Bun 1.4.0 when a JavaScript result needs confirmation.

Builds use fresh scratch space outside the checkout with up to four compiler jobs. The expected time for cold dependency setup and the closure builds is 10–25 minutes, with link failures and test results recorded per target. A failed build is diagnosed from its first compiler or linker error before a scoped retry. The fuzz runner has one timed campaign plus build time. The stop condition is a committed source, test and fuzz change with a final report of measured results and any unresolved integration or verification work.

## Continuation and work contract v2

The continuation starts at `ddd77afaa9f2e91a1de816765dbda33b409cf728` (tree `84161a0240dad495bce9f15e83a0b3698e324d15`). The inherited implementation and three focused test binaries built with the strict warnings under Clang 21.1.8 and Boost 1.92.0 on Linux ARM64. The first test batch passed 12 of 13 harness cases. The failing harness replayed 155 WebAuthn vectors. Sixteen malformed ECDSA inputs differed only in the decoder warning. All HTTP responses, headers, throws, storage and fetch observations matched. The replay vector file is outside the listed writable paths, so an exception for those 16 expectations is pending.

The acceptance matrix in v1 still applies. The continuation also checks that X.509 Name projection cannot silently discard an empty RDN and that a rejected CRL encoding cannot bypass a revocation check through a non-2xx response. Bun 1.4.0 and OpenSSL 3.0.13 confirmed the Name discrepancy. `30 02 31 00` has one empty RDN in `@peculiar/asn1-x509`. OpenSSL's name iterator returns zero entries and its cached DER retains the empty set. The gateway rejects that encoding before projecting the name.

The portable checks resolve their test list with `tools/links.py` and reuse the compile, vendor and OS fixture isolation methods from `cmake/ci/driver.py`. The small smoke closure has 61 first-party translation units and two vendor units. The full passkey closure selects 59 binaries. It is the next release and ASan/UBSan gate, followed by more of the full release inventory as time allows. The first link attempt failed before the test harnesses started because the private `/tmp` mount hid the SQLite shared object. The relink places the SQLite library beside the binary in the existing bind mount and uses an `$ORIGIN` search path. The acceptance runs use this isolated layout.

## Implementation and source scope

OpenSSL owns ASN.1 framing, ECDSA scalar decoding, OCTET STRING decoding, X.509 and CRL structures, PEM extraction, signature verification, RSA public-key components, standard X.509 extensions, and the Android KeyDescription and Apple nonce templates. The gateway retains a bounded DER preflight using `ASN1_get_object` to enforce complete input, resource ceilings and primitive string framing before typed decoding can normalize them. Gateway adapters retain the supported COSE mappings and signature schemes, the original signed certificate bytes used by `X509_verify`, the first CRL distribution point and GeneralName, the existing trust-anchor traversal, and the attestation-specific challenge, nonce and AAGUID checks. The CRL adapter retains the cache and fails closed on malformed published lists and on non-2xx replies whose body has a list envelope.

Compared with `093a9e0511e9d0deae5495dc324e53ff4dccd719`, `git diff --numstat` for `src/auth/passkeys.cpp` records 1,101 deleted production lines and 467 added, a net reduction of 634. The byte range from the tiny-cbor heading through `isoCBOR` has SHA-256 `fa496e42a74a5a39c87d66a9ecb790a26e4be3faa3e439fe70c412d5c5ddb5ce` at both revisions. The existing OpenSSL linkage is sufficient. The fuzz registry adds one target with nine checked-in seeds. [Recorded behavior differences](../DEVIATIONS-FROM-BUN.md)

## Runner commands and focused results

The runner was Ubuntu 24.04.5 on AArch64 with four CPUs. The compiler was Clang 21.1.8 with libstdc++ 14 and mold 2.30.0. The runner's OpenSSL was 3.0.13, ICU 74.2, zlib 1.3, Brotli 1.1.0 and Zstd 1.5.5. SQLite 3.54.0 was verified against `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` and built with `./configure --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'`. Boost 1.92.0 came from `archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2` with SHA-256 `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c`, verified against the published release metadata. Build commands used `./bootstrap.sh --with-libraries=json,container` and `./b2 -j4 toolset=gcc cxxstd=20 variant=release link=static threading=multi --with-json --with-container stage`.

The scratch driver at `/tmp/passkey-asn1.H9gLQs/run-tests.py` imports `cmake/ci/driver.py`. It resolves each path through `tools/links.py`, builds selected objects with `ninja -k 0 -j <jobs>`, uses the registered simdjson and simdutf archives, and runs the existing fixture namespace helper. It includes every `src/rt/*.cpp` in each link. `smoke` selects the new legacy unit binary, `passkeys_test` and the WebAuthn replay. `closure` selects those plus the larger passkey test binaries and every other test whose resolved link list includes `src/auth/passkeys.cpp`. Commands ran from the repository root.

```sh
python3 -B /tmp/passkey-asn1.H9gLQs/run-tests.py release smoke
python3 -B /tmp/passkey-asn1.H9gLQs/run-tests.py asan smoke --jobs 2
python3 -B /tmp/passkey-asn1.H9gLQs/replay-warnings.py release
python3 -B /tmp/passkey-asn1.H9gLQs/replay-warnings.py asan
python3 -B /tmp/passkey-asn1.H9gLQs/run-tests.py release closure --jobs 2
python3 -B /tmp/passkey-asn1.H9gLQs/run-tests.py asan closure --jobs 2
```

The first-party compiler arguments were `-std=c++20 -pthread -Wall -Wextra -Wshadow -Wconversion -Werror -DBOOST_JSON_NO_LIB -DBOOST_CONTAINER_NO_LIB -DBOOST_SYSTEM_NO_LIB -DBOOST_ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING -DUOS_GATEWAY_NATIVE_LIBRARIES=1`, with the gateway include paths and Boost/SQLite system includes. Both explicit profiles select simdjson for `src/rt/json.cpp`. Release adds `-O2 -g0`. The sanitizer profile adds `-O1 -gline-tables-only -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all`. The driver enabled leak detection, required a loopback-only network namespace with a checked outside-route denial, and set an empty HOME and fresh TMPDIR for each test. Its environment allowlist excluded all provider variables.

The follow-up source and test commit is `3e2aa5766a0bf0173263c6054c5e269d5828ac64` (tree `968728d69a5e2031de95c574321363ca9a77db7e`). Its release and ASan/UBSan smoke builds each passed 13 of 14 harness cases across 2 of 3 binaries, with the sole failure being the unchanged warning assertions. The scratch replay used the original C++ harness and all 155 vectors. It adjusted only the 16 warning values against the measured output and asserted that every other vector field was identical. That replay passed 1 of 1 harness cases in both profiles. Bun 1.4.0 was checked directly with `bun --no-env-file --no-install bind-probes.ts`, using the root frozen lockfile's `@peculiar/asn1-schema`, `@peculiar/asn1-ecc` and `@peculiar/asn1-x509` in the scratch checkout.
