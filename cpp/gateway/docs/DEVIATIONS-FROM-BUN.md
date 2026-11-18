# C++ gateway runtime library behavior

## Date parsing

Date parsing accepts ISO calendar dates, ISO timestamps with optional fractional
seconds and numeric UTC offsets, and IMF-fixdate with a matching weekday.
Unsigned four-digit years and signed six-digit years retain the existing UTC
representation and TimeClip range. Standardized date-only year/month forms and
the existing UTC default for an omitted offset remain supported.

| Changed parser rows | Change                                                                                      |
| ------------------: | ------------------------------------------------------------------------------------------- |
|                 352 | Legacy numeric, named-month and time-zone forms are rejected. Examples include `1/2/2020`, `Jan 1` and `2020-01-01 10:00 PST`. |
|                 151 | Legacy weekday formats are rejected, including `Wednesday, 21-Oct-15 07:28:00 GMT`. Gateway HTTP dates use IMF-fixdate. |
|                  20 | ISO widths, field ranges and calendar dates are checked. For example, `2021-02-29` no longer rolls into March. |
|                   9 | IMF-fixdate fields must be valid and canonical. For example, `Fri, 25 Feb 1989 09:17:12 GMT` has a mismatched weekday. |
|                 175 | Leading and trailing whitespace, controls and non-ASCII bytes are rejected. For example, `2020-01-01\0garbage` is rejected as a whole input. |
|                  72 | Parenthetical legacy tokens are rejected. For example, `2020-01-01 (comment)` is invalid. |
|                   4 | Fractional seconds use the first three digits, right-padded for shorter fractions. `.0001234567891` yields 0 ms, where Bun recorded 123 ms. |
|                   1 | IMF-fixdate years are literal Gregorian years. `Sat, 01 Jan 0000 00:00:00 GMT` denotes year 0000, where Bun recorded 2000. |

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
