# uos::rt text: strings, codecs, base64, Date

Oracle: Bun 1.4.0 (JavaScriptCore, ICU 78.3, Unicode 17.0). Headers:
`uos/rt/string.hpp`, `uos/rt/text_codec.hpp`, `uos/rt/base64.hpp`, `uos/rt/date.hpp`.

## Encoding convention (binding for every rt component and porter)

1. A JS string is a sequence of UTF-16 code units. In C++ it is a `std::string`
   holding **WTF-8**: every code point is UTF-8 encoded; a _lone_ surrogate
   (U+D800..U+DFFF not part of a valid pair) is encoded as its three-byte
   generalized-UTF-8 form `ED A0..BF 80..BF`. A well-formed JS string is plain UTF-8.
2. Canonical form: a surrogate _pair_ is always one four-byte sequence. The bytes
   `ED A0..AF xx ED B0..BF xx` (high then low, "split pair") are non-canonical.
   Every rt reader accepts a split pair and reads it as the pair (exactly what JS
   sees after `"\uD83D" + "\uDE00"`); every rt writer emits canonical WTF-8. Byte
   equality (`==`, hashing, map keys) is JS equality only on canonical strings, so
   join strings that may end/start with a lone surrogate with `rt::concat`.
3. Bytes that are not WTF-8 (raw network/file input) must be decoded at the boundary
   (`rt::TextDecoder`, `rt::decodeUtf8`). If they reach an rt string function anyway,
   each maximal ill-formed subpart is read as one U+FFFD (the WHATWG decoder rule),
   so results are deterministic, but they are not a JS behaviour: do not rely on it.
4. All indexes, lengths and positions in `uos/rt/string.hpp` are UTF-16 code unit
   counts, as in JS. A slice that cuts a surrogate pair yields a lone surrogate
   (WTF-8 `ED ..`), exactly like JS.
5. `std::string` comparison is UTF-8 byte order = code point order. JS `<`, `>`,
   `sort()` without comparator and `localeCompare` are not; use `rt::compare`.
6. Code that only copies, concatenates ASCII, or compares with `==` needs no helper.
   ASCII-only inputs take fast paths in every helper.
7. Byte arrays (`Uint8Array`, `ArrayBuffer`, `Buffer`) are `std::vector<std::uint8_t>`
   (owned) and `std::span<const std::uint8_t>` (views). A "binary string" (what
   `atob` returns, `String.fromCharCode(...bytes)`) is a JS string of code units
   U+0000..U+00FF, so bytes 0x80..0xFF take two UTF-8 bytes in it.
8. Number arguments (`rt::NumberArg`) take any arithmetic value and apply JS
   ToIntegerOrInfinity (NaN -> 0, truncation, +-Infinity), as the JS methods do.

## TypeScript construct -> C++ call

`rt` is `uos::rt`. Errors are `rt::JsError` subclasses with Bun's name and message.

### Strings (`uos/rt/string.hpp`)

| TypeScript                                                                       | C++                                                                                                                                                              |
| -------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `s.length`                                                                       | `rt::length(s)`                                                                                                                                                  |
| `s.charAt(i)`; `s[i]` with `i` in range                                          | `rt::charAt(s, i)`                                                                                                                                               |
| `s.charCodeAt(i)`                                                                | `rt::charCodeAt(s, i)` (NaN out of range)                                                                                                                        |
| `s.codePointAt(i)`                                                               | `rt::codePointAt(s, i)` (`nullopt` = undefined)                                                                                                                  |
| `s.at(i)`                                                                        | `rt::at(s, i)` (`nullopt` = undefined)                                                                                                                           |
| `s.slice(a)`, `s.slice(a, b)`                                                    | `rt::slice(s, a)`, `rt::slice(s, a, b)`                                                                                                                          |
| `s.substring(a[, b])`, `s.substr(a[, n])`                                        | `rt::substring(...)`, `rt::substr(...)`                                                                                                                          |
| `s.indexOf(t[, p])`, `s.lastIndexOf(t[, p])`                                     | `rt::indexOf(s, t[, p])`, `rt::lastIndexOf(s, t[, p])`                                                                                                           |
| `s.includes(t[, p])`, `s.startsWith(t[, p])`, `s.endsWith(t[, e])`               | `rt::includes`, `rt::startsWith`, `rt::endsWith` (same arguments)                                                                                                |
| `s.padStart(n[, f])`, `s.padEnd(n[, f])`                                         | `rt::padStart(s, n[, f])`, `rt::padEnd(s, n[, f])`                                                                                                               |
| `s.repeat(n)`                                                                    | `rt::repeat(s, n)` (RangeError texts as Bun)                                                                                                                     |
| `s.trim()`, `s.trimStart()`, `s.trimEnd()`                                       | `rt::trim(s)`, `rt::trimStart(s)`, `rt::trimEnd(s)`                                                                                                              |
| test for a JS WhiteSpace/LineTerminator code point                               | `rt::isJsWhitespace(cp)`                                                                                                                                         |
| `s.split()`, `s.split(sep[, limit])` with a string `sep` (incl. `""`)            | `rt::split(s)`, `rt::split(s, sep[, limit])`                                                                                                                     |
| `s.replace(str, rep)`, `s.replaceAll(str, rep)` (string pattern; `$$ $& $\` $'`) | `rt::replace(s, str, rep)`, `rt::replaceAll(s, str, rep)` (regex patterns: `rt::Regex`)                                                                          |
| `s.toLowerCase()`, `s.toUpperCase()`                                             | `rt::toLowerCase(s)`, `rt::toUpperCase(s)`                                                                                                                       |
| `String.fromCharCode(u)`, `(...units)`, `(...bytes)`                             | `rt::fromCharCode(u)`, `rt::fromCharCode(span<const uint16_t>)`, `rt::fromCharCode(span<const uint8_t>)`                                                         |
| `String.fromCodePoint(cp)`, `(...cps)`                                           | `rt::fromCodePoint(cp)`, `rt::fromCodePoint(span<const uint32_t>)` (RangeError)                                                                                  |
| `a < b`, `a > b`, default `sort()` order                                         | `rt::lessThan(a, b)`, `rt::compare(a, b)` (<0, 0, >0)                                                                                                            |
| `a.localeCompare(b)` (no locales/options: every call site)                       | `rt::localeCompare(a, b)` (-1, 0, 1; domain below)                                                                                                               |
| `encodeURIComponent`, `encodeURI`, `decodeURIComponent`, `decodeURI`             | same names (`rt::URIError` "URI error" / "String contained an illegal UTF-16 sequence.")                                                                         |
| `for (const ch of s)`, `[...s]`, `Array.from(s)`                                 | `for (uint32_t cp : rt::codePoints(s))` (iterator `.text()` = the element), `rt::codePointStrings(s)`                                                            |
| `ch.charCodeAt(0)` of a for...of element                                         | `rt::leadCodeUnit(cp)`                                                                                                                                           |
| `s.isWellFormed()`, `s.toWellFormed()`                                           | `rt::isWellFormed(s)`, `rt::toWellFormed(s)`                                                                                                                     |
| `a + b` when `a` may end with a lone high surrogate                              | `rt::concat(a, b)`, `rt::append(out, b)`                                                                                                                         |
| `parts.join(sep)` / `parts.join()` over strings                                  | `rt::join(parts, sep)` / `rt::join(parts)` (`std::span<const std::string>`, a vector converts; appends like `+`, so halves of a pair meeting at a boundary join) |
| UTF-16 code units of a string and back                                           | `rt::toUtf16(s)`, `rt::fromUtf16(units)`, `rt::appendCodePoint(out, cp)`                                                                                         |
| validate / repair storage                                                        | `rt::isAscii`, `rt::isWtf8`, `rt::isCanonicalWtf8`, `rt::canonicalWtf8`                                                                                          |

### Text codecs (`uos/rt/text_codec.hpp`)

| TypeScript                                                                   | C++                                                                                                      |
| ---------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- |
| `new TextEncoder().encode(s)`, `Buffer.from(s)`, `Buffer.from(s, "utf8")`    | `rt::encodeUtf8(s)` or `rt::TextEncoder().encode(s)` (lone surrogate -> EF BF BD)                        |
| `new TextEncoder().encode(s).byteLength` / `.length`                         | `rt::utf8ByteLength(s)`                                                                                  |
| `new TextDecoder().decode(bytes)`                                            | `rt::decodeUtf8(bytes)` (U+FFFD per maximal subpart, leading BOM dropped)                                |
| `new TextDecoder("utf-8", { fatal: true }).decode(bytes)`                    | `rt::decodeUtf8(bytes, {.fatal = true})` (TypeError "The encoded data was not valid for encoding utf-8") |
| `new TextDecoder("utf-8", { ignoreBOM: true })`                              | `rt::TextDecoder("utf-8", {.ignoreBOM = true})`                                                          |
| `decoder.decode(chunk, { stream: true })`, then `decoder.decode()`           | `decoder.decode(chunk, {.stream = true})`, then `decoder.decode()`                                       |
| `buf.toString()`, `buf.toString("utf8")` (keeps a BOM)                       | `rt::decodeUtf8(bytes, {.ignoreBOM = true})` or `rt::bufferToString(bytes)`                              |
| `decoder.encoding`, `decoder.fatal`, `decoder.ignoreBOM`, `encoder.encoding` | `.encoding()` ("utf-8"), `.fatal()`, `.ignoreBOM()`                                                      |
| well-formed UTF-8 check                                                      | `rt::isUtf8(bytes)`                                                                                      |

### Base64, hex and Buffer encodings (`uos/rt/base64.hpp`)

| TypeScript                                                                                     | C++                                                                                                                               |
| ---------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| `btoa(binary)`                                                                                 | `rt::btoa(binary)` (`rt::InvalidCharacterError` above U+00FF)                                                                     |
| `btoa(String.fromCharCode(...bytes))`, chunked `bytesToBase64`, `buf.toString("base64")`       | `rt::toBase64(bytes)`                                                                                                             |
| `atob(s)`                                                                                      | `rt::atob(s)` (binary string; `rt::InvalidCharacterError`)                                                                        |
| `Uint8Array.from(atob(s), (c) => c.charCodeAt(0))`, the `bytes[i] = binary.charCodeAt(i)` loop | `rt::atobBytes(s)`                                                                                                                |
| `String.fromCharCode(...bytes)`, `buf.toString("latin1")`, `buf.toString("binary")`            | `rt::binaryString(bytes)`                                                                                                         |
| `buf.toString("base64url")`                                                                    | `rt::toBase64Url(bytes)`                                                                                                          |
| `buf.toString("hex")`, `for (b of bytes) out += b.toString(16).padStart(2, "0")`               | `rt::toHex(bytes)`                                                                                                                |
| `Buffer.from(s, "base64")`, `Buffer.from(s, "base64url")` (never throw)                        | `rt::fromBase64(s)`                                                                                                               |
| `Buffer.from(s, "hex")` (stops at the first bad pair)                                          | `rt::fromHex(s)`                                                                                                                  |
| `Buffer.from(s, enc)`, `buf.toString(enc)` for any Node encoding name                          | `rt::bufferFrom(s, enc)`, `rt::bufferToString(bytes, enc)` (`enc`: name or `rt::BufferEncoding`; TypeError "Unknown encoding: x") |

### Date (`uos/rt/date.hpp`)

| TypeScript                                                             | C++                                                        |
| ---------------------------------------------------------------------- | ---------------------------------------------------------- |
| `new Date(ms)`                                                         | `rt::Date(ms)` (TimeClip)                                  |
| `new Date()`                                                           | `rt::Date(rt::dateNow())` (`uos/rt/clock.hpp`)             |
| `new Date(string)`                                                     | `rt::Date(std::string_view(s))`                            |
| `new Date(null)`                                                       | `rt::Date(0.0)`                                            |
| `new Date(y, m[, d, h, min, s, ms])` (local fields; TZ=UTC)            | `rt::Date(rt::Date::UTC(y, m, d, h, min, s, ms))`          |
| `Date.parse(s)`                                                        | `rt::Date::parse(s)` (NaN when invalid)                    |
| `Date.UTC(y[, m, d, h, min, s, ms])`                                   | `rt::Date::UTC(y[, m, ...])`                               |
| `d.getTime()`, `d.valueOf()`, `+d`                                     | `d.getTime()`                                              |
| `d.getUTCFullYear()` ... `d.getUTCMilliseconds()`, `d.getUTCDay()`     | same names (NaN when invalid)                              |
| `d.getFullYear()` and the other local getters                          | the `getUTC*` twin (TZ=UTC)                                |
| `d.getTimezoneOffset()`                                                | `d.getTimezoneOffset()` (0)                                |
| `d.setTime(t)`, `d.setUTCFullYear(...)` ... `d.setUTCMilliseconds(ms)` | same names; an omitted optional argument is `std::nullopt` |
| `d.toISOString()`                                                      | `d.toISOString()` (RangeError "Invalid Date")              |
| `d.toJSON()`, a Date inside `JSON.stringify`                           | `d.toJSON()` (`nullopt` = null)                            |
| `d.toUTCString()`                                                      | `d.toUTCString()` ("Invalid Date" when invalid)            |
| `String(d)`, `` `${d}` ``, `d.toString()`                              | `d.toString()`                                             |

## Limits

- `rt::localeCompare` is exact (tertiary strength, NFD, Bun's ICU root order) for
  strings whose code points all lie in the verified repertoire: U+0000..U+052F (Latin,
  IPA, combining marks, Greek, Cyrillic), U+1E00..U+1FFE, U+2000..U+206F,
  U+20A0..U+20BF except U+20A7, U+20D0..U+20F0, U+2100..U+218B except U+2180..U+2188,
  U+2190..U+22FF, U+2500..U+27BF, U+3000..U+30FF except U+3038..U+303A,
  U+FE00..U+FE0F, U+FEFF, U+FF01..U+FFEE, U+1F300..U+1F64F, U+1F680..U+1F6FC,
  U+1F900..U+1F9FF, U+E0001, U+E0020..U+E007F, U+E0100..U+E01EF (minus the code
  points DUCET 13.0 does not list); U+FFFD/FFFE/FFFF follow CLDR. Every other
  character (Han, Hangul, Arabic, Hebrew, Indic, unassigned, private use, lone
  surrogates) gets a deterministic implicit weight after every tabulated character,
  in code point order: not Bun's order. The gateway sorts ASCII identifiers, hosts and
  paths. Locales and options (`numeric`, `sensitivity`) are not provided (unused).
- `toLowerCase`/`toUpperCase` cover the full default case mapping of every code point
  (one-to-many mappings, final sigma), checked exhaustively against Bun; no
  locale-specific mapping (`toLocaleLowerCase` is unused).
- Not provided (no call site): `normalize`, `escape`/`unescape`, `Intl`, `toLocale*`,
  `localeCompare` options, `TextEncoder.encodeInto`, `TextDecoderStream`,
  `toDateString`/`toTimeString`/`toGMTString`, the local-time setters.
- `rt::TextDecoder` implements UTF-8 only: every other label throws RangeError
  `Unsupported encoding label "<label>"`, also the WHATWG labels Bun accepts
  (windows-1252, utf-16le, shift_jis, ...). Fatal streaming follows Bun, not the
  spec: the bytes after the error in that chunk are dropped, and a sequence completed
  from the previous chunk keeps its BOM-seen effect. The fatal TypeError carries no
  `code` (Bun adds `ERR_ENCODING_INVALID_ENCODED_DATA`; no call site reads it).
- `rt::InvalidCharacterError` (atob/btoa) is a `JsError` named "InvalidCharacterError"
  with `code()` 5, not an `rt::DOMException`: that class is defined by the async
  runtime (`abort.cpp`). A TS `catch` that tests `instanceof DOMException` on an atob
  error must catch `rt::InvalidCharacterError` too (no call site does).
- `Buffer` itself is not modelled: only its string/byte conversions. Bun's
  `Buffer.from(s, "base64" | "hex")` reads the low byte of every UTF-16 code unit
  (so U+0141 acts as `A`); `rt::fromBase64`/`rt::fromHex` reproduce that.
- `Date.parse` reproduces Bun's parser (V8's DateParser: the ES date-time format, then
  the legacy token loop: words before the first number ignored, parentheses skipped,
  month names by 3-letter prefix, `am`/`pm`, `UT`/`UTC`/`GMT`/`Z` and US zone names,
  `+hh:mm`/`+hhmm`/`+h` offsets, two-digit years 0..49 -> 20xx and 50..99 -> 19xx,
  missing year 2001, first U+0000 ends the input). Checked on about 250,000 inputs
  (ISO and RFC 7231/850/asctime forms, `toString`/`toLocaleString` output, mutated and
  random strings) with no difference. One rule depends on JSC's storage, not the
  content: U+000B is white space only in a string JSC stores as UTF-16; the port
  treats a string as UTF-16 when it has a code unit above U+00FF.
- Local time is UTC. `ops/ai-ubq-fi.service` and `ops/launch-vps.sh` set no `TZ`, the
  host has no `/etc/localtime` (so glibc and Bun use UTC), and no source file reads a
  local-time field. A date-time string without an offset and every local getter
  therefore use offset 0, and `toString` always prints `GMT+0000 (Coordinated
Universal Time)`. (`.env`, loaded by `ops/launch-service.ts`, was not read.)
- `Date.UTC` and the setters follow the spec's MakeDay/MakeTime exactly; a year
  beyond +-1e13 (not exactly representable, always out of range) gives NaN.
