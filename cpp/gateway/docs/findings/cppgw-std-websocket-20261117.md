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
