# Boost.Beast 1.92 WebSocket qualification

## Contract v1

Qualify Beast for row 13 on `093a9e0511e9d0deae5495dc324e53ff4dccd719`, tree `cb6c6415cfd603f5aac070d424bc42b4642349e1`. Preserve the C++ socket API, gateway wire behavior and validation boundaries. Prefer the library's RFC 6455 and RFC 7692 behavior for generic operations. Adopt each independently qualified slice if the whole stream cannot satisfy those boundaries.

| Caller | Contract |
| --- | --- |
| Inbound HTTP Upgrade and live sideband | Authenticated upgrade, application response headers, pre-read frame bytes, ordered text and binary delivery, bounded queues, early hard message limit and route-specific failure close |
| Outbound live sideband | TCP/TLS with certificate checks and custom authorization headers, negotiated compression disabled, bounded messages, refusal status and close propagation |
| Codex supervisor and native auth | `ws+unix` JSON-RPC, explicit compression disablement, connection and payload limits, request-response ordering |

Writes are limited to the assigned C++ WebSocket paths and ranges, the job report, and any WebSocket test, fuzz, or dependency registration explicitly assigned to this job. The client HTTP response parser belongs to the HTTP lane. TypeScript, shared HTTP, provider and routing code, workflow files, and deployment are outside scope.

Gate the active paths with strict ARM64 compilation, the resolved WebSocket and application test closures, and ASan/UBSan. Compare release nanoseconds and C++ allocations per operation on identical inputs. Run a two-minute sanitizer fuzz campaign against public socket entry points. The spike will settle whether Beast can reject an oversized message at the frame header and let the route choose the close frame. The work ends with the qualified implementation, source and test receipts, and a clean committed job branch for the port integration.

## Discovery

- `runtime/http_upgrade.cpp` overrides a bare receiver 1009 close and sends the route's code and reason. `runtime_http_test.cpp` and `live_sideband_http_test.cpp` assert 1011 with the application reason and zero oversize message deliveries.
- The existing receiver tests the cumulative declared size before consuming the mask or payload. Beast's public stream has a message limit and a passive control callback. A header-only loopback spike will verify when its limit sends the close frame relative to completion.

## Contract v2 and spike result

Adopt Beast's isolated RFC 6455 primitives for outbound header encoding, masking in both directions, UTF-8 and close-code validation, and the handshake accept token. The existing WebSocket and HTTP lifecycle and all acceptance gates in v1 still apply. The full-stream spike is evidence for the decision below. It does not qualify the narrower production change.

A Beast 1.92 server accepted the RFC sample handshake in paired in-memory streams. With a 1,024-byte limit, a masked frame header declaring 1,025 bytes and carrying no payload triggered `message_too_big`. The close frame was already present at completion as `88 02 03 f1` (1009, empty reason), with zero control callbacks. Beast sends this close before its teardown and before the caller gets the error. A read completion cannot substitute the gateway's close frame. An incremental payload count would wait for oversized payload bytes and lose the existing header-time rejection. Preserving both obligations with the full stream would require intercepting Beast's frame bytes or changing its read implementation.

The spike also observed that Beast accepted `Sec-WebSocket-Key: ?` in a pre-parsed request. The existing strict request validation therefore remains material. Beast's stream requires a Host header on parsed server requests. The Codex Unix client currently connects without one unless configured. The HTTP response-head parser and its pre-read byte transfer remain part of the existing client handshake. See `websocket::stream::control_callback`, `impl/read.hpp`, and `impl/accept.hpp` in the pinned release. [Boost 1.92.0](https://www.boost.org/releases/1.92.0/)

## Contract v3 and continuation

The inherited source was `73d0f35f3d810ca6245e45639ca085de58a960fc`, tree `b1157e3d74454bd84e739d0ae0de0edc56784346`. The focused Clang 21.1.8 `-O2` build on Linux ARM64 passed all three WebSocket binaries and 23 cases. It used the official Boost 1.92.0 source archive with SHA-256 `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c`. [Upstream archive metadata](https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2.json)

Reproduced the earlier result with `websocket::stream<beast::test::stream>` and `async_read`. A masked binary header with length 1,025, the four mask bytes, and zero payload bytes caused Beast to emit exactly four bytes for close 1009 before completing with `message_too_big`. The completion reported zero payload bytes and zero control callbacks. A bounded caller buffer would detect a large message after receiving payload. A transport adapter capable of changing the automatic close would have to recognize and replace Beast's serialized frame. Keep the header-time limit and virtual close hook for the live relay and HTTP Upgrade contract. A new frame interceptor or a second production framing path would retain the implementation being evaluated.

Extend the qualified slices to client nonce and masking-key generation, received close payload decoding, server nonce validation, and subprotocol tokenization. Retain explicit checks for the nonce's encoded shape, malformed and duplicate subprotocols, and the allowed close-code range. Apply Beast's UTF-8 validation even when a legacy caller sets `skipUTF8Validation`. Preserve wire status and headers, pre-read data, connection lifetimes, configured payload limits, and the client and server option types. The existing caller matrix and path ownership still apply. All gateway callers disable compression, so there is no active deflate implementation to replace.

Run the inherited and final WebSocket closures under the strict port flags. The wider optimized and instrumented builds compile each distinct source once and run the registry under credential-free Linux fixture isolation. Check each receipt against the committed tree, report failures outside this lane, and record exact scope and count for anything the runner cannot finish. Replay the baseline and final frame and handshake workloads with the same compiler and allocator counter. A 120-second WebSocket fuzz run and a clean integration-branch handoff close this continuation.

## Contract v4 and resumed build

The inherited worktree contained an unfinished v3 patch. The focused `-O2` replay passed 3 binaries and 23 cases on that patch, after the earlier continuation had corrected its Beast iterator and socket helper regressions. This continuation qualifies those same production slices and adds a focused validation fixture under the assigned legacy test directory. The client response-head parser remains owned by the separate HTTP lane. The caller matrix in v1 still applies. Both Codex Unix transports explicitly supply `Host: localhost`. Live sideband sets a 4 MiB hard limit and sends `1011` with `realtime sideband frame exceeds payload limit` when the inbound header declares an oversized message.

Run the release registry and instrumented WebSocket and application closures, then extend sanitizer coverage as runner time permits. Use separate temporary output trees, the in-repository Linux namespace fixture boundary, Clang 21 strict flags, and the existing Boost 1.92 archives. A sweep can take several minutes on four cores. Track object and test counts in the driver logs, replay any tooling or dependency failures in the smallest affected closure, and keep unrelated source defects in this report. Stop at a clean task branch with the wire contract, validation, benchmark, fuzz and test receipts bound to committed source. Service activation and integration of other C++ port work are outside this job.
