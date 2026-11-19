# Native gateway architecture

The C++20 gateway ports the repository's TypeScript application. It uses native libraries for generic operations
and keeps the gateway's wire formats, authentication, validation, accounting and storage contracts. The owner guide
summarizes platform evidence and how to try it. [TypeScript modules](https://github.com/ubiquity/ai.ubq.fi/tree/2b23fb6837e2b5c0dc9ffa50771f52d322dd413a/src) ·
[TypeScript entry point](https://github.com/ubiquity/ai.ubq.fi/blob/2b23fb6837e2b5c0dc9ffa50771f52d322dd413a/serve.ts) · [Owner guide](../README.md)

## Layers

| Layer          | Responsibility                                                                                                                                                                                                         | Start here                                                                                    |
| -------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------- |
| Executables    | `uos-gateway` starts the service with explicit package and state bindings.                                                                                              | [entrypoints/](../entrypoints/)                                                               |
| Startup        | Reads the private launch manifest, verifies the package and host dependencies, applies Linux confinement, opens the marked store and starts the application.                                                           | [runtime/startup.cpp](../runtime/startup.cpp)                                                 |
| Server runtime | Adapts TCP and HTTP to application requests, streams responses, upgrades WebSockets and reports the outcome of socket writes.                                                                                          | [src/runtime/](../src/runtime/), [src/serve.cpp](../src/serve.cpp)                            |
| Application    | Owns routes, authentication, schemas, provider selection, quotas, admin controls and persistence policy. Headers in `include/uos/` mirror the implementation tree. Many paths and names follow the TypeScript modules. | [src/](../src/), [include/uos/](../include/uos/), [PORT-MANIFEST.json](../PORT-MANIFEST.json) |
| `uos::rt`      | Gives the port C++ equivalents of the runtime APIs it calls, including Fetch, streams, promises, JSON values and KV.                                                                                                   | [include/uos/rt/](../include/uos/rt/), [src/rt/](../src/rt/)                                  |

Asio handles asynchronous I/O. llhttp parses incoming HTTP and outbound response heads. Beast provides WebSocket
handshake and frame primitives, with llhttp supplying the client's HTTP response parsing. Ada handles URL, IDNA
and URL search parameters. simdjson parses JSON into owned Boost.JSON values, simdutf handles Base64 and bulk
text conversion, and ICU supplies Unicode casing and collation. QuickJS-ng libregexp compiles and matches ECMAScript
patterns, including Unicode sets. QCBOR decodes and encodes passkey CBOR, and OpenSSL supplies TLS, crypto and typed
passkey ASN.1 decoding. Boost.Multiprecision supplies BigInt arithmetic. SQLite stores the KV rows, zlib, Brotli and
Zstd handle HTTP content decoding, and C++20 and POSIX supply filesystem paths, numeric conversion, dates and
collections. The adapters retain input bounds, backpressure, UTF-16 offsets, storage value encoding and delivery receipts.
[Library versions and provenance](../README.md#libraries-in-use) · [Vendor selection](../cmake/vendors.json)

## From client socket to provider and back

1. **Accept the connection.** `serve.cpp` supplies the application's HTTP and upgrade callbacks. `serveHttp` accepts
   TCP sockets through Asio. It checks the request head and framing and wraps the incoming body as a readable stream
   on an `rt::Request`. The same runtime handles negotiated socket upgrades for live routes.
   [Server wiring](../src/serve.cpp) · [HTTP transport](../src/runtime/http.cpp) ·
   [Incoming body](../src/runtime/http_body.cpp) · [Upgrades](../src/runtime/http_upgrade.cpp)
2. **Choose the route and apply policy.** The serve handler passes a request with its delivery receipt and abort
   signal to the application. The router handles health, assets, auth and admin routes and sends `/v1/*` requests
   to the inference router. That router authenticates the client and applies the route's admission and quota checks.
   The Chat Completions and Responses handlers read bounded bodies, validate and normalize their fields, and prepare
   the upstream payload. The ordinary models list and the versioned Codex catalog have separate response shapes.
   [Router](../src/handler/index.cpp) · [Inference routing](../src/handler/terminal_route.cpp) ·
   [Body validation](../src/request.cpp) · [Chat](../src/chat/envelope.cpp) · [Responses](../src/responses_handler.cpp) ·
   [Models list](../src/models/catalog.cpp) · [Codex catalog](../src/catalog/index.cpp)
3. **Dispatch upstream.** The request path selects the provider for the model and route. For ordinary Codex
   inference, account routing keeps a shared active subscription. Codex unavailability and paid-tier quota signals
   are classified before advancing through the eligible fallback order of Surplus Intelligence and OpenLux. The
   provider adapter sends the request through the native HTTP client or a WebSocket where the route calls for one.
   The HTTP client uses Asio for sockets and OpenSSL for upstream TLS.
   [Responses attempts](../src/responses_attempts.cpp) · [Codex dispatch](../src/codex/dispatch.cpp) ·
   [Subscription routing](../src/codex/routing_serial.cpp) · [Paid routing](../src/paid_fallback/routing.cpp) ·
   [HTTP client](../src/rt/fetch_transport.cpp) · [Live sideband](../src/live/sideband.cpp)
4. **Write the response.** Application code normalizes provider responses, including errors and stream events.
   The transport writes a buffered body or pulls stream chunks at the pace the socket can accept. An interrupted
   connection signals cancellation. The response's socket `finish` resolves the delivery receipt, which reports
   interruption on close or write failure before finish. Reaching the end of the provider stream only ends the
   producer. [Stream relay](../src/provider/stream_relay.cpp) · [Serve handler](../src/handler/serve_handler.cpp) ·
   [Socket writes and receipt](../src/runtime/http.cpp)
5. **Settle and drain.** The terminal wrapper follows provider completion and records client delivery separately.
   A completed provider operation can still have a settled quota outcome when the client disconnects. The request
   keeps one admission lifecycle through settlement and tracks durable writes as part of its drain. On SIGINT or
   SIGTERM the server stops accepting connections and drains active requests before the serve lifecycle closes
   optional telemetry and KV. [Terminal logging](../src/handler/terminal_log.cpp) ·
   [Paid ledger settlement](../src/paid_fallback/ledger_settlement.cpp) · [Request drain](../src/runtime/http_tasks.cpp) ·
   [Shutdown](../src/serve.cpp) · [Delivery tests](../tests/serve_delivery_http_test.cpp)

## Async and ownership

`rt::Task<T>` is a Boost.Asio awaitable. It runs when awaited or started on an executor. `rt::Promise<T>` is a shared
result. Constructing it from a task starts that task, and callers await `.wait()` to observe its result or error.
The event loop runs I/O and timers on one application thread and drains queued promise and microtask callbacks
between events. SQLite does its work on a worker thread and reports results back to the caller's loop.
[Tasks and promises](../include/uos/rt/async.hpp) · [Event loop](../src/rt/async.cpp) ·
[SQLite worker](../src/rt/kv_sqlite.cpp)

These rules apply to every coroutine, including tests. They cover the retained GCC 12.2 profile and ordinary C++
object lifetimes. [Porting rules](../PORTING.md) · [Async reference](rt/async.md)

- Build arguments in named locals before awaiting. This includes aggregate values, braced lists, lambdas and implicit
  conversions that would create temporaries. Construct the task first when passing an aggregate by value, then await
  the named task. Use a named range when the loop body suspends. `KvKey{...}` is the documented braced-list exception.
- Save an awaited result in a named variable before passing it to another function or accessing a field. If cleanup
  needs to await after catching an exception, save the error and await outside the catch block.
- Keep the object behind a member coroutine alive until it completes. Use owning parameters in the coroutine frame
  when it can outlive its caller. Pass shared mutable state by shared ownership to each child that uses it.
- A coroutine lambda's captures live in the lambda object. Prefer a free function or captureless coroutine with
  owning parameters for work that can outlive the closure. Verify that callbacks and anything they borrow survive
  through actual asynchronous use, including when `rt::async` owns the callback itself.
- `all`, `allSettled`, `race` and `any` preserve sibling lifetimes. A result or rejection can let the caller finish
  while another task is pending. Give that task its own owners. Cancel only where an explicit abort is part of the
  operation's contract. [Abort signals](../include/uos/rt/abort.hpp)

## Confinement

The Linux ARM64 launcher verifies the sealed package and applies confinement before opening state or listening.
The default Landlock policy requires ABI 8 or newer. With `UOS_GATEWAY_ALLOW_REDUCED_CONFINEMENT=1`, an ABI 3–7
kernel runs with its available Landlock rules and later threads inherit the startup thread's domain. ABI 3 has
filesystem restrictions and lacks Landlock TCP port controls. Seccomp thread synchronization is required in both
modes. The packaged macOS gateway answers `--version` only because serving requires Linux confinement.
[Startup](../runtime/startup.cpp) · [Confinement policy](RUNTIME.md#linux-confinement) · [Packages](RELEASES.md)

## Storage

The service opens `.data/kv-bun.sqlite3` beneath the private state root. The opener checks the `bun-jsc-v1` marker
in `uos_kv_metadata` before handing the file to the linked `rt::SqliteKv` backend. The packaged `@deno/kv` artifact
is verified for provenance. The linked backend makes the actual SQLite calls. It uses a connection and worker per
handle, SQLite transactions and write-ahead logging on writable stores. [Storage opener](../src/storage/kv.cpp) ·
[Codec constants](../include/uos/storage/kv.hpp) · [Native artifact verification](../src/storage/native_addon.cpp) ·
[SQLite backend](../src/rt/kv_sqlite.cpp)

The database uses Deno KV's SQLite schema and ordered tuple keys. It preserves versionstamps, atomic checks and
writes, absolute expiration times and queue records. Row encodings distinguish raw bytes, unsigned 64-bit values and
structured values. Structured values use Bun's JavaScriptCore serialization in this database. The legacy Deno
database uses V8 serialization for that part of the format. Typed regular expressions preserve their source and
flags in both codecs, including `v`. Bundled assets and tool inputs use native filesystem paths from the configured
resource root. [Tuple keys](../src/rt/kv_key.cpp) ·
[JavaScriptCore reader](../src/rt/kv_jsc_deserialize.cpp) · [Durable KV format and interop](rt/kv-durable.md)

Conversion is a separate offline operation with a stopped, consistent source, a new destination and a verified
receipt covering raw keys, revisions, absolute expiry and typed values. Rollback after new writes requires reverse
conversion of the latest stopped state. The JSC reader and the typed interchange preserve sparse array entries,
references and cycles. Stored-graph traversal is capped at 2,000 containers. Views that project into JSON can
materialize holes, and reject cycles and values beyond the JSON depth limit of 128 containers.
[Converter](../src/kv_conversion/convert.cpp) · [Offline launch requirements](RUNTIME.md) ·
[Storage cutover blockers](OPEN-STAGES.md)

## Tests

Each `*_test.cpp` has its own executable and reports how many cases ran and passed. The tests under `tests/rt/`
exercise runtime APIs. Application tests cover modules, differential cases recorded from Bun and real loopback
HTTP fixtures with scripted providers. The same-binary package smoke is a separate qualification of startup,
authentication, delivery, restart and shutdown. [Tests](../tests/) · [Runtime tests](../tests/rt/) ·
[Loopback gateway fixture](../tests/oss_gateway_http_test.cpp) · [Package smoke](../tests/production_smoke/README.md)

`tools/links.py` resolves each binary's application source list from the registry or the test's `Links` comment.
The portable driver compiles the common objects once, links every `src/rt/*.cpp` and the selected vendor archives
into each fixture's closure, and runs the fixtures in real Linux network, mount and PID namespaces. The fixture
environment supplies an empty home, fresh temp space and loopback-only networking. The driver writes a per-binary
log and a receipt with the exact inputs and counts. [Link resolver](../tools/links.py) ·
[Registry](../tests/LINKS.txt) · [Portable driver](CI.md) · [Test conventions](TESTING.md)

All four Linux ARM64 Release shards passed 450 binaries and 5,113 cases. ASan, UBSan and LSan passed 161 selected
binaries and 1,989 cases, including every runtime binary and the HTTP, WebSocket, passkey, storage and confinement
fixtures. [Final integration](findings/cppgw-integrate-f2-20261118.md)

The paired comparison with the TypeScript gateway lives at [commit `ca045a83ebaab6c50c20567d1a825638abac0891`](https://github.com/ubiquity/ai.ubq.fi/tree/ca045a83ebaab6c50c20567d1a825638abac0891/cpp/gateway/conformance).
