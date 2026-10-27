# Exact pure error contract placement

Root ruling permits only the unchanged 67-line GatewayError contract as a defining DATA dependency.
`gateway-source/src/error.rs` is byte-identical to `413eb5d4:rust/crates/gateway-core/src/error.rs`:
SHA256 b5347e26660045ac80f9c5c0a7f576b0265aaa79dbe135725bc61e6176a6cd1b. All public fields,
constructors, status/code/omission/null semantics and Display bytes remain unchanged.

The already co-located source leaf is cohesive: its JSON/string/serialization primitives already return that exact
pure error DATA and need its identity without depending on Core. It has only serde, serde_json/raw_value and pinned
ryu-js dependencies; no HTTP client, host, auth, store, PortableValue or runtime edge. Core depends on gateway-source;
PortableValue conversion and its explicit physical UTF16 serde stay with Core. Auth DTO/services depend directly on the
same defining leaf, never an old-path reexport. A separate error-only package could remove no further cycle and would
only add a pure package boundary; no error behavior is generalized here. The consumer inventories record all defining
moves. Core shrinks by hundreds of owned lines including this change, rather than growing the existing oversized crate.

`gateway-source/tests/error_contract.rs` contains actual status, field, omitted/nullable param, type trim/default,
constructor, Display and exact JSON-byte goldens, plus malformed JSON/lone-unit cases. Existing Core golden source
serialization, all65536 units, source tuple and ProfileV2 fixtures are still reachable by direct defining imports in the
validation composition. Test execution receipts, rather than this document, determine their passing status. No
SourceRequestError, runtime normalization, D-private failure type or generic codec/store behavior is owned or changed.
