# uos::rt WebSocket: the `ws` package (8.18.3)

Oracle: ws 8.18.3 (the version `package.json` pins) running under Bun 1.4.0, which is
what `src/runtime/websocket.ts` loads. Headers: `uos/rt/websocket.hpp` (namespace
`uos::rt::ws`, mirroring the npm package) and `uos/runtime/websocket.hpp` (the port of
`src/runtime/websocket.ts`). The implementation in `src/rt/websocket{,_frame,_socket,_client}.cpp`
uses Boost.Beast for masking, handshake primitives, and received UTF-8 validation.
The gateway's adapter owns the transport and event lifecycle. The original expectations
in `tests/rt/websocket_test.cpp` came from ws under Bun (section 7). [Runtime deviations](../DEVIATIONS-FROM-BUN.md#websocket)

## 1. What the TypeScript uses (the mandatory surface)

| Consumer                                                         | Uses                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| ---------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/runtime/websocket.ts`                                       | `loadWebSocketModule()` -> `{ default: WebSocket, WebSocketServer }`, cached promise                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| `src/runtime/http-upgrade.ts`                                    | `new WebSocketServer({ noServer: true, clientTracking: false, perMessageDeflate: false, maxPayload, WebSocket: PayloadLimitedSocket })`; a subclass overriding `close(code, reason)` (ws's bare 1009 becomes the route's code/reason); `server.once("headers", (headers) => headers.push(...))`; `server.handleUpgrade(incoming, socket, head, cb)`; `server.close()`; server sockets: `on("error")`, `once("close")`, `terminate()`, `close(1001, "Server shutting down")`                                                                                            |
| `src/runtime/http.ts`                                            | re-exports `loadWebSocketModule`; node:http `"upgrade"` event -> `upgrades.upgrade(incoming, socket, head)`                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| `src/codex/supervisor-transport.ts`                              | client `ws+unix://<socket>:/` with `{ headers: { Host: "localhost" }, perMessageDeflate: false, maxPayload: 1048576, handshakeTimeout: 3000 }`; `on("message"/"close"/"error")`, `once("open"/"error")`, `readyState` (1, 3), `send(string)`, `close(1000, "supervisor sample complete")`, `terminate()`; test seam swaps the constructor for a fake class                                                                                                                                                                                                             |
| `src/codex/native-auth-transport.ts`                             | client `ws+unix://<home>/app-server-control/app-server-control.sock:/` (`maxPayload: 65536`, `handshakeTimeout: 3000`); `on("message"/"error"/"close")`, `once("open"/"error")`, `send`, `terminate`                                                                                                                                                                                                                                                                                                                                                                   |
| `src/live/sideband.ts`                                           | upstream client `wss://api.openai.com/v1/live/<id>` (tests: `ws://127.0.0.1`) with `headers`, `maxPayload` 4 MiB, `handshakeTimeout` 10 s; `on("open")`, `on("message", (data, isBinary))`, `on("close", (code, reason))`, `on("error")`, `on("unexpected-response", (_req, res) => res.statusCode)`, `send(text)`, `bufferedAmount`, `readyState`, `close()`, `close(code, reason)`, `terminate()`. Downstream (server) socket: `onmessage` (`event.data` string or Buffer), `onclose` (`code`, `reason`), `onerror`, `readyState`, `bufferedAmount`, `send`, `close` |
| tests (`runtime-http`, `live-sideband-http`, native-auth helper) | ws client with default options, `headers`, `unexpected-response` + `response.resume()` + `terminate()`, `send(Uint8Array)`, `once("message")`, `removeListener`, `close(1000, reason)`; `Object.defineProperty(WebSocket.prototype, "bufferedAmount", ...)` keyed by `this.url`                                                                                                                                                                                                                                                                                        |

Nothing uses: ping/pong methods or events (auto-pong matters: peers may ping),
subprotocols, permessage-deflate, redirects, `verifyClient`, a server with its own
HTTP listener, `binaryType`, `pause`/`resume`, `addEventListener`, `createWebSocketStream`, Blob data.

## 2. TypeScript -> C++

`ws` is `uos::rt::ws`. Sockets and servers are handles in `std::shared_ptr`.

| TypeScript                                                                                                                | C++                                                                                                                                                                                                                                                                                                                                                                                   |
| ------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `const { default: WebSocket, WebSocketServer } = await loadWebSocketModule()`                                             | `auto module = co_await uos::runtime::websocket::loadWebSocketModule().wait();` -> `module.default_`, `module.WebSocketServer`                                                                                                                                                                                                                                                        |
| `typeof module.default === "function"`                                                                                    | `static_cast<bool>(module.default_)` (`ws::WebSocketConstructor`, a `std::function`)                                                                                                                                                                                                                                                                                                  |
| `new WebSocket(url, { headers: { Host: "localhost" }, perMessageDeflate: false, maxPayload: n, handshakeTimeout: ms })`   | `ws::ClientOptions options; options.headers = {{"Host", "localhost"}}; options.perMessageDeflate = false; options.maxPayload = n; options.handshakeTimeout = ms;` then `auto socket = module.default_(url, options);` or `ws::WebSocket::create(url, options)`                                                                                                                        |
| `new WebSocket(url)`                                                                                                      | `ws::WebSocket::create(url)`                                                                                                                                                                                                                                                                                                                                                          |
| `socket.on("open", () => ...)`, `once`, `removeListener`, `off`                                                           | `socket->on("open", [] { ... })`; a listener with identity is a named `ws::Listener` (`ws::Listener l([&] {...}); socket->once("message", l); socket->removeListener("message", l);`)                                                                                                                                                                                                 |
| `socket.on("message", (data, isBinary) => ...)`                                                                           | `socket->on("message", [](const ws::EventArgs& e) { const ws::Bytes& data = std::get<ws::Bytes>(e.data); bool isBinary = e.isBinary; })`                                                                                                                                                                                                                                              |
| `socket.on("close", (code, reason) => ...)`                                                                               | `[](const ws::EventArgs& e) { e.code; e.reason; }` (`reason` is the Buffer: `ws::Bytes`)                                                                                                                                                                                                                                                                                              |
| `socket.on("error", (err) => ...)`                                                                                        | `[](const ws::EventArgs& e) { rt::errorMessage(e.error); }` (`e.error` is an `rt::Reason`)                                                                                                                                                                                                                                                                                            |
| `socket.on("unexpected-response", (_req, res) => { res.statusCode; res.resume(); })`                                      | `[](const ws::EventArgs& e) { e.response->statusCode; e.response->resume(); }`                                                                                                                                                                                                                                                                                                        |
| `raw instanceof Uint8Array` / `typeof raw === "string"` on message data                                                   | `std::holds_alternative<ws::Bytes>(e.data)` / `std::holds_alternative<std::string>(e.data)` (real frames are always Bytes; a string comes from a test double)                                                                                                                                                                                                                         |
| `new TextDecoder().decode(raw)`, `String(raw)` of a Buffer                                                                | `rt::decodeUtf8(std::get<ws::Bytes>(e.data))`, `rt::decodeUtf8(bytes, {.ignoreBOM = true})`                                                                                                                                                                                                                                                                                           |
| `socket.onmessage = (event) => ...` / `onclose` / `onerror` / `onopen`                                                    | `socket->onmessage([](const ws::MessageEvent& event) {...})` (`event.data`: `std::string` for text, `Bytes` for binary); `onclose(const ws::CloseEvent&)` (`code`, `reason` string, `wasClean`); `onerror(const ws::ErrorEvent&)`; `nullptr` clears; `socket->onmessage()` reads                                                                                                      |
| `socket.send(text)`, `send(buffer)`, `send(data, cb)`, `send(data, { fin: false })`                                       | `socket->send(std::string(text))`, `socket->send(bytes)`, `socket->send(data, [](rt::Reason error) {...})`, `socket->send(data, ws::SendOptions{.fin = false})`                                                                                                                                                                                                                       |
| `socket.close()`, `close(code, reason)`                                                                                   | `socket->close()`, `socket->close(code, std::string(reason))` (or `close(std::optional<int>, std::optional<ws::Data>)`)                                                                                                                                                                                                                                                               |
| `socket.terminate()`, `ping(data)`, `pong(data)`                                                                          | same names                                                                                                                                                                                                                                                                                                                                                                            |
| `socket.readyState`, `bufferedAmount`, `url`, `protocol`                                                                  | `socket->readyState()`, `bufferedAmount()` (double), `url()` (`std::optional<std::string>`), `protocol()`                                                                                                                                                                                                                                                                             |
| `WS_READY_STATE_OPEN = 1` etc.                                                                                            | `ws::WebSocket::OPEN` (`CONNECTING` 0, `OPEN` 1, `CLOSING` 2, `CLOSED` 3)                                                                                                                                                                                                                                                                                                             |
| `new WebSocketServer({ noServer: true, clientTracking: false, perMessageDeflate: false, maxPayload: n, WebSocket: Cls })` | `ws::ServerOptions o; o.noServer = true; o.clientTracking = false; o.perMessageDeflate = false; o.maxPayload = n; o.WebSocket = [..](const ws::ServerOptions& so) { return std::make_shared<Cls>(...); };` then `module.WebSocketServer(o)` or `std::make_shared<ws::WebSocketServer>(o)`                                                                                             |
| `class Cls extends WebSocket { override close(code, reason) {...} }`                                                      | `class Cls : public ws::WebSocket { public: Cls(bool autoPong, ...) : ws::WebSocket(nullptr, autoPong) {} using ws::WebSocket::close; void close(std::optional<int> code, std::optional<ws::Data> reason) override {...} };` (`reason === undefined` is `!reason`)                                                                                                                    |
| `server.once("headers", (headers, req) => headers.push(line))`                                                            | `server->once("headers", [](const ws::EventArgs& e) { e.headers->push_back(line); e.request->url; })`                                                                                                                                                                                                                                                                                 |
| `server.handleUpgrade(incoming, socket, head, (ws, req) => ...)`                                                          | `server->handleUpgrade(request, upgradeSocket, head, [](std::shared_ptr<ws::WebSocket> socket, const ws::UpgradeRequest& req) {...})` (section 4)                                                                                                                                                                                                                                     |
| `server.close()`, `server.close(cb)`                                                                                      | `server->close()`, `server->close([](rt::Reason error) {...})`                                                                                                                                                                                                                                                                                                                        |
| `Object.defineProperty(Client.prototype, "bufferedAmount", { get() { ... original.get.call(this) } })`                    | `auto original = ws::WebSocket::bufferedAmountGetter(); ws::WebSocket::bufferedAmountGetter() = [original](const ws::WebSocket& s) { return ... original(s); };` ... restore `original`                                                                                                                                                                                               |
| a fake socket class for a test seam (`setSupervisorWebSocketConstructorForTest(Fake)`)                                    | derive from `ws::WebSocket` with `WebSocket(nullptr)`, override the virtual methods it fakes (`on`, `once`, `emit`, `send`, `close`, `terminate`, `readyState`, ...), and pass a `ws::WebSocketConstructor` lambda that `std::make_shared`s it; work the JS constructor queues on `this` (`queueMicrotask(() => this.emit("open"))`) goes in that lambda, after the shared_ptr exists |

Errors: ws's own errors are `rt::JsError` ("WebSocket was closed before the connection
was established", "Opening handshake has timed out", "Unexpected server response: 401",
"Invalid Sec-WebSocket-Accept header", "WebSocket is not open: readyState 0 (CONNECTING)",
...) or `ws::CodedError` (a `JsError` with `code`, `errno_`, `syscall`): frame errors
(`name` "RangeError"/"Error", `code` "WS_ERR_*"), system errors ("connect ECONNREFUSED
127.0.0.1:1", ECONNREFUSED, -111, "connect"), "socket hang up" (ECONNRESET), llhttp
parse errors ("Parse Error: Invalid status code", HPE_INVALID_STATUS), TLS errors ("self
signed certificate", DEPTH_ZERO_SELF_SIGNED_CERT; "Hostname/IP does not match
certificate's altnames: ...", ERR_TLS_CERT_ALTNAME_INVALID) and constructor TypeErrors
for headers (ERR_INVALID_HTTP_TOKEN, ERR_INVALID_CHAR). `close()`'s argument errors are
`rt::TypeError` / `rt::RangeError`; URL errors `rt::SyntaxError`. A `CodedError` named
"RangeError" is not an `rt::RangeError` type (no TS site tests the class).

## 3. Lifetime and threading

- Everything runs on the current rt loop: create sockets and servers inside the loop
  that will run them (`rt::blockOn`, an `ASYNC_TEST`, the serve loop). Events are
  macrotasks (one per socket read or write completion); ws's `process.nextTick` work is a
  microtask. A hostname (`wss://api.openai.com`) is resolved by Asio's resolver thread and
  the result posted back; IP literals and Unix sockets never leave the loop thread.
- An open connection keeps its `WebSocket` alive (its IO holds it, as the event loop
  does in JS); while connecting, the handshake request holds it. Hold your own
  `std::shared_ptr` only as long as the TS holds the object.
- After "close" has been emitted no event can follow, so the socket drops all its
  listeners: whatever they captured (often the socket itself or its peer, e.g. the live
  sideband's two sockets) is released, which JS would garbage-collect. `listenerCount`
  is therefore 0 after "close" (JS keeps counting them).
- Listeners are copied for each emit (Node's snapshot); a listener may add, remove,
  close, terminate or drop references freely. Capture the socket in its own listeners by
  raw pointer or `shared_ptr`; both are safe while the event runs.
- An exception escaping a listener stops that emit (the remaining listeners are skipped,
  as Node's `emit`) and goes to the loop's `UncaughtPolicy` (tests fail, serve prints);
  unlike Bun, ws's processing then continues normally. "error" with no listener is
  uncaught the same way.
- `WebSocketServer` must be created with `std::make_shared` (its "close" event is
  deferred and holds it). A server socket class must be created with `std::make_shared`
  too (`ServerOptions::WebSocket` returns one).
- GCC 12 rule (async.md section 3): `co_await` a promise from `loadWebSocketModule()`
  bound to a named local or `.wait()` on the call; never build `ClientOptions` or a
  lambda inside a `co_await` expression.

## 4. What the HTTP server stage must provide

node:http hands ws `(req, socket, head)`; the C++ seam is explicit:

```cpp
ws::UpgradeRequest request;
request.method = "GET";                       // req.method
request.url = "/v1/live/abc?x=1";             // req.url, the request target as received
request.rawHeaders = {{"Host", "h"}, ...};    // req.rawHeaders: wire order and case, Latin-1 (byte b -> U+00b)
ws::UpgradeSocket socket{std::move(stream)};  // asio::generic::stream_protocol::socket on rt::executor()
socket.readable = !clientSentFin;             // socket.readable
socket.writable = !stageEndedItsSide;         // socket.writable
ws::Bytes head = bytesReadPastTheHead;        // `head`
server->handleUpgrade(request, socket, std::move(head), callback);
```

- `request.header(name)` reproduces `req.headers[name]` from `rawHeaders` with node:http's
  rules (pinned against Bun: duplicates joined with ", ", "cookie" with "; ", first value
  kept for host, authorization and the other single-value fields). `src/runtime/http.ts`'s
  own `incomingHeaders()` / `incomingRequest()` read the same rawHeaders.
- The stream is a connected TCP (`asio::ip::tcp::socket`) or Unix-domain
  (`asio::local::stream_protocol::socket`) socket, moved into the generic socket type, on
  the loop's executor, with no operation pending on it (cancel the stage's own read
  first; every byte it already read past the request head goes in `head`). Like Node,
  which does not read an upgraded socket while `http-upgrade.ts` awaits the handler,
  the stage should not read it before the hand-over. TLS termination is not provided on
  the server side.
- The stage keeps the socket (to answer with `refuseUpgrade`, watch "close"/"error",
  read `remoteAddress`) until it calls `handleUpgrade`. `handleUpgrade` is synchronous:
  ws takes the stream (moves it out of `socket.stream`) when it answers (101, or its
  405/400/503 response followed by end and destroy) or destroys a half-closed socket.
  If a "headers" listener throws, the exception propagates first and the stream stays
  with the caller, open, so `http-upgrade.ts`'s `catch` can still `refuseUpgrade(500)`.
- After the callback, ws owns the connection: its "close" is the socket's.
  `http-upgrade.ts`'s `socket.removeListener(...)` cleanup maps to dropping the stage's
  own pre-upgrade watchers.
- `head` reaches the new socket one tick after `handleUpgrade` returns, i.e. after the
  callback attached its listeners: frames a client sent together with its request are
  delivered, not lost (live-sideband's "frames sent immediately after the 101").

## 5. Behaviour (pinned by the tests)

- Handshake: request head in Node's order (user headers as given, Sec-WebSocket-Version,
  Sec-WebSocket-Key, Connection, Upgrade, Origin, then Host `host[:port]` unless given,
  then Authorization Basic from URL userinfo, unescaped); header values are written as
  Latin-1. `http:`/`https:` become `ws:`/`wss:` in `url()`. In `ws+unix:<path>:<request
path>` the socket path ends at the first ":" and the request path at the next one (no
  Host header unless given). A 101 counts as an
  upgrade only on HTTP/1.1 with an Upgrade header (Bun); anything else is
  "unexpected-response" (or "Unexpected server response: <status>" when nobody listens),
  after which the socket stays CONNECTING until terminated. `handshakeTimeout` is an
  idle timer (it restarts on socket activity, as socket.setTimeout does) that stops once
  a response head arrived.
- Frames: client masking keys come from Beast's secure generator. The transport
  uses 7/16/64-bit lengths and reassembles continuations across interleaved control
  frames. Received text and close reasons always require valid UTF-8 (1007).
  The receiver checks `maxPayload` against the running message total from each
  frame header before its payload arrives (1009). It preserves error codes and close
  statuses and ignores data after a close frame.
- Close: ws's handshake (echo, then FIN once both close frames passed; 30 s close timer,
  then destroy), codes 1005 (no status) and 1006 (no close frame), `close()` validation
  with readyState already CLOSING when it throws, `wasClean`, messages still delivered
  while CLOSING, server "close" 1006 after a receiver error.
- `bufferedAmount`: bytes not yet handed to the kernel plus what was "sent" after close
  started (`send`/`ping`/`pong` then only count); a write the kernel takes at once leaves
  it 0 and runs its callback after the current microtasks, as Bun does.
- Server: ws's validation responses (405 "Invalid HTTP method", 400 for Upgrade, key,
  version (+ `Sec-WebSocket-Version: 13, 8`), subprotocol syntax, `path` mismatch, 503
  after `close()`), "headers" lines written as UTF-8, subprotocol selection,
  `clientTracking`.
- TLS client: OpenSSL; the chain is judged after the handshake, then Node's
  `checkServerIdentity` (wildcards, IP SANs, CN fallback) with Node's messages;
  `rejectUnauthorized: false` accepts anything; SNI is the host unless it is an IP.

## 6. Limits (not provided, or not exact)

- permessage-deflate: never offered or accepted. `perMessageDeflate: true` (the client
  default) behaves as false, so a default-options client omits ws's
  `Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits` request header;
  `ServerOptions::perMessageDeflate = true` throws `std::logic_error`. Every gateway call
  site passes false.
- Not provided (no call site): redirects (`followRedirects`), client subprotocols,
  `verifyClient`, the "wsClientError" event, servers with `port`/`server`,
  `allowSynchronousEvents: false`, `binaryType` other than "nodebuffer", Blob data,
  `pause()`/`resume()`, `addEventListener`/`removeEventListener`, `createWebSocketStream`,
  `generateMask`, `finishRequest`, the "redirect" event, ws's `ClientRequest` argument of
  "unexpected-response" (only the response is passed), TLS options other than `ca`,
  `rejectUnauthorized` and `servername`, TLS on the server side.
- Trust store: Bun bundles Mozilla's roots; rt uses the system store (OpenSSL's default
  paths, else `/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt`
  or `/etc/ssl/cert.pem`) plus `NODE_EXTRA_CA_CERTS`. TLS failures other than
  certificate verification carry OpenSSL's text (`ERR_SSL_<REASON>`), not BoringSSL's;
  a peer closing during the TLS handshake gives Node's "Client network socket
  disconnected before secure TLS connection was established" (ECONNRESET).
- Timing: event order on one socket matches ws; the exact number of turns does not
  (ws's `process.nextTick` is a microtask here, Bun's socket internals are not
  modelled). Cross-socket ordering in one process may differ from Bun's.
- A listener that throws does not leave ws stuck as it would in Bun (section 3).
- Response parsing: llhttp's texts for a non-HTTP start ("Parse Error: Expected HTTP/,
  RTSP/ or ICE/") and a bad status ("Parse Error: Invalid status code") are pinned; other
  malformed heads get llhttp-style texts that were not compared (HPE_INVALID_VERSION,
  HPE_INVALID_HEADER_TOKEN, HPE_HEADER_OVERFLOW above 16 KiB). DNS failures say
  "getaddrinfo ENOTFOUND <host>" (EAI_AGAIN for temporary failures) like Node, not
  compared against Bun.
- A negative or non-finite `handshakeTimeout` means no timeout (Node would throw
  ERR_OUT_OF_RANGE from socket.setTimeout for a negative one).
- `close()` on a socket that never connected (a bare `WebSocket(nullptr)`) throws
  TypeError "undefined is not an object (evaluating 'stream.setHeader')", JSC's text for
  ws reading its missing request.
- The 30 s close timer is ws's constant and is not exercised by the tests (it needs 30 s
  of real time with a socket open).

## 7. Oracle

Probes ran ws's own ESM entry (`node_modules/ws/wrapper.mjs`, imported by absolute path
as `src/runtime/websocket.ts` does, which bypasses Bun's built-in `ws`) under `bun`:

- loops: an `http.createServer` whose "upgrade" event does what `http-upgrade.ts` does
  (noServer `WebSocketServer`, `handleUpgrade`, `close()`), a ws client, and a recorder
  logging `[side, event, ...args, readyState]` for open, message, close, error, ping,
  pong, upgrade and unexpected-response (Buffers as hex, errors as `{error, message,
code, errno, syscall}`);
- raw client: `net.connect`, an upgrade request with key `dGhlIHNhbXBsZSBub25jZQ==`,
  hand-built frames with mask `11 22 33 44`, recording the response head, the bytes
  after it and whether the peer ended;
- raw server: `net.createServer` answering the request with exact 101 / error / garbage
  bytes and frames, recording what the client sent;
- TLS: `https.createServer` with self-signed certificates (`openssl req -x509 -newkey
ec`), clients with and without `ca` / `rejectUnauthorized`.

The C++ tests replay the same scenarios with `rt::ws` on both ends (or against raw
Asio peers) and compare the logs as JSON text, side by side.
