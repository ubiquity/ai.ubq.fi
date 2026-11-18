# C++ gateway KEEP audit

Source baseline `b54357a92c0f2669335d83f04047c3f7de32f925` on `agent-jobs/cppgw-std-icu-20261117-44188151110-1`, tree `d0aabad25d3dbe02f2f33ff519e5b77715d19d37`.

## Work contract v1

Audit rows 17–55 under the owner's library-first policy. Measure the existing code, trace callers in `cpp/gateway/src`, and give a decision, net savings, risk, and ownership boundary for each row. The proposed work starts with the pinned C++20 build, Boost 1.92, and the existing native dependencies. [Replacement plan](../OSS-REPLACEMENT-PLAN.md) · [CI](../CI.md) · [Build](../BUILD.md) · [Runtime](../RUNTIME.md) · [Porter rules](../../PORTING.md)

| Surface | Acceptance |
| --- | --- |
| Public HTTP and upstream I/O | Preserve protocol, status, validation, cancellation, actual delivery receipts and exactly-once settlement. |
| Storage and security | Preserve on-disk values, ordering, expiry and revisions, authorization, trust, launch confinement and fail-closed behavior. |
| Execution | Keep named coroutine arguments, explicit owners across suspension, and uncancelled sibling work where the application relies on it. |
| Report | Audit all 39 rows and write at least eight runnable briefs. Check changed Markdown, diff hygiene, ownership and exact local test selections. |

The owned paths are this report and new Markdown under `cpp/gateway/docs/briefs/`. Source, tests, build settings, deployments, and the other agents' branches are outside the report's write scope. The initial unknowns were live API usage, current ledger boundaries and the cost of the remaining adapters. Completion requires a verdict for every row, a ranked estimate and verified brief commands.

## Ranked implementation briefs

Eleven lanes cover 13 KEEP rows with **1,185–1,820 estimated net first-party lines** removed. A further direct C++ design is assessed separately below. Prioritize by the midpoint of the net estimate divided by ordinal risk, with higher net savings breaking ties. Estimates subtract replacement code and caller churn and give zero credit to a move into a different first-party file. Each lane remeasures on its merged starting tree.

Risk **1** is an unreachable runtime feature with fixture coverage, **2** is a bounded internal refactor, **3** reaches validation, I/O, protocol output or shared runtime state, **4** changes multiple production lifetimes or stored-data interpretation, and **5** touches security enforcement, persistent encoding or settlement. The ordering is a triage aid. It measures code removal potential, with proof costs expressed by the risk score.

| Rank | Plan rows | Brief | Net LOC | Risk | LOC / risk | Sequencing |
| ---: | --- | --- | ---: | ---: | ---: | --- |
| 1 | 54 | [Structured console output](../briefs/console.md) | 280–420 | 2 | 175.0 | Rebase after the console helper lane. |
| 2 | 53 | [Explicit environment view](../briefs/environment.md) | 120–150 | 1 | 135.0 | Rebase after the env helper lane. |
| 3 | 29, 30 | [Inbound multipart and upload parts](../briefs/multipart.md) | 195–285 | 3 | 80.0 | Own FormData and Blob sections only. Coordinate with Headers. |
| 4 | 50, 51 | [File text I/O and lexical paths](../briefs/filesystem-paths.md) | 165–245 | 3 | 68.3 | One owner for both sections of `fs.*`. Rebase after filesystem helpers. |
| 5 | 18 | [Numeric helpers for live domains](../briefs/numbers.md) | 85–120 | 2 | 51.3 | Rebase after BigInt. Coordinate the Codex UUID call site with text work. |
| 6 | 46 | [Native membership and ordered snapshots](../briefs/ordered-containers.md) | 95–175 | 3 | 45.0 | Keep routing, SQL row and KV consumers out of the first patch. |
| 7 | 35 | [UTC calendar operations](../briefs/dates.md) | 90–145 | 3 | 39.2 | Rebase after date helpers and passkey ASN.1. |
| 8 | 28 | [Validated header storage](../briefs/headers.md) | 55–95 | 3 | 25.0 | One owner with the active Headers lane. Land before a writer refactor. |
| 9 | 32 | [Narrow string operations](../briefs/strings.md) | 35–65 | 2 | 25.0 | Preserve ICU, UTF and regex regions. Recompute after regex lands. |
| 10 | 27 | [Whole-buffer gzip adapters](../briefs/gzip.md) | 45–75 | 3 | 20.0 | Leave the streaming inflater interface to rows 24 and 27 jointly. |
| 11 | 34 | [Native paths in place of synthetic file URLs](../briefs/file-urls.md) | 20–45 | 3 | 10.8 | Rebase after URL and filesystem work. Preserve the SQLite URI boundary. |

## Measurement

The inventory names disjoint files and inclusive line spans. I counted the Git blobs at the baseline, including blanks, comments and generated tables as physical LOC. That gives **27,372 lines across 80 distinct paths**. All 39 row counts match the plan after rebasing the two string ranges. Between the plan commit `0edaf83367bbb29eaf50076afd428818f4b3e4bb` and the baseline, the only changes under the audited runtime source and include trees are `string.cpp` and `string.hpp`. ICU shifted the retained ranges. Rows 32 and 33 below give current coordinates.

Caller counts below are textual matches of the named symbols in `src/**/*.cpp` outside `src/rt/`. They include qualified references or matching comments where present and give a lower bound on the surface when aliases, unqualified calls and headers are involved. For runtime-internal paths, each verdict names the immediate caller. Follow includes and caller links before deleting an API. Every runtime translation unit participates in each test's link, so a link-closure match alone does not prove a runtime call.

## Verdicts by plan row

### JSON and numbers

#### 17. JSON stringify and quote — 302 LOC · retain · 0 net · risk 4

`jsonStringify` and `jsonQuote` have 234 matches across 120 application files. `worker/entrypoint.cpp` hashes serialized input, `sentinel/replay_envelope.cpp` encodes metadata, and `kv_conversion/convert.cpp` hashes or compares projections and writes proof data. `chat/completions.cpp` uses it for SSE and `rt/http.cpp` for JSON responses. The writer implements JS numeric-key enumeration, decimal formatting, nonfinite-to-null, lone-surrogate escaping, a depth cap, pretty indentation and a replacer overload. `boost::json::serialize` fits compact finite JSON on independent wire paths once validated. Its general key order, double formatting and string handling need qualification before changing digest input or recorded bytes. The existing writer still serves those callers, so a parallel serializer earns no immediate deletion. Keep row 1's parse fast path, rows 18–19's number and member-order helpers, and row 44's stored-value projection under their own owners. [Writer](../../src/rt/json.cpp#L132) · [Worker digest](../../src/worker/entrypoint.cpp#L296) · [Conversion hashes](../../src/kv_conversion/convert.cpp#L183)

#### 18. Number coercion, decimal output and Math — 673 LOC · narrow · 85–120 net · risk 2

`parseInt` and `stringToNumber` feed query controls, configuration and upstream usage. `Number::isSafeInteger` protects quota and authority inputs. `toFixed` and `parseFloat` have no callers in `src`. The nondecimal `numberToString` overload's one application call prints a bounded hexadecimal nibble in `codex/dispatch.cpp`. Replace that operation with typed integer formatting or a nibble lookup, and retire unused fractional radix and fixed-decimal machinery. `std::from_chars`, `std::to_chars` and `<cmath>` already provide primitives. Keep JS conversion at live untyped inputs and retain decimal spellings used in serialization, IDs and persisted hashes. Plain `std::round` has different negative-half behavior. Own only the numbered numeric regions plus the UUID formatter call site. The BigInt lane owns `number.cpp:13–19,216–495,791–1020` and the BigInt header section. [Number code](../../src/rt/number.cpp#L496) · [UUID variant](../../src/codex/dispatch.cpp#L376) · [Brief](../briefs/numbers.md)

#### 19. ECMAScript structural JSON and value helpers — 925 LOC · retain for now · 0 net · risk 4

The sampled `get*`, `objectSet/Define/Keys/Entries`, `truthy`, `makeNumber`, coercion and equality/clone group has 4,735 matches in 237 application files. Plain `boost::json::object::if_contains` and `value::if_*` cover typed lookups, a value copy covers `structuredClone`, and `std::ranges`/stable algorithms cover typed arrays. `rt::objectSet` skips `__proto__`; `objectDefine` defines it; `objectDelete` uses stable erase. Enumeration sorts canonical numeric keys first. Deep equality, JS truthiness and numeric ranges in route/authority callers require deliberate migration. A blanket substitution would change decisions or hashes. The direct C++ option below leaves explicit boundary behavior and removes redundant facades domain by domain. Shared JSON file work in rows 1 and 17, record bindings in 48, and stored values in 44 each have independent ownership. [Helpers](../../src/rt/json.cpp#L1067) · [Port type conventions](../../PORTING.md)

### HTTP and cryptography

#### 20. Server HTTP session, events and writer — 1,592 LOC · retain through parser integration · 0 net · risk 5

`serve.cpp` and the two compaction smoke modules use `serveHttp`. Internally, `ConnectionState` and `ServerResponse` own pipelining, chunked output, interim responses, the write queue, backpressure and socket lifetime. The server resolves successful delivery on the finish event after accepted writes. Asio already handles socket I/O. Beast can supply message serialization and framing, while the session, response timing, shutdown, and delivery state still need gateway ownership. A direct handler/writer transition could remove the internal Node emitter model later. The parser/HTTP cap lane owns row 6 inside `http.cpp`. Row 21 owns the receipt, incoming-body, early-header and upgrade adapters. [Session](../../src/runtime/http.cpp#L303) · [ServerResponse](../../src/runtime/http.cpp#L1385) · [Production server](../../src/serve.cpp#L151)

#### 21. Incoming bodies, delivery receipt and upgrade handoff — 1,168 LOC · retain · 0 net · risk 5

`responses_native_headers.cpp` can commit headers early, `handler/terminal_log.cpp` tracks terminal bookkeeping against delivery, `serve.cpp` awaits server completion, and `live/sideband.cpp` supplies the authenticated upgrade. The implementation distinguishes finish, close and errors, pauses reads, handles drain, hands off buffered upgrade data, and loops until request-owned writes settle. There is no library-level replacement for an admission or settlement receipt. A typed Asio request context could replace the event and weak side-table mechanics after the HTTP and WebSocket lanes integrate. Preserve exactly one delivery resolution and one settlement, pending writes before telemetry/KV close, and actual WebSocket headers, limits, and Unix socket behavior. [Receipt](../../src/runtime/http.cpp#L1819) · [Drain](../../src/runtime/http_tasks.cpp) · [Terminal](../../src/handler/terminal_log.cpp#L474)

#### 22. Fetch API and HTTP connection lifecycle — 1,567 LOC · retain · 0 net · risk 4

The direct `rt::Fetch`/`rt::fetch` probe finds 33 references in 18 files. Injected `Fetch` functions and `globalFetch` also serve provider and local mock traffic. `HttpClientTransport` owns DNS/connect/TLS, abortable body reads, idle pooling, stale-connection retry and redirect handling with cross-origin credential stripping. Asio and OpenSSL provide transport primitives. Beast can handle HTTP exchange operations, and `ssl::stream` can replace manual BIO plumbing only after proving cancellation and shutdown equivalence. Neither supplies the project's pool reuse, body ownership or replay policy. Keep the client facade while parser rows 7–8, wire row 23, TLS row 25 and the Headers/WebSocket lanes qualify independently. [Fetch seam](../../src/rt/fetch.cpp#L174) · [Pool and redirect](../../src/rt/fetch_transport.cpp#L335) · [TLS I/O](../../src/rt/fetch_transport_net.cpp#L80)

#### 23. Fetch wire request writer and byte queue — 236 LOC · retain pending parser and Headers integration · 0 net · risk 3

`fetch_transport.cpp` builds the request head and reads through `Connection`'s `ByteQueue` in `fetch_transport_net.cpp`. `requestHead` projects validated names/Latin-1 values, separate Set-Cookie values, user overrides and Host, length/transfer framing and encoding defaults. Beast's header serializer and `flat_buffer` could replace the hand formatting and queue mechanics. The feature fits the parser's buffer and serializer choice. Installing both buffer representations during parallel integrations repeats ownership and copying, so defer any credit until row 7 and row 28 settle. Preserve duplicate and credential-header checks and unique unambiguous body framing. [Writer](../../src/rt/fetch_transport_http.cpp#L162) · [Buffer](../../src/rt/fetch_transport_internal.hpp#L138)

#### 24. Fetch content decoding — 366 LOC · retain · 0 net · risk 4

`fetch_transport.cpp` chooses a `ContentDecoder` after reading headers and pumps decoded bytes to the body reader. gzip/x-gzip and deflate use row 27's zlib inflater, and Brotli and Zstd already use their streaming C APIs. The adapter selects available codings, emits `Accept-Encoding`, distinguishes the current deflate envelopes, applies gzip member/trailing policy and surfaces corruption, truncation and cancellation to the consumer. Boost.Iostreams would introduce a link/profile change while leaving those boundaries and bounded output responsibility in the gateway. Keep row 27's streaming interface and codec policies independent of the whole-buffer lane and HTTP parser. [Selection](../../src/rt/fetch_transport_codec.cpp#L309) · [Pump](../../src/rt/fetch_transport.cpp#L335)

#### 25. TLS context, trust and hostname binding — 286 LOC · retain pending independent equivalence · 0 net · risk 5

`fetch_transport_net.cpp` and `websocket_socket.cpp` use system/extra CA trust, TLS 1.2 minimum, SNI for DNS, OpenSSL chain results and the shared post-handshake name check before sending ordinary requests. OpenSSL can replace hand-written SAN/CN and IP comparison using `X509_check_host`, `X509_check_ip_asc` or verification parameters. The current DNS wildcard, A-label, trailing-dot, SAN precedence and IP-only SAN decisions need an exact acceptance matrix before swapping. Setting verification flags alone can change accepted names. Preserve stable failure classes, the existing explicit trust controls and cancellation. Keep this distinct from passkey X.509 parsing, fetch connection ownership and the active WebSocket lane. [Trust](../../src/rt/tls.cpp#L105) · [Name check](../../src/rt/tls.cpp#L197) · [Fetch verification](../../src/rt/fetch_transport_net.cpp#L123) · [WSS verification](../../src/rt/websocket_socket.cpp#L347)

#### 26. WebCrypto and Node crypto API — 1,286 LOC · retain security boundaries · 0 net · risk 5

`utils.cpp` and startup use digest/hash, admission and credentials use UUIDs/random bytes, Passport and passkeys verify RSA/JWK, and Sentinel uses AES-GCM, HKDF/HMAC and zeroization with bound AAD. The digest, MAC, signature, cipher, KDF and RNG engines are already EVP/RAND. The long surface combines JWK import/export, algorithm/key-usage checks and Bun-style coercion. Typed OpenSSL adapters could avoid unused WebCrypto convenience paths once a per-algorithm reachability and key-format matrix is available. Direct ad hoc EVP at each site would duplicate checks. Keep key acceptance, tag lengths, context separation, constant-time comparison and errors used for classification. Keep separate from rows 3 and 10–12 and the current passkey lane. [API](../../include/uos/rt/crypto.hpp#L103) · [Passport](../../src/auth/passport.cpp#L200) · [Sentinel](../../src/sentinel/replay_envelope.cpp#L439)

#### 27. CompressionStream and DecompressionStream — 609 LOC · narrow · 45–75 net · risk 3

The application has four direct calls in `catalog/store.cpp` and `sentinel/replay_envelope.cpp`. It uses whole-buffer gzip and gunzip and bounded piece delivery for Sentinel. The implementation exposes zlib `Inflater` plus synchronous and Task wrappers. The application already has zlib and the `CompressionStream` name describes the source behavior. Replace legacy Blob-size segmentation in the whole-buffer path with a bounded zlib input loop, and simplify its two adapters where scheduling equivalence is proved. Preserve concatenated members, old compressed payloads, work/byte ceilings and plaintext scrubbing even if valid gzip bytes differ. Retain the streaming inflater, zlib link/loader seam and Fetch coding policy. [Whole-buffer functions](../../src/rt/gzip.cpp#L384) · [Bounded envelope reader](../../src/sentinel/replay_envelope.cpp#L417) · [Brief](../briefs/gzip.md)

#### 28. Headers — 331 LOC · narrow · 55–95 net · risk 3

`rt::Headers` has 218 qualified references in 83 application files. Auth reads individual fields, the server builds incoming headers and emits Set-Cookie separately, and provider/relay code copies, filters and appends. The current storage keeps a sorted field vector, a parallel spelling vector, a separate cookie list and a duplicate joined Cookie value. Consolidate per-field data in one STL record, preserve the separate cookies and remove the Bun-only spelling lookup used to format errors. Beast `http::fields` stores multiple values and supports case-insensitive lookup. Its container does not implement current Cookie/Set-Cookie projection or string/ByteString validation by itself, and a direct alias would change callers. Coordinate with the active Headers lane. Keep the HTTP parser, Fetch writer and WebSocket upgrade ownership separate. [Storage and validation](../../src/rt/headers.cpp#L9) · [Incoming and outgoing server headers](../../src/runtime/http.cpp#L1562) · [Brief](../briefs/headers.md)

#### 29. Web Request, Response, Body, Blob and File — 947 LOC · narrow upload types · 75–125 net · risk 3

`Request`, `Response` and `BodyInit` have 1,791 qualified matches in 126 files. Core usage includes single consumption, cloning for bounded observation/classification, shared metadata and streaming cancellation. Keep that surface in this lane. Only Images and Live use `File` at application entrypoints. They need owner-backed bytes, a filename and detected media type for Images, and decoded text for a Live part. A struct, `std::variant` and the existing UTF decoder can represent incoming parts without generic Blob construction, File inheritance and JS Blob chunking at that boundary. Confirm remaining request-body variants and move their tests to equivalent explicit input cases before deleting those constructors. Coordinate with row 30 in the same files. Leave Headers, Fetch and stream internals to rows 22, 28 and 38. [Body handling](../../src/rt/http.cpp#L690) · [Image use](../../src/images.cpp#L121) · [Live use](../../src/live/calls.cpp#L58) · [Brief](../briefs/multipart.md)

#### 30. FormData and multipart wire format — 295 LOC · narrow to the inbound path · 120–160 net · risk 3

Two application calls to `.formData()` are in Images and Live. They parse buffered requests and read ordered parts, first matches or all matches. The outbound writer and general mutation API are reached by runtime compatibility fixtures. `std::vector` and `std::variant` fit the inbound result directly. Beast parses HTTP framing, with multipart parsing a separate responsibility. Keep bounded multipart decoding and its reachable URL-encoded compatibility, and retire production outbound FormData after preserving the fixture coverage. The two routes must retain duplicate handling, file/text distinction, content-type and boundary decisions, and byte/work budgets. Own this with row 29 in the same lane, with row 31's MIME classification and HTTP parser rows protected. [Parser](../../src/rt/http.cpp#L576) · [Writer](../../src/rt/http.cpp#L163) · [Image selects duplicates](../../src/images.cpp#L238) · [Brief](../briefs/multipart.md)

#### 31. Bun MIME extensions and content sniffing — 342 LOC · retain decision table · 0 net · risk 3

`parseMultipart` calls `typeForExtension`, then sniffs bytes for unknown extensions. Images accepts PNG/JPEG/WebP and checks mask media type. It consults the filename fallback only when the detected type is empty or `application/octet-stream`. A recognized non-image extension blocks sniffing even if the bytes have image magic. Live uses file text. A three-image allowlist by itself would turn recognized non-image extensions into unknowns; trusting the part's Content-Type would change validation. At this dependency closure neither Beast nor ICU supplies a stable MIME database. After the inbound lane, a mature platform-independent classifier is eligible only when it preserves these decisions. Own the extension table, sniff and precedence separately from row 30's parser. [Table and sniff](../../src/rt/http.cpp#L234) · [Precedence](../../src/rt/http.cpp#L656) · [Media validation](../../src/images.cpp#L121)
