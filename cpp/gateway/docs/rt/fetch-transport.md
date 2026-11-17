# uos::rt fetch transport: the HTTP/1.1 network client behind `fetch`

`#include "uos/rt/fetch_transport.hpp"` (`src/rt/fetch_transport{,_http,_net,_codec}.cpp`,
internals in `src/rt/fetch_transport_internal.hpp`; TLS trust and certificate checks shared
with rt::ws in `src/rt/tls{.cpp,_internal.hpp}`). Oracle: Bun 1.4.0's fetch on Linux arm64.
`rt::defaultFetch()` (docs/rt/web.md) validates the input, builds the Request and handles
aborts; this component is the `rt::FetchTransport` it sends through. Every expected value in
`tests/rt/fetch_transport_test.cpp` was recorded by running Bun's fetch against scripted raw
TCP/TLS peers (`Bun.listen`, loopback, OS-assigned ports); the C++ test replays the same scripts
on Asio peers in-process.

## Installation (what serve's main does)

```cpp
int main(int argc, char** argv) {
  rt::process::setArgv(argc, argv);
  rt::loop().setUncaughtPolicy(rt::UncaughtPolicy::report);
  rt::installHttpClientTransport();   // before anything can call fetch
  rt::blockOn(serveMain());
  return rt::process::exitStatus();
}
```

`installHttpClientTransport()` creates an `rt::HttpClientTransport` with Bun's defaults, makes
it the transport `rt::defaultFetch()` (and so `rt::fetch`, `rt::globalFetch()`) sends through,
and returns it. Any other C++ entry point whose code fetches (a CLI port of a script) calls it
the same way. Nothing installs it implicitly: unit tests keep the previous seams unchanged
(`rt::MockTransport` and `setFetchTransport`, `GlobalFetchOverride`, `ScopedLoopbackFetch`), and
a test that forgets to mock fails with `std::logic_error("rt::fetch: no FetchTransport
installed")` instead of reaching the network.

## Inventory: how the gateway fetches (the mandatory surface)

| What                                                                                                                                                                                         | Where (TS)                                                                                                                                                                                                                 |
| -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| upstream inference POSTs: `body: string` (`JSON.stringify` / `encodedBody`), `redirect: "manual"`, `signal` = `AbortSignal.any([caller, deadline])`, streaming SSE bodies read incrementally | codex/dispatch.ts, provider/{claude-code,metered,surplus,cerebras,lithos,openrouter}.ts, deepseek/index.ts, embeddings/voyage.ts, worker/transport.ts (`fetch(request)` with a Request input)                              |
| metadata GETs: `redirect: "manual"` (most), default `"follow"` (models lists, chart proxy, jev client), `"error"` (passport)                                                                 | models/_, provider/_-models.ts, admin/*, auth/{index,passport,passport-sessions}.ts, codex/{auth-refresh,manual-reset,banked-reset-provider}.ts, health.ts, metered-quota.ts                                               |
| headers the code sets                                                                                                                                                                        | `Authorization`, `ChatGPT-Account-ID`, `originator`, `user-agent` (codex_cli_rs/...), `Accept`, `If-None-Match`, `Content-Type`, provider keys; never Host, Connection, Content-Length or Accept-Encoding                  |
| timeouts                                                                                                                                                                                     | `AbortSignal.timeout(ms)` / `AbortSignal.any`, own `setTimeout` deadlines that abort a controller; `error.name === "TimeoutError"` decides 504s                                                                            |
| decompression                                                                                                                                                                                | relies on Bun decoding `Content-Encoding` while keeping the `Content-Encoding`/`Content-Length` headers: responses-handler.ts, openrouter-streams.ts, anthropic-*.ts and chart-source-proxy.ts delete them before relaying |
| errors read                                                                                                                                                                                  | `instanceof TypeError` / `DOMException` and `.name` (upstream-wire.ts diagnostics); no code reads fetch's `.code`                                                                                                          |
| not used                                                                                                                                                                                     | ReadableStream request bodies, `keepalive`, `duplex` (server side only), Bun's `proxy` / `unix` / `tls` / `decompress` / `verbose` options, `data:` / `file:` / `s3:` URLs, `fetch.preconnect` (smoke scripts copy it)     |

## Behaviour matched to Bun (pinned by the tests)

Request head. `<METHOD> <path+query> HTTP/1.1` (no fragment, no userinfo), then the request's
headers under their wire names sorted in byte order of those names (capitals before lowercase):
WebKit's common header names in Bun's spelling whatever the caller's case (`Content-Type`, `ETag`,
`DNT`, `TE`; Bun writes `Icy-Metaint`, `Last-Event-Id`, `Sourcemap` and `X-Sourcemap` where
WebKit's table says `Icy-MetaInt`, `Last-Event-ID`, `SourceMap`, `X-SourceMap`), every other name
as the caller first spelled it (rt::Headers keeps it: `rt::detail::headerWireFields`; a later
append or set in another spelling keeps the first, a delete forgets it, `new Headers(h)` and
Request copies keep it), Cookie values appended to one another joined with "; " (other names
", "); every Set-Cookie value last as its own `set-cookie` line, then those of `Connection: keep-alive`, `User-Agent: Bun/1.4.0`,
`Accept: */*`, `Host: <url.host>` (default port omitted) and `Accept-Encoding: gzip, deflate,
br, zstd` the request does not set itself, then `Content-Length: N` for buffered bodies
(string, bytes, Blob, URLSearchParams, FormData; `0` for a bodiless method other than GET,
HEAD and TRACE) or `Transfer-Encoding: chunked` for a ReadableStream body (one HTTP chunk per
stream chunk, empty chunks skipped). A request's own Content-Length and Transfer-Encoding are
dropped; `Expect: 100-continue` is sent but the body goes out at once. Values are Latin-1. The
request is written while the response is read, so an early response (413 during an upload) is
delivered as Bun delivers it.

Response. Status line `HTTP/1.<digit>`, spaces, exactly three digits, an optional reason
(statusText is the reason decoded as UTF-8 with U+FFFD; header values are Latin-1). Fields are
`token: value` lines, CRLF or LF; obs-fold, a space before the colon, an empty name, a control
byte other than tab, more than 256 fields or an unparsable status line fail with
`Malformed_HTTP_Response`; a head over about 1.5 MiB with `ResponseHeadersTooLarge`. 1xx heads
are skipped; 101 fails with `UnrequestedUpgrade`. Body framing: none for HEAD and TRACE requests
and 204/304 (whatever Content-Length says); `Transfer-Encoding` whose last token is the only
`chunked` (else `UnsupportedTransferEncoding`; other codings are ignored) wins over
Content-Length; Content-Length must be digits only (`+2`, `-1`, `2, 2`, overflow, two
different values: `InvalidContentLength`); otherwise the body runs until the connection
closes. Chunk sizes: up to 16 hex digits, anything up to the LF ignored, trailers skipped; a bad
size or a missing CRLF after the data errors the body with `InvalidHTTPResponse`. Every fetched
Response has a body stream (empty for HEAD/204/304), and `url`/`redirected` are set
(`setFetchResult`).

Streaming. The body stream (highWaterMark 1) reads the connection as the consumer pulls: each
socket read becomes one chunk as soon as it arrives (coalesced the way Bun coalesces), at most
one chunk is read ahead (backpressure), and a fully received small body releases its connection
even if nobody reads it.

Keep-alive. Sequential requests reuse a connection; concurrent ones open their own. Idle
connections are kept per loop in Bun's slot order (a released connection takes the first free
slot, a request takes the first matching slot), up to 64 for http: and 64 for https:, for 300 s;
expiry uses one unreferenced rt timer per pool, so idle connections never keep a loop running.
A connection is not reused after `Connection: close` (response or request), an HTTP/1.0 response
without `Connection: keep-alive`, a close-delimited body, bytes received beyond the response, a
cancelled or failed body, or a request still being written. A pooled connection that saw a FIN,
an RST or stray bytes while idle is replaced at once (checked without reading when it is
taken; TLS session tickets are consumed). Stale connections: when a reused connection fails
before a complete response head (EOF, RST, partial head), GET, HEAD, PUT, DELETE, OPTIONS and
TRACE are retried on the next pooled or a new connection, as often as reused connections fail;
POST, PATCH, other methods and stream bodies fail with `ECONNRESET`.

Redirects (`request.redirect()`). Only 301, 302, 303, 307 and 308 redirect. `follow`: the
Location (a Latin-1 header value) resolved against the current URL, its own fragment kept (the
request's is not inherited); 303 (unless HEAD) and 301/302 for POST become GET without body or
Content-Type/Encoding/Language/Location; 307/308 resend method and body (a ReadableStream body:
TypeError "Request body is a ReadableStream and cannot be replayed for this redirect");
a different origin drops Authorization, Cookie and Proxy-Authorization; the 3xx body is read off
so its connection can be reused; no Location returns the 3xx; an unparsable Location fails with
`RedirectURLInvalid`, a non-http(s) one with `UnsupportedRedirectProtocol`; after 127 requests
`TooManyRedirects`. `manual`: the 3xx as is (url = request URL, redirected false). `error`: any of
the five statuses fails with `UnexpectedRedirect`, with or without Location.

Decompression. Content-Encoding split at commas, trimmed, compared case-insensitively,
`identity` ignored: exactly one of gzip/x-gzip, deflate, br, zstd left decodes, anything else
(two codings, `gzip;q=1`, `compress`) passes the bytes through. Headers are left as received.
gzip: further members only when the next byte is 0x1f, other trailing bytes ignored; deflate:
zlib-wrapped when the first chunk has more than one byte and starts with 0x78, raw otherwise;
br and deflate ignore bytes after the end, zstd decodes further frames and fails on garbage; an
empty body is never an error. Corrupt or truncated data errors the body with `ZlibError`,
`BrotliDecompressionError` or `ZstdDecompressionError`, and prints Bun's `Decompression error:
<code>` line to stderr. gzip/deflate use rt::gzip's Inflater (zlib); br and zstd use the SDK's
`libbrotlidec.so.1` and `libzstd.so.1` through dlopen (no headers in the SDK; their stable C
entry points are declared in fetch_transport_codec.cpp). A coding whose library is missing is
left out of Accept-Encoding and fails the body if a server sends it anyway.

TLS. SNI is the host name (none for IP literals), ALPN offers only `http/1.1`. Trust: the system
store plus `NODE_EXTRA_CA_CERTS` (shared with rt::ws), or `HttpClientOptions::ca` instead;
`NODE_TLS_REJECT_UNAUTHORIZED=0` or `rejectUnauthorized = false` accepts anything. The chain is
judged after the handshake, then the host name (Node's checkServerIdentity rules).

Errors (all TypeError, `code` = JsError::code, `path` = the requested URL, `errno_` 0):

| Situation                                                                 | message                                                                                                                       | code                                               |
| ------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------- |
| connect failed (refused, unreachable)                                     | Unable to connect. Is the computer able to access the url?                                                                    | ConnectionRefused                                  |
| DNS                                                                       | getaddrinfo ENOTFOUND <host> (errno 4, syscall, hostname)                                                                     | ENOTFOUND                                          |
| EOF/RST before or during the body, RST during the TLS handshake           | The socket connection was closed unexpectedly. For more information, pass \`verbose: true\` in the second argument to fetch() | ECONNRESET                                         |
| protocol and redirect failures above                                      | `<code> fetching "<url>". For more information, pass \`verbose: true\` in the second argument to fetch()`                     | the code                                           |
| certificate chain                                                         | e.g. `self signed certificate`, `certificate has expired`                                                                     | DEPTH_ZERO_SELF_SIGNED_CERT, CERT_HAS_EXPIRED, ... |
| host name not in the certificate                                          | ERR_TLS_CERT_ALTNAME_INVALID fetching "<url>". ...                                                                            | ERR_TLS_CERT_ALTNAME_INVALID                       |
| TLS handshake failed or the server closed during it (a plain HTTP server) | unknown certificate verification error                                                                                        | UNKNOWN_CERTIFICATE_VERIFICATION_ERROR             |
| no activity for 300 s (head or body)                                      | DOMException TimeoutError "The operation timed out."                                                                          | (DOMException code 23)                             |

Aborts are the front end's: the signal's reason rejects the fetch or errors the body; the
transport closes the connection at every stage (Asio cancellation of DNS, connect, TLS, write
and read; a request body stream is cancelled with the reason).

## Operational settings (`HttpClientOptions`)

| Option               | Default (Bun)               | Meaning                                                         |
| -------------------- | --------------------------- | --------------------------------------------------------------- |
| `userAgent`          | `"Bun/" + rt::bun::version` | User-Agent when the request has none                            |
| `maxIdleConnections` | 64                          | idle connections kept for http:, and again for https:           |
| `keepAliveTimeoutMs` | 300000                      | idle pooled connection lifetime                                 |
| `timeoutMs`          | 300000                      | idle time after which a request or body fails with TimeoutError |
| `ca`                 | unset                       | PEM bundle trusted instead of the system store (Bun's `tls.ca`) |
| `rejectUnauthorized` | true                        | false accepts any certificate                                   |

Environment: `NODE_EXTRA_CA_CERTS`, `NODE_TLS_REJECT_UNAUTHORIZED`, `SSL_CERT_FILE` (OpenSSL).
Run-time libraries: OpenSSL 3, `libz.so.1`, `libbrotlidec.so.1`, `libzstd.so.1` (all in the SDK
sysroot the binaries run with). `HttpClientTransport::idleConnections()` and
`closeIdleConnections()` report and drop the current loop's pool.

## Differences from Bun

- A ReadableStream made from a buffered body (`new Response("x").body`) is sent chunked; Bun
  sends Content-Length for such blob-backed streams. The transport tells buffered bodies from
  streams with `detail::transferBody` (it marks only a buffered body used).
- Statuses outside 200..599 other than 101 (Bun accepts 600, 099) reject with rt::Response's
  RangeError.
- `data:`, `file:` and `s3:` URLs (which Bun fetches natively) reject with a TypeError
  ("rt::HttpClientTransport speaks http: and https: only ...").
- `HTTP_PROXY` / `HTTPS_PROXY` / `NO_PROXY`, which Bun's fetch honours, are not implemented (the
  service unit sets none; only scripts/admin-models.ts touches NO_PROXY). HTTP/2 is out of
  scope (Bun's fetch speaks HTTP/1.1 and offers only http/1.1 in ALPN).
- TLS is OpenSSL, not BoringSSL: the ClientHello (ciphers, extensions) differs, sessions are not
  resumed, and failures other than certificate checks all map to UNKNOWN_CERTIFICATE_...
- No DNS cache (Bun caches lookups); temporary DNS failures also report ENOTFOUND.
- The transport's timers (pool expiry, idle timeout) take rt timer ids, so later
  `setTimeout` ids differ from Bun's.
- A close-delimited 3xx body is not read before following the redirect (its connection is
  closed instead).

## Tests

`tests/rt/fetch_transport_test.cpp` (33 cases): request heads byte for byte (header name spelling,
order and Cookie joining for records, entry lists, set/append/delete sequences, copies and
Request inputs, recorded from Bun on a raw loopback listener); the response parser
table (68 heads); keep-alive, pool slots, the 64 limit, 300 s expiry (fake clock), loop
shutdown; stale retries per method with FIN, RST and partial heads; early responses during an
upload; chunk delivery latency and boundaries; large bodies both ways; aborts at every stage;
the idle timeout; redirects; decompression (payloads recorded from Bun's node:zlib); TLS with
embedded self-signed certificates (SNI, ALPN, trust, host names, expiry, handshake failures);
connection, DNS and reset errors; `installHttpClientTransport`.
