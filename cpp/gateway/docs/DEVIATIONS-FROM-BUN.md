# Runtime deviations from Bun

The gateway keeps the application and protocol behavior described in [PORTING.md](../PORTING.md). This record covers deliberate generic runtime differences and the evidence that bounds them.

## JSON

The Linux ARM64 simdjson 5.2.0 and Boost.JSON 1.92 checkpoint passed 8/8 focused binaries and 71/71 cases, with 643 parser vectors retained. The package manifest is `c196ee22541cd11fcf67589a47ba7dc686c446e92bae0043b84e85da48a1cf4a`. [Behavior handback](/tmp/claude-1002/prod/assignment3/runtime/b1-20261117T230423Z/deviations.md) [Focused results](/tmp/claude-1002/prod/assignment3/runtime/b1-20261117T230423Z/focused-results.json)

- Raw parse failures use one `SyntaxError("Invalid JSON")`. For example, malformed DeepSeek SSE still produces a `malformed_event` with a `SyntaxError` cause. The same diagnostic applies to streamed Fetch and the differential fixtures for Claude models and streams, native artifacts, legacy KV, and passkeys. Buffered Fetch keeps its malformed-body classification and cursor validation still rejects invalid encoded cursors. [Suite repair](findings/cppgw-suite-repair-20261118.md)
- Parsed members follow decoded input order. For example `{"b":1,"2":2,"1":3}` serializes in that order. Anthropic replay still sorts its canonical representation for matching and accounting. After restoring it, numeric-looking object keys retain their position in that representation. Explicit key-order helpers remain available for application consumers that require their projection.
- Raw malformed UTF-8, including encoded surrogate code points, is rejected before a JSON value is exposed. A JSON-escaped lone code unit such as `"\uD800"` still survives round trips where existing persisted JSON requires it. Native KV continues to use the separately identified storage codec.
- An overflowing JSON numeric literal such as `1e400` is rejected. Harmony preserves upstream tool argument text when it cannot normalize it, including out-of-range numbers. Finite canonical doubles, subnormals, negative zero and compact number spelling keep their tested results.

The later v40 limit enforcement has its own [runtime table](RUNTIME.md#input-limits), source identity and boundary tests.

## UTF and Encoding

The simdutf 9.4.0 checkpoint kept the tested generic outputs and error classes. For example, a multibyte UTF-8 sequence split across streamed chunks is held until the next chunk by the bounded state adapter. Simdutf handles well-formed scans and transcoding, while the WHATWG adapter handles incomplete sequences, replacement, fatal errors and BOM state. Tested WTF-8/UTF-16 behavior protects callers' code-unit and storage contracts. The four focused Linux ARM64 binaries passed 56/56 cases. [Behavior handback](/tmp/claude-1002/prod/assignment3/runtime/c2-20261117T230537Z/GENERIC-BEHAVIOR.md) [Focused results](/tmp/claude-1002/prod/assignment3/runtime/c2-20261117T230537Z/focused-results.json)

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

## WebSocket

- The receiver always validates UTF-8 in text messages and close reasons. With `skipUTF8Validation: true`, `ws@8.18.3` under Bun 1.4.0 delivers text bytes `c3 28` and accepts close 1000 with reason byte `ff`. The C++ receiver rejects both with close 1007. Beast enforces the RFC 6455 UTF-8 requirement and gateway callers use validation. The option still compiles for existing callers.

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

## Linux confinement

The native C++ launcher accepts an operator opt-in for Landlock ABI 3–7. For example, an ABI 3 Raspberry Pi with `UOS_GATEWAY_ALLOW_REDUCED_CONFINEMENT=1` installs the filesystem rules and the same seccomp filter used on ABI 8. Its bootstrap verifies that future threads will inherit the Landlock restrictions from the sole existing thread. The startup receipt reports the protections that the kernel applied. [Runtime configuration](RUNTIME.md#linux-confinement) [Verification report](findings/cppgw-landlock-fallback-20261118.md)

## Incoming integrations

Add a component section after its verified handback. Record the changed behavior, one concrete input/outcome, the application or security rationale, and the exact source and test receipt. Use this location for future URL/IDNA, HTTP, regex, WebSocket, passkey codec, BigInt, ICU and helper integrations. Record limit-specific exceptions and tested boundary outcomes in [Runtime](RUNTIME.md#input-limits).
