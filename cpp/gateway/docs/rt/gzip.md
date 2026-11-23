# uos::rt gzip: `CompressionStream` / `DecompressionStream` over zlib

`#include "uos/rt/gzip.hpp"` (`src/rt/gzip.cpp`), namespace `uos::rt::gzip`. Oracle: Bun 1.4.0
on Linux arm64. Every expected value in `tests/rt/gzip_test.cpp` was recorded from Bun by scratch
scripts (CompressionStream, DecompressionStream, node:zlib, and fetch against a loopback server).
The codec is the system zlib, `libz.so.1` (the SDK ships zlib **1.2.13**, without `zlib.h`).

## Inventory (the mandatory surface)

| Construct                                                                                                            | Where                                                                                      |
| -------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------ |
| `new Blob([body]).stream().pipeThrough(new CompressionStream("gzip"))`, read by `new Response(stream).arrayBuffer()` | `src/catalog/store.ts` `gzip`, `src/sentinel/replay-envelope.ts` `gzip`                    |
| `new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip"))`, read by `new Response(stream).text()`     | `src/catalog/store.ts` `gunzip`                                                            |
| the same stream read by a `getReader()` loop that stops past a size limit                                            | `src/sentinel/replay-envelope.ts` `gunzip`                                                 |
| fetch's automatic `Content-Encoding` decoding (callers then delete `Content-Encoding`)                               | not TS code: the rt fetch transport (`docs/rt/fetch-transport.md`) may build on `Inflater` |

Nothing else in `src/`, `serve.ts` or `lib/` compresses (no node:zlib, `Bun.gzipSync`, brotli
or zstd). Not provided: `CompressionStream("deflate" | "deflate-raw")`, streaming compression,
`pipeThrough` transforms on `rt::ReadableStream`, node:zlib's API.

## TypeScript -> C++

| TypeScript                                                                                                | C++                                                                                                      |
| --------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- |
| `await new Response(new Blob([bytes]).stream().pipeThrough(new CompressionStream("gzip"))).arrayBuffer()` | `co_await rt::gzip::gzip(bytes)` (`Task<Bytes>`), or `rt::gzip::gzipSync(bytes)`                         |
| the same with `new DecompressionStream("gzip")`                                                           | `co_await rt::gzip::gunzip(bytes)`, or `rt::gzip::gunzipSync(bytes)`                                     |
| `... .text()` instead of `.arrayBuffer()`                                                                 | `rt::decodeUtf8(inflated)` (BOM stripped, U+FFFD for ill-formed bytes)                                   |
| a reader loop over `new Blob([bytes]).stream().pipeThrough(new DecompressionStream(format))`              | `rt::gzip::forEachDecompressedPiece(bytes, Format::gzip, onPiece)`; throw from `onPiece` to stop         |
| a `DecompressionStream` fed chunk by chunk                                                                | `rt::gzip::Inflater inflater(Format::gzip);` then `write(chunk)`, `read()` until `std::nullopt`, `end()` |
| `new DecompressionStream("deflate")` / `("deflate-raw")`                                                  | `Format::deflate` / `Format::deflateRaw`                                                                 |
| `catch (e)` on a rejected read                                                                            | `catch (const rt::gzip::InflateError& e)` (a `rt::TypeError`; `e.failure` says which)                    |

`Inflater` takes `InflateOptions{.windowBits, .trailing, .pieceBytes}` for other uses:
`windowBits` is zlib's (8..15 zlib wrapper, 0 the window the zlib header declares, -8..-15 raw
deflate, +16 gzip, +32 gzip or zlib by the header); `trailing` says what input after the end of
the stream does (`members`, `error`, `ignore`, `magicMembers`, below). `write` reads the chunk in
place: keep it alive until `read()` returns `std::nullopt`. A piece span is valid until the next
call. The replay port keeps its exported `gzip`/`gunzip` and calls these; the catalog store's
private `gzip`/`gunzip` wrap `rt::gzip::gzip`/`gunzip` with the UTF-8 conversions.

## Bun's DecompressionStream, as matched

Formats: `"gzip"` is zlib `windowBits` 31 and another member follows a member (`members`);
`"deflate"` is 15 and `"deflate-raw"` -15, where any byte after the end of the stream fails
(`error`). Every failure is a `TypeError` (`InflateFailure` in brackets):

| Input                                                                                                                                                                                                                                       | Bun's rejection                                                                                                          |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------ |
| empty input, or input ending inside a header, block, trailer or a begun member (e.g. one `0x00` or `1f 8b` after a member)                                                                                                                  | `unexpected end of file` (truncated)                                                                                     |
| bad magic, method, reserved flag bits, header CRC, block type, stored lengths, distance, CRC-32, ISIZE, Adler-32, zlib header check; the wrong format (zlib or raw given to "gzip", ...); two or more bytes of non-gzip data after a member | `inflate failed` (corrupt)                                                                                               |
| a zlib header with FDICT                                                                                                                                                                                                                    | `Missing dictionary` (dictionary)                                                                                        |
| any byte after a "deflate" / "deflate-raw" stream, in the same chunk or a later one                                                                                                                                                         | `Trailing junk found after the end of the compressed stream`, `code` `ERR_TRAILING_JUNK_AFTER_STREAM_END` (trailingJunk) |

No other rejection carries a `code`. Pieces: for each input chunk the reader gets full pieces of
max(64 KiB, chunk size) bytes while the chunk lasts, then whatever the chunk produced beyond the
last full piece; output continues across gzip members inside a chunk (two small members in one
chunk are one piece). A failure discards the output produced since the last full piece (one small
member followed by garbage in the same chunk rejects with nothing read; the garbage in the next
chunk rejects after the member's piece). `new Blob([bytes]).stream()` hands over 16 KiB, then 2 MiB
chunks; `forEachDecompressedPiece` and `gunzipSync` feed exactly those. The test pins all of this:
53 one-Blob streams, 15 chunked ones (byte by byte, member and stream boundaries between chunks,
100-byte chunks of 1 MiB of zeros), and the pieces of this component's own output under four
chunkings.

## Compressed bytes are valid gzip, not Bun's bytes

`gzipSync` writes one member with Bun's header (`1f 8b 08 00 00000000 00 03`) through zlib
1.2.13 at its default level 6 (15-bit window, memLevel 8, default strategy). Bun inflates every
recorded output back to its input (SHA-256 checked, also through node:zlib), but its own
deflate picks other blocks, so the bytes and sizes match only for some inputs:

| Input (bytes)                             | Bun's CompressionStream | `gzipSync`           |
| ----------------------------------------- | ----------------------- | -------------------- |
| empty (0), "hello" (5), zeros (1,048,577) | 20, 25, 1,051           | identical bytes      |
| lorem (4,500)                             | 112                     | 91                   |
| text (90,000)                             | 624                     | 342                  |
| catalog-like JSON (155,627)               | 15,063                  | 15,336               |
| random (300,000)                          | 300,113                 | 300,113, other bytes |
| bytes 0..255 cycled (76,800)              | 954                     | 641                  |
| 16-symbol noise (400,000)                 | 216,679                 | 228,199              |

What this affects: (1) the catalog store's KV rows: the chunk bytes, `compressed_bytes` /
`source_compressed_bytes` and, for bodies near a 55,000-byte multiple, `chunk_count` /
`source_chunk_count` differ from what the TS gateway writes for the same body; loading only
compares them with the metadata stored alongside, so either writer's rows load in the other
(tests/catalog_store_differential_test.cpp masks the sizes and inflates 18 Bun-written streams);
(2) the sentinel replay envelope: the compressed plaintext inside the AES-GCM ciphertext, hence the
ciphertext size and stored chunk count (the IV, so the ciphertext digest, is random per capture
anyway); decryption inflates either. replay-store's "compressed payload is too large" check
(compressed + 16 > plaintext limit + 1 MiB + 16) cannot trip for either writer: both expand
incompressible input by about 5 bytes per 16 KiB. Nothing in the TS hashes compressed bytes. A different `libz.so.1`
may write different bytes again: `tests/rt/gzip_test.cpp` pins the SHA-256 of the bytes Bun
verified and fails until they are re-recorded.

## Turns

Bun settles these helpers entirely in microtasks (no macrotask), after 13 turns for the
`arrayBuffer()` forms, 8 when a corrupt stream rejects `text()`, 11 for a reader loop over one
piece and 6 when it rejects (small inputs; more pieces take more turns). The rt forms keep the
shape the two C++ callers had before this component: the codec runs synchronously when the task
is awaited, `gzip` and `gunzip` resume the caller one microtask turn later, and `gunzip` throws a
corrupt stream at once. `forEachDecompressedPiece` is synchronous; the replay port takes its one
turn itself. So the C++ is not turn-exact, exactly as the two ports were not before.

## Loading zlib, and wiping

`libz.so.1` is opened with `dlopen` on the first call that needs it (the loader's library path
decides which file: the tests' wrapper puts the SDK's first). A missing library throws
`std::runtime_error("rt::gzip: cannot load libz.so.1 (zlib, behind CompressionStream and
DecompressionStream): <dlerror>")` from that call (and from every later one until it loads); a
missing symbol or a non-1.x `zlibVersion()` names the library too. `setZlibLibraryForTest(name)`
makes later loads use another name ("" restores `libz.so.1`); the test uses it for the missing
library case. zlib's `z_stream` is declared for zlib 1.x on LP64; zlib's own size check rejects a
different layout at init.

Because the sentinel compresses plaintext it zeroes, every buffer that held data is wiped with
`OPENSSL_cleanse` before it is freed: the inflater's piece buffer, every outgrown output buffer,
zlib's window and state (a size-tracking allocator handed to zlib), and the copy of the input the
async forms own. Spans handed out by `read()` / `onPiece` point into the piece buffer.

## Bun's fetch decoding (for the transport)

Measured with fetch against a loopback server (`tests/rt/gzip_test.cpp` replays 33 bodies through
`Inflater` with the policies below):

- An empty body is not decoded (an empty result, whatever the encoding).
- `gzip` and `x-gzip`: `windowBits` 31 with `Trailing::magicMembers`: after a member, another
  member starts only if the next byte is `0x1f` (a lone `1f`, `1f 8b`, a header alone or a
  truncated member then fail); any other byte ends the body and the rest is ignored (`00`,
  `xyz`, even `xyz` followed by a member).
- `deflate`: a body whose first byte is `0x78` is inflated as zlib (window from its header) and,
  when that fails, again from the start as raw deflate; any other body is raw deflate. A zlib
  stream with a smaller window (first byte `0x18` .. `0x68`) therefore fails. Data after the end of
  the stream is ignored (`Trailing::ignore`). A body that is valid both ways decodes as zlib.
- Failures reject with a `TypeError` whose `code` is `"ZlibError"` and whose message is
  ``ZlibError fetching "<url>". For more information, pass `verbose: true` in the second argument to fetch()``;
  Bun also prints `Decompression error: ZlibError` to stderr.
- Not measured: members or streams split across network reads, and the size of decoded fetch
  body chunks.
