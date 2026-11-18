# C++ gateway final integration F1

## Work contract v1

Start at cross-line tip `6d28e858e7ab8b8b1036db4a4adfdade0465ed40`. Merge the fetched stage 2b, macOS release, and
README/architecture tips in that order as ordinary two-parent merges. Build and test each integrated tree before the
next merge. Record textual conflicts, semantic reconciliations, and expectation changes as they are resolved.

| Surface | Integration decision |
| ------- | -------------------- |
| JSON and text | One simdjson JSON parser with the stage 2b depth cap, the shared input limits, simdutf text and ICU Unicode handling. |
| HTTP and WebSocket | llhttp for the inbound HTTP and outbound fetch boundaries. Keep Beast's WebSocket client and handshake, and preserve close, framing and delivery decisions across supported Boost versions. |
| Runtime and storage | Take the stage 2b helper slimming, Ada, QCBOR, OpenSSL, Boost.Multiprecision and sparse/cyclic KV changes. Preserve strict input validation, the stored codec and graph bounds. |
| Gateway behavior | Preserve wire, status, authentication, validation, quota and accounting contracts. Compare packaged native and Bun gateways with the same local mocks. |
| Dependencies and release | Register each vendored library once in every build and packaging path. Preserve vendor bytes. Retain the default Linux confinement requirement and the explicit reduced-confinement opt-in. |

Edits are confined to `cpp/gateway/` and the gateway workflows as needed. TypeScript, other `cpp/` paths, credentials,
production service state, and live provider traffic are out of scope. Test processes use empty homes, fresh temporary
directories, a credential-free environment and local mocks through the existing OS fixture boundaries. Dependencies,
downloads and build products stay outside the checkout.

The starting evidence is 445/445 Release binaries and 5,054/5,054 cases on cross-line, and 448/448 and 5,086/5,086
on stage 2b. Stage 2b reports full paired parity with two shared validation failures. Conflicts, final inventory, the
WebSocket Boost discrepancy and the target-specific package results remain open at discovery.

Acceptance covers all four Linux ARM64 Release shards with `bash cpp/gateway/tools/ci-build.sh --config release
--shard N/4`, focused ASan/UBSan/LSan coverage, `python3 cpp/gateway/tools/links.py --check`, paired packaged
conformance, the portable package verifier, and the workflow's `kv_jsc` fuzz command for at least 600 seconds.
Capture exact Git commits and trees, vendor comparisons, case counts, timings, and production source counts. The
stop condition is a clean, verified handoff with all three ordered merges and any concrete blockers identified.

## Starting refs

| Input | Fetched tip |
| ----- | ----------- |
| Stage 2b | `af7a0036b823ef2357c26b9b5f03f2677796b775` |
| macOS release | `980d97fb0c90bd6937c06d4f9e3b25bc1d52a9e1` |
| README and architecture | `20c83e02258a5435b6ebc9f16c7b927d3b111407` |

The checkout was clean on `agent-job`. The host is Ubuntu 24.04.5, Linux ARM64, with four CPUs and 15 GiB RAM.
Passwordless network namespace setup and Docker 28.0.4 are available. Bun reports 1.4.0.
