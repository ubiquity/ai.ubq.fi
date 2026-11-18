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

## Merge 1 — stage 2b

| Git conflict | Resolution |
| ------------ | ---------- |
| `cmake/ci/driver.py` | Combine per-vendor include flags and the explicit Ninja dependencies on every registered vendor header, including QCBOR's nested includes. |
| `cmake/vendors.json` | Keep a single llhttp entry for server, fetch and WebSocket response-head consumers. Add Ada and QCBOR. Keep all three simdutf consumers. |
| `docs/DEVIATIONS-FROM-BUN.md` | Keep the cross-line deviations and add the runtime, library and depth decisions from stage 2b. Describe the Beast and llhttp boundaries. |
| `fuzz/ci.py` | Use the same verified vendor registry as the other build paths. Keep the matching C frontend, library link flags, timed campaign receipt and finding classification. |
| `tests/rt/json_test.cpp` | Keep cross-line's document-size and array/object depth rejection cases. Keep stage 2b's value-copy coverage and exercise both arrays and objects. |

The automatic WebSocket client merge retains Beast's handshake key and accept digest and uses the existing llhttp
response-head adapter where the socket transport needs HTTP parsing. The merged JSON parser remains simdjson.
`kJsonMaxDepth` now uses the central 128-container constant for recursive operations on JSON values. KV keeps
the separate 2,000-container graph bound and the sparse/cyclic reader. The CMake, SDK, macOS, benchmark, and
fuzz build paths use the selected vendor inputs once per build closure.

The three stage 2b parser rejection assertions at depth 129 expected `RangeError` with `Maximum call stack size
exceeded.` The merged parser expects the cross-line `SyntaxError` with `Invalid JSON` for all three inputs.
The stringify, comparison and coercion tests continue to require `RangeError` when given values past the limit.
The stage 2b depth-related Codex 429, BigInt, fetch and WebAuthn fixture changes are recorded in its
[findings](cppgw-integrate-s2b-20261118.md#fixture-decisions).

The original stage 2b merge compiled 369 production objects after two portability corrections for
Boost.JSON array construction and `std::min` argument types. The stage 2b replay on the prior parser
promoted integer-index keys in two generic console vectors. The merged simdjson and console path
prints both full objects in insertion order. The vectors now expect `4294967294` last (formerly first)
and `1` between `valid_id` and `with space` (formerly first). Both inputs and all fields are retained.
Pre-serialized gateway log records and terminal accounting fields use their existing paths.

## WebSocket close code 1014

On ARM64 `ubuntu:26.04`, Clang 21.1.8 and distro Boost `1.90.0-6ubuntu1`, a direct
`boost::beast::websocket::detail::is_valid_close_code` probe returned false for the IANA-registered 1014
(Bad Gateway). The same call returned true with staged Boost 1.92. The 1.90 receive helper also reported an
invalid close code for a 1014 frame. The gateway now handles 1014 in its shared send and receive validator,
uses Boost.Endian to read the code, and uses Beast to validate the reason's UTF-8. The original assertion
still requires the client to enter CLOSING without throwing and send exactly one masked 1014 frame.
Two new received-frame examples check a valid 1014 reason and rejection of a malformed UTF-8 reason.

## Stage 2b release gate

All four release shards passed on `7b602c71` after the console fixture correction. The counts by shard were
113/1,332, 113/1,233, 112/1,439 and 112/1,100, for 450 binaries and 5,104 cases. The receipts are under
`/var/tmp/cppgw-integrate-f1.WpF47zpW/receipts/stage2b/`.

## Merge 2 — release packaging

| Git conflict | Resolution |
| ------------ | ---------- |
| `packaging/portable/payload.py` | Keep the release branch's timestamps, `VERSION`, native build receipt and installation files. Derive all linked vendor versions from the same registry used to collect their licenses. |
| `tools/macos-build.sh` | Keep expanded pkg-config system include paths, matching C/C++ compilers, system treatment for all vendor headers, and explicit header dependencies for each C and C++ vendor source and consumer. |

The Docker build context retains the llhttp consumer and adds the direct QCBOR test consumer. Portable Linux
payloads include the five linked vendor licenses and notices where supplied, SQLite's public-domain notice,
Boost and ICU licenses, Rust addon notices and the Debian copyright files for the linked toolchain and system
libraries. The macOS package uses the same vendor registry for versions and includes each linked vendor license.
The Linux release instructions describe the reduced-confinement opt-in.
