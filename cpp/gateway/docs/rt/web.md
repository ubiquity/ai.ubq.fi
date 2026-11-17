# uos::rt web: URL, Headers, Request/Response, fetch

Oracle: Bun 1.4.0 (WebKit URL parser and FetchHeaders, Bun's own Body, multipart parser
and fetch). Headers: `uos/rt/url.hpp`, `uos/rt/headers.hpp`, `uos/rt/http.hpp`,
`uos/rt/fetch.hpp`. Every behaviour below is pinned by bun-generated vectors in
`tests/rt/{url,headers,http,fetch}_test.cpp`.

## Conventions (binding for every porter)

1. **Values and handles.** `URL`, `URLSearchParams`, `Headers`, `Blob`/`File` and
   `FormData` are values: copying is `new X(other)`. To alias one (`const h =
response.headers`) hold a reference (`rt::Headers& h = response.headers()`).
   `Request` and `Response` are handles: copies are the same JS object (headers, body,
   identity), so pass them by value freely. `==` is `===`.
2. **Strings** are canonical WTF-8 (`docs/rt/text.md`). A header value holds only code
   points up to U+00FF: the HTTP stages map each wire byte b to U+00b and back (Latin-1).
3. Getters are `x()`, setters `x(value)`; JS `null`/`undefined` results are `std::nullopt`.
   A JS method named like a C++ keyword gets `_` (`delete_`).
4. **Body readers** (`text()`, `json()`, `bytes()`, ...) do their synchronous part when
   called (validation, marking the body used, locking a stream), as JS does, and return a
   `Task`. GCC 12 rule (see `docs/rt/async.md`): never `co_await` a call that takes a
   `RequestInit`/`ResponseInit`/lambda temporary or pass a `co_await` result straight into a
   call; bind the Task to a named local first:
   `auto call = rt::fetch(url, std::move(init)); rt::Response r = co_await std::move(call);`.
5. **Errors** carry Bun's text. `rt::InvalidURLError` (TypeError "Invalid URL", `.code`
   "ERR_INVALID_URL", `.input`), `rt::NodeURLError` and `rt::CodedTypeError` (TypeError with
   Bun's `.code`); catch `rt::TypeError` where the TS does not inspect `.code`.

## URL and URLSearchParams (`url.hpp`)

| TypeScript                                                                                             | C++                                                                                                                         |
| ------------------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------- |
| `new URL(s)` / `new URL(s, base)` (base string or URL)                                                 | `rt::URL(s)` / `rt::URL(s, base)`; throws `InvalidURLError`                                                                 |
| `new URL(url)`                                                                                         | `rt::URL(url)` (copy; its own searchParams)                                                                                 |
| `URL.canParse(s[, base])` / `URL.parse(s[, base])`                                                     | `rt::URL::canParse(...)` / `rt::URL::parse(...)` -> `std::optional<URL>`                                                    |
| `url.href`, `String(url)`, `` `${url}` ``, `url.toString()`, `url.toJSON()`                            | `url.href()` (`toString()`, `toJSON()` are the same)                                                                        |
| `url.href = s`                                                                                         | `url.href(s)` (throws `InvalidURLError`)                                                                                    |
| `origin`, `protocol`, `username`, `password`, `host`, `hostname`, `port`, `pathname`, `search`, `hash` | `url.origin()`, ... ; setters `url.search("")`, `url.hash("")`, ... (WebKit's setter rules, including its host/port quirks) |
| `url.searchParams`                                                                                     | `url.searchParams()` -> `URLSearchParams&`, linked both ways                                                                |
| `new URLSearchParams("?a=1")`                                                                          | `rt::URLSearchParams("?a=1")`                                                                                               |
| `new URLSearchParams([["a","1"]])`                                                                     | `rt::URLSearchParams({{"a","1"}})`; items of any length: `URLSearchParams::fromSequence(...)` (TypeError "Type error")      |
| `new URLSearchParams({ a: "1" })`                                                                      | `URLSearchParams::fromRecord({{"a","1"}})` (JS key order: array-index keys first)                                           |
| `p.get(k)`                                                                                             | `p.get(k)` -> `std::optional<std::string>`                                                                                  |
| `getAll`, `has(k[, v])`, `set`, `append`, `delete(k[, v])`, `sort()`, `toString()`, `size`             | `getAll`, `has`, `set`, `append`, `delete_`, `sort` (UTF-16 order, stable), `toString`, `size()`                            |
| `[...p]`, `[...p.keys()]`, `[...p.values()]`                                                           | `p.entries()`, `p.keys()`, `p.values()` (snapshots)                                                                         |
| `for (const [k, v] of p)`, `p.forEach((v, k) => ...)`                                                  | range-for / `p.forEach(...)`: live, mutations are observed like JS                                                          |
| node:url `pathToFileURL(p)` / `fileURLToPath(u)`                                                       | `rt::pathToFileURL(p)` / `rt::fileURLToPath(u)` (`NodeURLError` with Node's codes)                                          |
| `new URL(rel, import.meta.url)`                                                                        | no rt global: the module port supplies its file URL (`rt::pathToFileURL(dir + "/")`)                                        |

## Headers (`headers.hpp`)

| TypeScript                                                               | C++                                                                                           |
| ------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------- |
| `new Headers()` / `new Headers({ a: "1" })` / `new Headers([["a","1"]])` | `rt::Headers()` / `rt::Headers{{"a","1"}}` (appends in order)                                 |
| items of any length                                                      | `Headers::fromSequence(...)` (TypeError "Header sub-sequence must contain exactly two items") |
| `new Headers(other)` / `new Headers(init?.headers)`                      | `rt::Headers(other)` / `rt::Headers(init.headers.value_or(rt::Headers()))`                    |
| `HeadersInit` parameter or field                                         | `rt::Headers`                                                                                 |
| `h.get(n)`                                                               | `h.get(n)` -> `std::optional<std::string>` (Set-Cookie values joined by ", ")                 |
| `append`, `set`, `has`, `delete`, `getSetCookie()`                       | `append`, `set`, `has`, `delete_`, `getSetCookie()`                                           |
| `h.count` (Bun)                                                          | `h.count()`                                                                                   |
| `[...h]`, `[...h.keys()]`, `[...h.values()]`                             | `h.entries()`, `h.keys()`, `h.values()` (snapshots)                                           |
| `for (const [n, v] of h)`, `h.forEach((v, n) => ...)`                    | range-for / `h.forEach(...)`: live                                                            |

Bun facts kept: names lowercased; `append` joins ordinary headers with ", " and Cookie with "; "; `get("cookie")` and iteration expose that same Cookie join. Iteration yields the other names in code point order, then every Set-Cookie value; values lose leading/trailing
tab/LF/CR/space and must have no NUL/LF/CR and no code point above U+00FF; errors
`Invalid header name: '<name>'` (`has()` ends it with `"`), `Header '<name>' has invalid
value: '<stripped value>'` where `append`/constructors spell a WebKit common header
canonically (`Content-Type`) and `set` uses the name as given.

Wire spelling (for the HTTP client, not a JS API): a Headers object also keeps each name as the
caller first spelled it since it was last absent (later appends and sets in other spellings keep
it; a delete forgets it; copies keep it), and the Cookie value with appended parts joined by
"; ". `rt::detail::headerWireFields(headers)` returns them; rt::HttpClientTransport writes request
heads from it as Bun does (docs/rt/fetch-transport.md). JS-visible names remain lowercased; Cookie uses the same "; " join in `get` and iteration, while ordinary headers retain ", " and Set-Cookie retains separate iteration entries.

## Blob, File, FormData (`http.hpp`)

| TypeScript                                                                      | C++                                                                                                      |
| ------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- |
| `new Blob(parts, { type })`                                                     | `rt::Blob({rt::BlobPart("text"), rt::BlobPart(bytes), rt::BlobPart(blob)}, type)`                        |
| `blob.size`, `blob.type`, `blob.text()`, `arrayBuffer()`, `bytes()`, `stream()` | same names; `size()` is a double                                                                         |
| `new File(parts, name, { type, lastModified })`                                 | `rt::File(parts, name, type, lastModified)`                                                              |
| `file.name`                                                                     | `file.name()` -> `std::optional<std::string>` (see limits)                                               |
| `x instanceof Blob` with x a FormData value                                     | `std::holds_alternative<rt::File>(x)`                                                                    |
| `new FormData()`, `append`, `set`, `get`, `getAll`, `has`, `delete`             | `rt::FormData`, same names (`delete_`); values are `FormData::Value` = `std::variant<std::string, File>` |
| `typeof v === "string"`                                                         | `std::holds_alternative<std::string>(v)`                                                                 |
| `[...form.entries()]`, `form.keys()`                                            | `form.entries()`, `form.keys()`; range-for                                                               |

Blob types: one of `application/javascript`, `application/json`,
`application/x-www-form-urlencoded`, `text/css`, `text/html`, `text/javascript`,
`text/jsx`, `text/plain` spelled exactly becomes Bun's canonical type with charset; any
other type is lowercased, or "" when it has a character outside U+0020..U+007E.

## Request and Response (`http.hpp`)

| TypeScript                                                                                        | C++                                                                                                                                                                                                            |
| ------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `BodyInit`                                                                                        | `rt::BodyInit` = variant of `nullptr` (null), `std::string`, `rt::Bytes`, `rt::ByteStream`, `URLSearchParams`, `FormData`, `Blob`                                                                              |
| `new Response(body, { status, statusText, headers })`                                             | `rt::Response(rt::BodyInit(body), rt::ResponseInit{.status = 201, .statusText = "...", .headers = h})`                                                                                                         |
| `new Response()` / `new Response(null, init)`                                                     | `rt::Response()` / `rt::Response(nullptr, init)`                                                                                                                                                               |
| `new Response(other.body, init)`                                                                  | `rt::Response(rt::bodyInit(other.body()), init)` (null stays null)                                                                                                                                             |
| `Response.json(data, init)`                                                                       | `rt::Response::json(jsonValue, init)`                                                                                                                                                                          |
| `Response.redirect(url, status)` / `Response.error()`                                             | `rt::Response::redirect(url, status)` / `rt::Response::error()`                                                                                                                                                |
| `status`, `statusText`, `ok`, `headers`, `url`, `redirected`, `type`, `body`, `bodyUsed`          | same names as getters; `headers()` -> `Headers&`; `body()` -> `std::optional<ByteStream>`                                                                                                                      |
| `await r.text()` / `json()` / `arrayBuffer()` / `bytes()` / `formData()`                          | `co_await` the same names (`json()` -> `json::value`, bytes -> `rt::Bytes`)                                                                                                                                    |
| `r.clone()`                                                                                       | `r.clone()`                                                                                                                                                                                                    |
| `new Request(url, init)` (string or URL)                                                          | `rt::Request(url, rt::RequestInit{.method = "POST", .headers = h, .body = b, .signal = s})`                                                                                                                    |
| `new Request(request)` / `new Request(request, init)`                                             | `rt::Request(request, {})` / `rt::Request(request, rt::RequestInit{...})` (never `rt::Request copy(request)`: that aliases)                                                                                    |
| `new Request(url, request)` (a Request as init)                                                   | `rt::Request(url, request)`                                                                                                                                                                                    |
| `method`, `url`, `headers`, `signal`, `redirect`, `cache`, `body`, `bodyUsed`, readers, `clone()` | same names                                                                                                                                                                                                     |
| `RequestInit.redirect` / `cache` / `duplex`                                                       | `std::string` fields, validated like Bun (`duplex` accepted and ignored)                                                                                                                                       |
| `WeakMap<Request, V>` / `WeakSet<Response>`                                                       | `rt::WeakMap<rt::Request, V>` (`get`, `has`, `set`, `delete_`) / `rt::WeakSet<rt::Response>` (uos/rt/weak.hpp, included by http.hpp; any key type with `identity()`/`weakIdentity()`: rt::Kv, rt::Promise too) |
| a partial init (`{ status: 201 }`, `{ method: "POST" }`)                                          | `rt::ResponseInit{.status = 201}` / `rt::RequestInit{.method = "POST"}`: every member defaults to undefined (std::nullopt), so omitted members compile under -Wextra                                           |

Bun facts kept: no content-type for string bodies; URLSearchParams/FormData/typed Blob
bodies add theirs when `init.headers` is given, otherwise on the first `headers()` access
and only if the body is still an untouched buffer; statuses are truncated toward zero
(NaN 0, infinities saturate) and must be 101 or 200..599 (RangeError "The status provided
(n) must be 101 or in the range of [200, 599]"); no null-body-status check; status 101 has
type "error"; `Response.error()` has a non-null empty body; methods: Bun's known methods
in upper or lower case, anything else "GET"; GET/HEAD requests may have a body; a
buffered body read as a stream gives a 16 KiB chunk then chunks of up to 2 MiB; reading a
string body returns the original string (lone surrogates, BOM) until a clone turns it into
bytes; `json()` of a buffered body throws "Unexpected end of JSON input" (no bytes) or
"Failed to parse JSON", of a stream body JSON.parse's messages; a second read rejects with
`CodedTypeError` "Body already used" (ERR_BODY_ALREADY_USED); `clone()` of a used or
locked body throws "Body is disturbed or locked"; `formData()` decodes urlencoded or
multipart (Bun's own parser: file types come from the filename extension through Bun's
1124-entry table, else PNG/JPEG/GIF89a/BMP/TIFF sniffing; the part's Content-Type is
ignored) and fails with "Can't decode form data from body because of incorrect MIME
type/boundary" or "FormData parse error ..." (ERR_FORMDATA_PARSE_ERROR); a FormData body
gets a random `----WebKitFormBoundary<32 hex>` boundary.

## fetch (`fetch.hpp`)

| TypeScript                                                                                                       | C++                                                                                                                |
| ---------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------ |
| `typeof fetch`, `(input: RequestInfo \| URL, init?: RequestInit) => Promise<Response>`                           | `rt::Fetch` (from a lambda `(rt::RequestInfo, rt::RequestInit) -> Task<Response>` or `-> Response`)                |
| `RequestInfo \| URL`                                                                                             | `rt::RequestInfo` = `std::variant<std::string, URL, Request>`                                                      |
| `fetch(input, init)`                                                                                             | `rt::fetch(input, std::move(init))` (calls the current global)                                                     |
| `globalThis.fetch` / `globalThis.fetch = f`                                                                      | `rt::globalFetch()` / `rt::setGlobalFetch(f)`                                                                      |
| `const original = globalThis.fetch; globalThis.fetch = mock; try {...} finally { globalThis.fetch = original; }` | `rt::GlobalFetchOverride scope(mock);`                                                                             |
| `globalThis.fetch === defaultFetch`                                                                              | `rt::globalFetch() == defaultFetch` (identity)                                                                     |
| `options.fetcher ?? globalThis.fetch`                                                                            | `options.fetcher ? options.fetcher : rt::globalFetch()` with the option held as an `rt::Fetch` (empty = undefined) |
| tests/helpers/fetch-mock.ts `createFetchMock(handler)`                                                           | `rt::Fetch(handler)`; transport-level tests: `rt::MockTransport`                                                   |

`rt::defaultFetch()` (the initial global) reproduces Bun's front end: URL errors "fetch()
URL must not be a blank string." / "fetch() URL is invalid" (ERR_INVALID_URL), schemes other
than http:, https:, s3:, file:, data: "protocol must be http:, https: or s3:"
(ERR_INVALID_ARG_VALUE), "fetch() request with GET/HEAD method cannot have body.", a used
Request input "Request body already used" (ERR_BODY_ALREADY_USED); validation precedes the
abort check; a Request input's body is taken (the input becomes used); an aborted signal
rejects with `signal.reason` (any value) before or during the request, and after the
response errors its body with the reason (even a fully received one).

## Limits

- **IDNA**: hosts are mapped with the Latin-1 subset of UTS #46 (all ASCII and Latin-1
  inputs, including Latin-1 text decoded from wire headers, are exact). A host with a code
  point above U+00FF, or an `xn--` label that decodes to one, throws `InvalidURLError` where
  Bun applies full UTS #46 (e.g. `http://你好你好` parses to `xn--6qqa088eba` in Bun).
- Not provided because no TS code uses them: `headers.toJSON()`/`JSON.stringify(headers)`,
  `body.blob()`, `Request.mode/credentials/referrer/integrity/keepalive/destination`, fetch's
  Bun-only options (`verbose`, `proxy`, `tls`, `decompress`, `unix`), `fetch.preconnect`.
- `signal: null` is treated as undefined (Bun: a fresh signal for a Request input).
- A buffered body exposed through `body()`, locked by `getReader()` and released without
  a read is still readable in C++ (Bun: "Body already used").
- `ByteStream` chunks are bytes (Bun also accepts string chunks in a body stream).
- `File::name()` is `std::nullopt` where Bun leaves it undefined (a parsed part with an empty
  filename or empty content; a Blob appended without filename); TS reading `.name` there
  throws, so ports must `.value()` it where the TS dereferences it.
- `rt::WeakMap` keeps a value alive while its key lives (a dead key's value until the next
  purge, at a later `set`); a value holding its own key leaks (no ephemeron collection).
- `rt::Headers{...}` is a sequence: a JS record with integer-like header names validates
  them first (unobservable unless such a name has an invalid value).

## Transport seam (for the HTTP client stage)

The HTTP client stage is `rt::HttpClientTransport` (`uos/rt/fetch_transport.hpp`, reference
`docs/rt/fetch-transport.md`): serve's main installs it with `rt::installHttpClientTransport()`;
tests keep using `rt::MockTransport` / `ScopedLoopbackFetch`.

Implement `rt::FetchTransport::send(rt::Request) -> Task<rt::Response>` and install it with
`rt::setFetchTransport`. The front end has already validated the input, built the Request
(absolute `url()` with fragment, Bun's `method()`, `headers()` including the body's content
type, `redirect()`, `cache()` (Bun ignores it), `signal()`) and handles abort: on abort it
abandons the send Task and Asio cancels its pending operations (`operation_aborted` inside
the transport, which must then close the connection); after the response it errors the body
stream and cancels the transport's stream (whose cancel must close the connection).
`send` must:

1. Read the request body through `request.body()` (null, or a `ByteStream` to send as it
   is produced) and write header names/values as Latin-1 bytes; Set-Cookie values are
   separate entries.
2. Resolve once the response head arrived with `rt::Response(rt::BodyInit(stream),
{.status, .statusText = reason phrase, .headers = appended in wire order})`, the body a
   `ByteStream` fed as bytes arrive, then `response.setFetchResult(finalUrl, redirected)`.
3. Redirects per `request.redirect()`: "follow" (redirected = true, url = final URL),
   "manual" (the 3xx response as is, url = the request URL), "error" (reject TypeError
   "UnexpectedRedirect fetching \"<url>\". For more information, pass `verbose: true` in
   the second argument to fetch()", code "UnexpectedRedirect").
4. Map network failures to Bun's TypeErrors, e.g. connection refused "Unable to connect. Is
   the computer able to access the url?" (code "ConnectionRefused") and DNS failure
   "getaddrinfo ENOTFOUND <host>" (code "ENOTFOUND"); pin the rest against Bun.
5. Pin by test against Bun: decompression, chunk boundaries, header ordering, keep-alive.
