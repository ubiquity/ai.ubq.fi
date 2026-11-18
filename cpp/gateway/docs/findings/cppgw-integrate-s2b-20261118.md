# C++ gateway library integration, stage 2b

## Work contract v1

Start at the verified stage 1 tip `e90858809cf9cd733912be44077e97f949681c18`.
Merge the eight accepted branch tips from `origin` as two-parent commits in the
requested order. Reconcile overlapping code, vendor registrations and the
specified fixtures under `cpp/gateway/`. Preserve upstream vendor bytes.
The checkout handoff is the stage 2b branch for the maintainer's final integration.

| Surface | Required behavior |
| --- | --- |
| Slimming | Retain the supported console, environment, filesystem, Headers, multipart, ordered container and gzip contracts. |
| Fetch | Keep framing, limits, redirect and delivery behavior for supported HTTP requests. Reuse the stage 1 llhttp sources. |
| URL | Use Ada 4.0.0 for URL, IDNA and form operations. Retain origin, redirect and request validation at the gateway boundary. |
| Passkeys | Require canonical CBOR and retain authentication and attestation checks with OpenSSL's typed decoders. |
| BigInt and stored values | Keep supported numeric and stored representations, including sparse arrays, shared references and cycles. Verify input depth bounds. |
| Conformance | Compare the native and Bun gateways with isolated local mocks. Preserve wire behavior, statuses, accounting and validation. |

The owner policy permits standard rejection of malformed inputs and library
diagnostic wording. Preserve validation coverage and all valid vectors. Keep
TypeScript, hosted workflows, other `cpp/` paths and production deployment
outside the integration. Current observations are the fetched accepted tips,
their recorded fixtures and the known-good stage 1 receipts. Textual conflicts,
compiler diagnostics and the merged suite results are open at discovery.

Prepare ARM64 dependencies outside the checkout. Verify changed build
registries and vendor bytes, then run the four
`bash cpp/gateway/tools/ci-build.sh --config release --shard N/4` selections,
the affected `--config asan` tests and the documented Bun 1.4.0 conformance
suite. Use empty homes, fresh temporary directories and local mock traffic.
The stop condition is a clean handoff with eight ordered merges, exact receipts,
the first-party line-count comparison and any observed limitations.

## Initial evidence

Ubuntu 24.04.5, AArch64, four CPUs. The starting checkout matched the specified
stage 1 tip and all eight requested refs were fetched. Bun 1.4.0 is present.
Compiler and library preparation is in progress.
