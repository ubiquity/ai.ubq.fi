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

| Changed behavior | Example | Effect |
| ---------------- | ------- | ------ |
| Duplicate keys, sections, invalid keys and incomplete lines fail the whole file | `A=1\nA=2` | A malformed file cannot partially populate the environment. |
| Colon assignments, `export` prefixes and split assignments are rejected | `export A=1` | The accepted grammar is a flat assignment file. |
| Quotes must end on the same line and may be followed by whitespace or a hash comment | `A="x" junk` | Incomplete and ambiguous values fail atomically. |
| C++ quoted extraction consumes the backslash before the next character | `A="a\nb"` gives `anb` | The format has no escape-to-control conversion or expansion. Unquoted backslashes remain literal. |
| ASCII control bytes other than tab and line endings, interior bare CR, and input over 256 KiB are rejected | `A=x\x00y` | Values cannot introduce embedded NUL or line boundaries, and parser allocation has an input bound. |

Sixty-eight of the original 124 inputs now reject the whole file. Nine
retain valid assignments with changed quoted values.

## Filesystem text and POSIX paths

Text file reads and writes use the shared runtime UTF-8 codec. The tested
BOM, malformed maximal subparts, split surrogate pair and raw-byte
expectations held. Path functions use the C++ lexical model and preserve
native POSIX path bytes. Resolve and join retain their existing
interpretation of absolute components.

| Changed lexical vectors | Example | Standard behavior |
| ----------------------: | ------- | ----------------- |
| 2 | `normalize("a/b/c/..")` gives `"a/b/"` | Lexical normalization may leave a final separator after removing a dot or parent element. Resolve still removes that separator. |
| 11 | `dirname("/a/b/")` gives `"/a/b"` and `dirname("/a//b")` gives `"/a"` | A trailing separator denotes an empty final component, and repeated separators are collapsed in the result. |
| 14 | `basename("a/")` and `extname("/a/b.c/")` give an empty string | A trailing separator has no filename or extension. The same rule applies to the suffix overload and root. |
| 2 | `basename("/a/b.txt", "b.txt")` gives an empty string | A matching suffix is removed from the filename even when it spans the entire component. |

Relative paths require resolvable absolute endpoints. A lookup failure
cannot appear to a containment caller as an empty, equal relative path.
