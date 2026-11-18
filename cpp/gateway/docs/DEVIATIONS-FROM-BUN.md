# C++ gateway deviations from Bun

## Passkey CBOR

- Integer and length headers use their shortest encoding. For example, `19 00 18` is rejected because 24 fits in `18 18`. QCBOR supplies the decoded argument and the CTAP2 check verifies its encoded width.
- Map labels must be in CTAP2 order and unique. For example, `a2 02 00 01 00` is rejected because label 1 must precede label 2. Text and integer labels retain their separate types. Leading UTF-8 BOMs in labels are rejected to preserve the prior duplicate-label protection.
- The decoder admits booleans, integers in the previously supported range, byte and text strings, arrays and maps. Tags, null, undefined and other simple values and floats are rejected. For example, a half-precision `-7.0` (encoded as `f9 c7 00`) cannot stand in for the COSE algorithm identifier `-7`.
- Text uses its UTF-8 byte length. The text `é` encodes as `62 c3 a9`. The old encoder used a length of one UTF-16 code unit. Invalid UTF-8 and truncated byte strings are rejected instead of producing a replacement string or a clamped slice.
- Each input is limited to one MiB and eight levels of container nesting. A ninth nested array is rejected even when it is empty. QCBOR also limits the element count of individual arrays and maps.
- A complete attestation object or stored COSE key occupies its entire input. For example, `a0 00` is rejected as a complete item and consumes one byte in prefix mode. Authenticator data uses prefix mode for its COSE key and extensions and checks the final cursor. The Firefox 117 EdDSA map-count correction remains in the authenticator data path for that exact browser encoding.
- Decode errors carry the port's `JsError` type and CBOR validation reason. For example, empty input reports `Invalid passkey CBOR (truncated item)`. The gateway continues to map failed verification to its existing response statuses.
