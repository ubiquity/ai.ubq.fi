# Deviations from Bun

## Regular expressions with QuickJS-ng libregexp 0.18.0

The C++ API retains flags and source projection, UTF-16 indices and byte spans, captures,
named groups, `lastIndex`, and the JavaScript match, replace and split algorithms.
QuickJS supplies pattern syntax, matching and Unicode tables. The test overlay records
only rows whose outcomes changed against the original Bun 1.4.0 vectors.

| Class | Example | Effect on the gateway |
| --- | --- | --- |
| Syntax diagnostics | `(` throws `SyntaxError: expecting ')'`. QuickJS can also report a different first error when a pattern contains multiple errors. | Callers still get a syntax error and the engine's diagnostic. Validation does not depend on the wording. |
| Incomplete repetitions and invalid class ranges | QuickJS rejects `a{2,1` and `[\d-z-a]` in legacy mode. Bun accepts these forms. | Stricter input handling. Gateway expressions use complete repetitions and valid ranges. |
| Unicode identity escapes | QuickJS rejects `\é` with `u`, including inside a class. Bun treats the escaped character as a literal. | Stricter Unicode syntax. Gateway patterns can use the literal character or a Unicode escape. |
| Legacy named-reference spelling | QuickJS accepts `(?<a>.)[\k]` and `\k<a\x62>` without `u` as legacy syntax. Bun rejects them. | Fixed validators and generated globs do not use these forms. Stored expressions use the library's syntax. |
| Unicode properties and aliases | QuickJS accepts `\p{IDS_Unary_Operator}`. It rejects `\p{gc=Any}`, `\p{gc=ASCII}` and `\p{gc=Assigned}`. The binary forms `\p{Any}`, `\p{ASCII}` and `\p{Assigned}` remain available. | The installed Unicode data and property parsing come from the vendor. The gateway's use of `\p{L}` and `\p{N}` keeps its recorded results. |
| Large repetition counts | QuickJS saturates counts at its signed 32-bit limit. It accepts `a{99999999999999999999}` and `a{4294967297,4294967296}`, which Bun rejects. | Practical gateway expressions use counts far below that limit. The execution budget also applies to large repetitions. |
| Unicode sets | `[a--b]` with `v` compiles and matches `a`, as in Bun. The former C++ port rejected `v`. | Standalone libregexp adds support for set operations and properties of strings. `u` and `v` are still mutually exclusive. |
| Construction bounds | 254 explicit capture groups compile. A 255th gives `SyntaxError: too many captures`. Patterns above 64 KiB and parser stack exhaustion give `RangeError`. | The cap follows libregexp's capture representation. The adapter limits parser resource use without relying on JavaScriptCore's nesting rules. |
| Exhausted execution | A deep search of `^(?:a\|b)*$` can reach the allocator cap and throw `RangeError`. Bun can return false on memory exhaustion. `^[ab]*$` can also reach the cap in QuickJS. | Exhaustion is an explicit error, so an incomplete scan cannot be mistaken for clean text or ordinary rejection of a subject. |

The parser stack check allows at most 128 KiB below the entry frame and leaves 32 KiB
at the native thread stack base. The allocator refuses individual requests above 72 MiB.
One match operation shares a 2.5-second monotonic deadline at libregexp's interruption
points and a limit of 4,096 timeout hook calls, each after 10,000 library polls. Iterating
string operations also check the deadline every 256 searches. Exhausted compilation and
execution surface as ordinary `rt::RangeError` exceptions.
