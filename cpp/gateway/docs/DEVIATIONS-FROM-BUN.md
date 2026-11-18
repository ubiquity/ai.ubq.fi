# C++ gateway runtime library behavior

## Date parsing

Date parsing accepts ISO calendar dates, ISO timestamps with optional fractional
seconds and numeric UTC offsets, and IMF-fixdate with a matching weekday.
Unsigned four-digit years and signed six-digit years retain the existing UTC
representation and TimeClip range. Standardized date-only year/month forms and
the existing UTC default for an omitted offset remain supported.

| Changed parser rows | Change                                                                                                                                       |
| ------------------: | -------------------------------------------------------------------------------------------------------------------------------------------- |
|                 352 | Legacy numeric, named-month and time-zone forms are rejected. Examples include `1/2/2020`, `Jan 1` and `2020-01-01 10:00 PST`.               |
|                 151 | Legacy weekday formats are rejected, including `Wednesday, 21-Oct-15 07:28:00 GMT`. Gateway HTTP dates use IMF-fixdate.                      |
|                  20 | ISO widths, field ranges and calendar dates are checked. For example, `2021-02-29` no longer rolls into March.                               |
|                   9 | IMF-fixdate fields must be valid and canonical. For example, `Fri, 25 Feb 1989 09:17:12 GMT` has a mismatched weekday.                       |
|                 175 | Leading and trailing whitespace, controls and non-ASCII bytes are rejected. For example, `2020-01-01\0garbage` is rejected as a whole input. |
|                  72 | Parenthetical legacy tokens are rejected. For example, `2020-01-01 (comment)` is invalid.                                                    |
|                   4 | Fractional seconds use the first three digits, right-padded for shorter fractions. `.0001234567891` yields 0 ms, where Bun recorded 123 ms.  |
|                   1 | IMF-fixdate years are literal Gregorian years. `Sat, 01 Jan 0000 00:00:00 GMT` denotes year 0000, where Bun recorded 2000.                   |

The stricter grammar removes ambiguous date interpretations from validation.
The retained ISO and IMF forms cover the gateway's persisted timestamp and
retry-deadline fixtures. All 2,050 parser inputs remain covered.

## Headers diagnostics

An invalid value passed to `Headers::append` or to a constructor reports a
lowercase field name, limited to 64 characters, and omits the rejected value.
For example, appending a value containing a newline to `Content-Type` throws
`TypeError: Header 'content-type' has invalid value`. The diagnostic carries
the field identity without printing untrusted value bytes. All 170 affected
vectors still require rejection. HTTP field validation and serialization
continue to use the original name and value rules.

## Explicit environment files

The parser accepts a bounded flat INI file using Boost.PropertyTree and
C++ quoted strings. Single-line assignments, leading BOM, CRLF, inline
hash comments and literal dollar references remain supported. Existing
environment variables still take precedence when an explicit file is loaded.

| Changed behavior                                                                                           | Example                | Effect                                                                                             |
| ---------------------------------------------------------------------------------------------------------- | ---------------------- | -------------------------------------------------------------------------------------------------- |
| Duplicate keys, sections, invalid keys and incomplete lines fail the whole file                            | `A=1\nA=2`             | A malformed file cannot partially populate the environment.                                        |
| Colon assignments, `export` prefixes and split assignments are rejected                                    | `export A=1`           | The accepted grammar is a flat assignment file.                                                    |
| Quotes must end on the same line and may be followed by whitespace or a hash comment                       | `A="x" junk`           | Incomplete and ambiguous values fail atomically.                                                   |
| C++ quoted extraction consumes the backslash before the next character                                     | `A="a\nb"` gives `anb` | The format has no escape-to-control conversion or expansion. Unquoted backslashes remain literal.  |
| ASCII control bytes other than tab and line endings, interior bare CR, and input over 256 KiB are rejected | `A=x\x00y`             | Values cannot introduce embedded NUL or line boundaries, and parser allocation has an input bound. |

Sixty-eight of the original 124 inputs now reject the whole file. Nine
retain valid assignments with changed quoted values.

## Filesystem text and POSIX paths

Text file reads and writes use the shared runtime UTF-8 codec. The tested
BOM, malformed maximal subparts, split surrogate pair and raw-byte
expectations held. Path functions use the C++ lexical model and preserve
native POSIX path bytes. Resolve and join retain their existing
interpretation of absolute components.

| Changed lexical vectors | Example                                                               | Standard behavior                                                                                                               |
| ----------------------: | --------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
|                       2 | `normalize("a/b/c/..")` gives `"a/b/"`                                | Lexical normalization may leave a final separator after removing a dot or parent element. Resolve still removes that separator. |
|                      11 | `dirname("/a/b/")` gives `"/a/b"` and `dirname("/a//b")` gives `"/a"` | A trailing separator denotes an empty final component, and repeated separators are collapsed in the result.                     |
|                      14 | `basename("a/")` and `extname("/a/b.c/")` give an empty string        | A trailing separator has no filename or extension. The same rule applies to the suffix overload and root.                       |
|                       2 | `basename("/a/b.txt", "b.txt")` gives an empty string                 | A matching suffix is removed from the filename even when it spans the entire component.                                         |

Relative paths require resolvable absolute endpoints. A lookup failure
cannot appear to a containment caller as an empty, equal relative path.

## Console formatting

Structured diagnostics now use compact Boost.JSON serialization. The
console still accepts the existing values and placeholder tokens, and the
sink uses the original method to select stdout, stderr and capture routing.
Plain strings and preformatted application events retain their bytes
within the output limit.

| Changed behavior                                                                        | Example                                                                                                              | Effect                                                                                                                                                                        |
| --------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Arrays and objects use compact JSON with quoted keys and insertion order                | `{a: 1}` displays as `{"a":1}`                                                                                       | The same diagnostic fields remain readable, without Bun's columns, line wrapping or numeric-key sorting.                                                                      |
| JSON projects nonfinite numbers to null and omits undefined object members              | `{missing: undefined, reading: NaN}` displays as `{"reading":null}`                                                  | Structured values use standard JSON. Standalone undefined remains visible and an undefined `%j` stays empty.                                                                  |
| Errors use JSON name, message, optional properties and nested causes                    | `Error("outer", {cause: Error("inner")})` displays an error object with `"cause":{"name":"Error","message":"inner"}` | Fields such as code, status and errno remain available. JSON escaping keeps newlines in error messages within the record.                                                     |
| Scalars use `to_chars`, and numeric substitutions parse complete text with `from_chars` | `%d` with `"0x10"` gives `nan`, and `%d` with `1e21` gives `1e+21`                                                   | Negative zero, infinity, exponent spelling, and numeric text follow the standard functions. Arrays and objects use JSON instead of JavaScript coercion for `%s`.              |
| Boost.JSON writes fractional values in standard exponent notation                       | `%j` with `1.5` gives `1.5E0`                                                                                        | JSON consumers read the same number.                                                                                                                                          |
| Projection and output are bounded                                                       | A nested value beyond depth 16 or an array beyond its first 100 elements uses `"[truncated]"`                        | The projection also charges text and nodes against 32 KiB, and the complete formatted line is capped at 256 KiB. Malformed structured UTF-8 is replaced before serialization. |

## Unicode case mapping and default collation

The C++ runtime follows the Unicode data in the linked ICU release. On ICU 78.3, the existing Bun 1.4.0 case and collation vectors keep their expected values. The gateway uses ICU root-locale collation independently of machine locale.

Outside the old port's recorded collation repertoire, ICU supplies the order and canonical equivalences. With ICU 78.3, Deseret U+10400 sorts before the lone surrogate U+D83D, and `가` and `가` compare equal. Both results match Bun 1.4.0. This extends collation to names in additional scripts and keeps tertiary ordering for catalog and schema names.

## Server HTTP request parsing with llhttp 9.4.4

| Input or behavior                                                                               | Bun 1.4.0                                                                                   | C++ server with llhttp                                                                   | Reason                                                                                                      |
| ----------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| Request transfer coding without a final `chunked` coding, such as `Transfer-Encoding: identity` | An unread request can reach the handler and produce a response before the parser sends 400. | Sends HTTP 400 before dispatch.                                                          | A request body needs unambiguous framing.                                                                   |
| Two identical `Content-Length: 1` fields                                                        | Accepts the request and reads one byte.                                                     | Sends HTTP 400 before dispatch.                                                          | A single length field avoids ambiguity between HTTP peers.                                                  |
| ASCII DEL in a field value, such as `X-Test: a<DEL>b`                                           | Preserves the byte in `rawHeaders`.                                                         | Sends HTTP 400.                                                                          | Strict llhttp rejects DEL in HTTP field values. Valid Latin-1 field bytes retain their existing projection. |
| Literal UTF-8 in a request target, such as the bytes for `/café`                                | Decodes the bytes into the incoming URL.                                                    | Sends HTTP 400.                                                                          | Valid URI targets can send `/caf%C3%A9`.                                                                    |
| `Proxy-Connection: close` without `Connection`                                                  | Keeps the HTTP/1.1 connection alive.                                                        | Uses llhttp's close decision.                                                            | The client receives `Connection: close` and can open a fresh connection.                                    |
| A 4,192-byte chunk extension value or two 8,300-byte trailer values                             | Accepts those requests.                                                                     | Limits each chunk size line to 4,096 bytes and the full trailer block to 16,408 bytes.   | Caps metadata buffering across socket reads.                                                                |
| Parser error details after dispatch, such as a missing CR after chunk data                      | The incoming request can report `ECONNRESET`.                                               | The parser reports its own error code and reason when the request has an error listener. | The server retains the abort and HTTP 400 behavior.                                                         |

The first four rows correspond to `te-identity-unread`,
`duplicate-equal-content-length`, `info` and `info-utf8-target` in the
server wire vectors. For the `info` case, the standalone parser test also
exercises accepted OWS, mixed-case names, ordered duplicates, empty values and
Latin-1 bytes.

## Header replacement diagnostics

`Headers::set` uses the same bounded `TypeError` diagnostic as `append` for
invalid values. It reports the lowercase name, limited to 64 characters, and
omits the value. The generated tests still require rejection for all 131
affected inputs.

## Inbound form bodies

Images and Live decode the bounded request bytes into ordered text and file
parts. Each file owns its bytes, an optional filename, and its inferred media
type. Multipart parsing retains the existing boundary handling, extension table,
and byte sniffing. URL-encoded parsing uses the shared UTF-8 decoder and
`URLSearchParams`.

The C++ body API retires generic Blob and File constructors, outbound FormData
serialization, and asynchronous `Request::formData()` and
`Response::formData()` methods. Fixtures with multipart or typed byte bodies
supply the wire bytes and explicit Content-Type. Ordinary body readers and
URLSearchParams content-type inference continue through the existing Body API.

## Literal console arguments and shared JSON serialization

The gateway joins arguments with spaces and appends a newline at the
sink. All text is literal, including percent sequences and pre-serialized
event payloads. Structured fields and error diagnostics use the gateway's
compact JSON serializer. Nonfinite numbers become `null`, signed zero
becomes `0`, and lone UTF-16 surrogates in structured strings and keys
are escaped. The serializer replaces malformed UTF-8 and orders numeric
object keys using its usual JSON ordering.

Error diagnostics contain the name, message, known code and cause chain.
OS errors carry the existing path, destination, descriptor, syscall and
errno fields when present. Structured projection remains bounded to
depth 16, 100 array elements and a 32 KiB budget for text and nodes, with
the 256 KiB cap on the combined text. Captures retain independent text
arguments and the original event prefix and payload.

## Process environment snapshot

The native runtime takes its first-use snapshot from the OS environment.
This replaces the explicit file parser described above. The native service
receives operator-selected values through its launcher environment. A `.env`
file in the working directory or home has no effect on the C++ view. An
explicit empty value remains present, and scoped overrides affect only the
view. Subprocesses without an explicit environment inherit the OS values.
[Launch and offline tools](RUNTIME.md#launch-and-offline-tools)

## Native lexical path adapters

The gateway retains `join`, `resolve`, `relative`, `dirname` and `isAbsolute`.
Single-segment `join` covers the former normalization helper. The filename and
extension vectors exercise `std::filesystem::path` directly. Resolve restarts at
the last absolute segment, drops terminal separators except at the root, and
uses the current directory for relative inputs. Relative comparisons return an
empty string for equal paths and reject a comparison without absolute endpoints.
Percent-encoded and backslash bytes stay literal in these operations.

## Ordered containers

Catalog membership checks use `std::unordered_set<std::string>`. The
catalog arrays and vectors determine model order. An OpenRouter payload
keeps the first valid row for each ID. Catalog merges that check against
an existing list still use that list as the membership snapshot.

Ordered Map and Set store stable list nodes. Erasing a key removes it
from lookup immediately and keeps its node readable by active traversals.
Reinserting the key appends a fresh node. A traversal can visit entries
appended before it reaches the end. A C++ iterator already at the list's
end remains there.

## Whole-buffer gzip

Whole-buffer decoding feeds fixed 64 KiB slices into the zlib inflater.
Callbacks receive pieces up to 64 KiB. Compression and decoding return
the complete bytes. The inflater still validates every gzip member and
applies the format's trailing-data policy. Sentinel checks its plaintext
limit before appending each piece and schedules both decode outcomes by
one microtask. Fetch uses its existing streaming decoder.

## Fetch and WebSocket-client HTTP responses with llhttp 9.4.4

- **Status line and line endings.** Bun 1.4.0 fetch accepts `HTTP/1.1  200 OK`, `HTTP/1.2 200 OK`, and response heads with bare LF line endings. Strict llhttp rejects these heads. The gateway uses standard HTTP/1.0 and HTTP/1.1 response lines and CRLF delimiters.
- **Reason phrase spacing.** Bun fetch reads `HTTP/1.1 200  OK` as status text `OK`. llhttp preserves the extra space in the reason phrase. The gateway uses the numeric status for decisions.
- **Duplicate and conflicting lengths.** Bun accepts identical duplicate `Content-Length` fields and responses that contain both `Content-Length` and `Transfer-Encoding`, including `identity`. Strict llhttp rejects those responses before exposing a head. A single message boundary avoids ambiguity.
- **Malformed length errors.** For `Content-Length: abc`, Bun fetch reports `InvalidContentLength`. The gateway reports `Malformed_HTTP_Response` when llhttp rejects the head. Both reject fetch before exposing a response.
- **Chunk delimiters and trailers.** Bun fetch accepts bare-LF chunks, spaces before a chunk size's CRLF, and malformed trailer field names such as `X Trailer`. Strict llhttp validates chunk metadata and trailers. Fetch reports `InvalidHTTPResponse` when the body read reaches invalid chunk syntax.
- **Chunk size padding.** Bun rejects `00000000000000002` as a chunk size because the field exceeds 16 digits. llhttp accepts it as size two and rejects numeric overflow. llhttp parses the digits incrementally with fixed parser storage.
- **Transfer coding order.** Bun fetch accepts `Transfer-Encoding: chunked,` and decodes the chunks. llhttp identifies the response as EOF-delimited. The gateway rejects this disagreement before streaming the body so chunk delimiters cannot reach clients as payload bytes.
- **TRACE response bodies.** Bun 1.4.0 fetch suppresses the TRACE response body in the recorded fixture. The gateway delivers a TRACE payload according to its HTTP framing, as it does for other methods with response bodies. HEAD and bodyless status codes complete at the head. [HTTP Semantics, Section 9.3.8](https://www.rfc-editor.org/rfc/rfc9110.html#section-9.3.8)
- **Response parser errors.** Malformed lengths, conflicting framing and unsupported status-line grammar reject the fetch with a `FetchError` before a response resolves. Invalid chunk syntax rejects the body read with a `FetchError` after the response head resolves. The response tests check the exception type, error code and stage.

## URL, IDNA and URLSearchParams

The gateway uses Ada 4.0.0 for generic URL and form operations. The following differences were observed against Bun 1.4.0.

| Behavior                                                                                                  | Example                                                                                                                                                                                                                                              | Reason                                                                                                                                                                                                                                                                                                  |
| --------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Blob URLs inherit only HTTP and HTTPS origins.                                                            | `blob:ws://a/`, `blob:ftp://a` and `blob:file:///x` have origin `null`.                                                                                                                                                                              | Ada follows WHATWG's origin algorithm. The narrower origin rule grants no additional origin authority.                                                                                                                                                                                                  |
| Non-special schemes have opaque origins.                                                                  | `ftps://h` has origin `null` where Bun returns `ftps://h`.                                                                                                                                                                                           | Ada treats `ftps:` as non-special.                                                                                                                                                                                                                                                                      |
| Non-special authorities can retain an empty path.                                                         | `sc://u@h?q` stays as written. Bun inserts `/` before `?q`. Updating credentials or clearing a path can also leave the path empty in Ada.                                                                                                            | Ada serializes the actual path. The gateway's HTTP(S) paths keep their slash.                                                                                                                                                                                                                           |
| File URL drive letters are normalized with a host, and a drive-only authority can have no trailing slash. | `file://h/C\|` becomes `file://h/C:`. `file://h:?q` becomes `file:///h:?q`.                                                                                                                                                                          | Ada follows its file URL rules. POSIX file conversion still checks local hosts and escaped slashes and preserves literal filenames through escaping.                                                                                                                                                    |
| The host setter uses Ada's port parsing.                                                                  | Setting `host = "x:80"` on `http://a:81/` yields `http://x/`. With `"x:99999"`, Ada updates the host and retains port 81. For `"x:1:2"`, Ada uses the numeric prefix 1. An embedded tab in `"x:\t9"` is stripped before parsing, which gives port 9. | These component mutations use Ada state overrides. HTTP(S) constructors still reject invalid ports and serialize canonical authorities.                                                                                                                                                                 |
| The port setter preserves the port on a control-only assignment.                                          | Setting `port = "\n"` on `http://a:81/` retains 81 in Ada and clears it in Bun.                                                                                                                                                                      | Ada treats the stripped assignment as a failed parse. A genuinely empty assignment still clears the port.                                                                                                                                                                                               |
| Hostname and file host setters reject port syntax.                                                        | Setting `hostname = "[::1]:9"` leaves the URL unchanged. On `file:///x`, assigning `host = "x:"` leaves the host empty.                                                                                                                              | Ada applies hostname-only and file-host rules.                                                                                                                                                                                                                                                          |
| Percent-decoded form bytes are retained even when they are invalid UTF-8.                                 | In `h=%FF`, the value contains byte `0xff`. Bun returns U+FFFD.                                                                                                                                                                                      | Ada's form decoder round-trips the encoded byte. The runtime still converts WTF-8 inputs to USVString where its API accepts JavaScript strings.                                                                                                                                                         |
| Special-scheme host validation is stricter for ContextJ and certain bidirectional domains.                | `https://क्‍ष.example/` and `http://ࡰ.example/` are rejected where Bun accepts them. A Bidi domain with a non-ASCII LTR companion label is also rejected.                                                                                            | Ada can stop at the first joiner and its pinned direction tables have gaps. The gateway refuses those cases and requires LTR companions of RTL labels to be ASCII DNS labels. It also keeps rejecting malformed A-labels, leading combining marks and invalid Bidi companions that raw Ada would admit. |

The diff retains all 2,081 constructor and IDNA limit inputs, 2,475 setter vectors, 148 form/linkage/POSIX steps,
59,850 authority grid inputs, 9,432 setter grid inputs, 1,260 curated IDNA vectors, and 2,228,224 scalar observations.
Compared with the original Bun expectations, 7 explicit constructors, 108 explicit setters, one form step,
27 authority grid blocks, 10 setter grid blocks, 40 curated IDNA rows, and 5 scalar blocks changed. The scalar
sweep accepts 313,410 observations, 144 fewer than Bun. Every observation that both accept keeps its original href.
The old SDK comparison limited IDNA to Latin-1. Ada now applies the same Unicode validation in both build profiles.

[Ada 4.0.0](https://github.com/ada-url/ada/releases/tag/v4.0.0) · [WHATWG URL Standard](https://url.spec.whatwg.org/)

## Passkey CBOR

- Integer and length headers use their shortest encoding. For example, `19 00 18` is rejected because 24 fits in `18 18`. QCBOR supplies the decoded argument and the CTAP2 check verifies its encoded width.
- Map labels must be in CTAP2 order and unique. For example, `a2 02 00 01 00` is rejected because label 1 must precede label 2. Text and integer labels retain their separate types. Leading UTF-8 BOMs in labels are rejected to preserve the prior duplicate-label protection.
- The decoder admits booleans, integers in the previously supported range, byte and text strings, arrays and maps. Tags, null, undefined and other simple values and floats are rejected. For example, a half-precision `-7.0` (encoded as `f9 c7 00`) cannot stand in for the COSE algorithm identifier `-7`.
- Text uses its UTF-8 byte length. The text `é` encodes as `62 c3 a9`. The old encoder used a length of one UTF-16 code unit. Invalid UTF-8 and truncated byte strings are rejected instead of producing a replacement string or a clamped slice.
- Each input is limited to one MiB and eight levels of container nesting. A ninth nested array is rejected even when it is empty. QCBOR also limits the element count of individual arrays and maps.
- A complete attestation object or stored COSE key occupies its entire input. For example, `a0 00` is rejected as a complete item and consumes one byte in prefix mode. Authenticator data uses prefix mode for its COSE key and extensions and checks the final cursor. The Firefox 117 EdDSA map-count correction remains in the authenticator data path for that exact browser encoding.
- Decode errors carry the port's `JsError` type and CBOR validation reason. For example, empty input reports `Invalid passkey CBOR (truncated item)`. The gateway continues to map failed verification to its existing response statuses.

## Passkey ASN.1, X.509 and CRL decoding

- The ECDSA decoder requires a DER sequence of two positive, minimally encoded integers. Bun 1.4.0 unwraps an indefinite sequence such as `30 80 02 01 01 02 01 02 00 00`, a context-specific wrapper, and a negative integer such as `02 01 ff` as an unsigned scalar. The gateway rejects these encodings. A zero scalar is also rejected at decode time. Valid DER verification, including DER sign padding, keeps its result.
- Malformed signatures now raise `Invalid DER ECDSA signature`. For example, Bun diagnoses the sequence `30 03 02 01 01` as a schema length mismatch. Some formerly decoded inputs, including the replay's context-tagged and indefinite signatures, now produce a warning at decode time. The assertion still returns HTTP 400 with `Invalid passkey assertion`. No HTTP error body, credential write or fetch depends on the decoder diagnostic.
- A decoded structure must occupy the entire input. Bun accepts `04 01 61 00` as the OCTET STRING `61` and ignores a suffix on a certificate or CRL. The gateway rejects suffixes, non-minimal framing and constructed OCTET STRINGs. For example, Bun projects `24 03 04 01 61` to an empty OCTET STRING. Rejecting that encoding prevents OpenSSL's BER flattening from introducing a new match for an AAGUID or challenge.
- OpenSSL validates X.509 structures and time fields. For example, an offset-form validity time is rejected in favor of the RFC 5280 UTC encoding. Duplicate extension OIDs are rejected when the gateway consults them, and key and signature bit strings must be octet aligned. Name strings are projected as UTF-8. The chain check also requires the inner and outer signature algorithms to agree. These checks reject ambiguous certificate inputs while preserving the existing trust and accepted-algorithm rules.
- Serial numbers and RSA public-key components use unsigned magnitudes. The DER integer `02 02 00 80` projects to serial `80` in both the certificate and the CRL. Bun can preserve the sign-padding byte in the CRL serial and miss the match. Negative serial numbers are rejected.
- An Apple nonce must have the explicit `[1]` tag around its OCTET STRING. The previous verifier skipped six bytes and could compare a nonce under a different tag. Android Key rejects the `allApplications` tag on either authorization list regardless of field order. These checks enforce the attestation's existing scope and nonce requirements.
- A CRL fetched successfully with an HTTP 2xx status must decode before the attestation can pass its revocation check. For example, a malformed 200 response used to be treated as an unavailable CRL. This ensures a newly rejected encoding cannot bypass an applicable revocation check. An unavailable CRL still follows the existing fetch failure path.
- An X.509 Name containing an empty relative distinguished name is rejected. Bun retains the empty set in `30 02 31 00`, while OpenSSL omits it from its name entries. The gateway rejects the original encoding before projecting fields or comparing the issuer and subject.
- A non-2xx CRL response with an ASN.1 SEQUENCE header must also decode as a complete list. The gateway historically consulted a CRL body regardless of the response status. Appending a byte to a CRL served with an HTTP 404 now fails the check. A plain 404 `missing` response is still considered unavailable.
- Standard extension payloads must match their OpenSSL DER encoding. For example, a constructed Authority Key Identifier field `a0 03 04 01 61` cannot be used as the cache key `61`. This prevents implicitly tagged BER strings from changing cache or CRL URL selection when OpenSSL flattens them.

## BigInt

- A decimal sign requires digits. Bun 1.4.0 parses `BigInt("+")` and `BigInt("-")` as zero. The gateway reports `SyntaxError` for those inputs so incomplete signed values cannot become numeric zero.
- A binary, octal or hex prefix requires digits even when whitespace follows it. Bun 1.4.0 parses `BigInt("0x ")` as zero. The gateway reports `SyntaxError` and applies the same digit requirement to `'0x'`, `'0o'` and `'0b'` numeric tokens. Stored values and KV keys carry complete digits.

## JSON nesting

The native JSON parser, serializer and recursive helpers accept up to 128 nested
containers. Deeper documents raise `RangeError`, including when projecting a stored
value to JSON. The JSC codec and typed-value interchange retain their independent
2,000-container limit for stored graphs.
