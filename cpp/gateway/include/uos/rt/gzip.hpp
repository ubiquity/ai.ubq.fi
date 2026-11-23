#pragma once
// gzip and deflate over the system zlib: `CompressionStream("gzip")` and
// `DecompressionStream(format)` as Bun 1.4.0 behaves (src/catalog/store.ts and
// src/sentinel/replay-envelope.ts pipe Blob bytes through them), plus an incremental
// inflater with zlib's windowBits and trailing-data policies for fetch's response decoding.
// docs/rt/gzip.md has the TS -> C++ table, Bun's error table and the differences from Bun.
//
// The SDK ships libz.so.1 (zlib 1.2.13) but no zlib.h, so the library is loaded with dlopen
// on first use (a missing library throws std::runtime_error naming it then) and zlib 1.x's
// z_stream is declared in the implementation. Compressed bytes are valid gzip that Bun
// inflates, not byte-identical to Bun's. Every buffer that held data is wiped before it is
// freed (the piece buffer, outgrown output, zlib's window and state, the async forms' input
// copy), because the sentinel compresses plaintext it zeroes.
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uos::rt::gzip {
  using Bytes = std::vector<std::uint8_t>;

  // Why inflating failed. Each is a TypeError with Bun's DecompressionStream message.
  enum class InflateFailure {
    truncated,     // "unexpected end of file": the input ended inside the stream (also empty input)
    corrupt,       // "inflate failed": zlib rejected a header, block, distance, CRC-32, ISIZE or Adler-32
    dictionary,    // "Missing dictionary": a zlib header that asks for a preset dictionary
    trailingJunk,  // "Trailing junk found after the end of the compressed stream", code ERR_TRAILING_JUNK_AFTER_STREAM_END
  };

  // What a DecompressionStream reader's read() rejects with.
  struct InflateError : TypeError {
    InflateFailure failure;

    explicit InflateError(InflateFailure failure_) : TypeError(messageOf(failure_)), failure(failure_) {
      if (failure_ == InflateFailure::trailingJunk) code = "ERR_TRAILING_JUNK_AFTER_STREAM_END";
    }

    static std::string messageOf(InflateFailure failure_) {
      switch (failure_) {
        case InflateFailure::truncated: return "unexpected end of file";
        case InflateFailure::corrupt: return "inflate failed";
        case InflateFailure::dictionary: return "Missing dictionary";
        case InflateFailure::trailingJunk: return "Trailing junk found after the end of the compressed stream";
      }
      return "inflate failed";
    }
  };

  // DecompressionStream's formats.
  enum class Format {
    gzip,        // "gzip": RFC 1952 members
    deflate,     // "deflate": the zlib wrapper (RFC 1950)
    deflateRaw,  // "deflate-raw": a bare RFC 1951 stream
  };

  // What the inflater does with input that follows the end of the compressed stream.
  enum class Trailing {
    members,       // it begins another gzip member (DecompressionStream("gzip"))
    error,         // InflateFailure::trailingJunk (DecompressionStream("deflate"), ("deflate-raw"))
    ignore,        // it is discarded, and so is every later chunk (Bun's fetch decoding of `deflate`)
    magicMembers,  // another member when its first byte is 0x1f, otherwise ignore (Bun's fetch decoding of `gzip`)
  };

  struct InflateOptions {
    // zlib's windowBits: 8..15 for the zlib wrapper (0: the window its header declares),
    // -8..-15 for raw deflate, 8..15 + 16 for gzip, 8..15 + 32 for gzip or zlib as the header says.
    int windowBits = 15 + 16;
    Trailing trailing = Trailing::members;
    // The smallest piece capacity (1 .. 2^30). A chunk's pieces hold up to max(pieceBytes, chunk
    // size) bytes: Bun's DecompressionStream uses max(64 KiB, chunk size).
    std::size_t pieceBytes = 64 * 1'024;
  };

  // `new DecompressionStream(format)`: gzip {31, members}, deflate {15, error}, deflate-raw {-15, error}.
  InflateOptions decompressionStreamOptions(Format format);

  // An incremental inflater with the piece behaviour of Bun's DecompressionStream. For each
  // written chunk read() yields full pieces of max(pieceBytes, chunk size) bytes while the chunk
  // lasts and then what is left when the chunk is used up, so a reader sees exactly the pieces
  // Bun's reader sees (output continues across gzip members inside one chunk). A failure
  // discards the output inflated since the last piece, as Bun's stream does, and every later
  // call rethrows it.
  //
  //   rt::gzip::Inflater inflater(rt::gzip::Format::gzip);
  //   inflater.write(chunk);                                      // for each input chunk
  //   while (auto piece = inflater.read()) consume(*piece);       // may throw InflateError
  //   inflater.end();                                             // after the last chunk
  //
  // Throws std::runtime_error when zlib cannot be loaded, std::invalid_argument for options zlib
  // refuses. Movable (a moved-from inflater may only be destroyed or assigned), not copyable.
  class Inflater {
  public:
    explicit Inflater(Format format = Format::gzip);
    explicit Inflater(const InflateOptions& options);
    Inflater(Inflater&& other) noexcept;
    Inflater& operator=(Inflater&& other) noexcept;
    Inflater(const Inflater&) = delete;
    Inflater& operator=(const Inflater&) = delete;
    ~Inflater();

    // Hands over the next input chunk. It is read in place: keep it alive and unchanged until
    // read() returns std::nullopt. std::logic_error while the previous chunk is not used up.
    void write(std::span<const std::uint8_t> chunk);
    // The next output piece of the written input, or std::nullopt once the chunk is used up and
    // its output delivered. The span stays valid until the next call on this inflater.
    std::optional<std::span<const std::uint8_t>> read();
    // The input is over: InflateError(truncated) unless the stream (every member begun) is complete.
    void end();
    // Whether end() would succeed now.
    bool complete() const;

  private:
    struct State;
    std::unique_ptr<State> state;
  };

  // Whole-buffer gzip with zlib defaults and concatenated-member validation.
  Bytes gzipSync(std::span<const std::uint8_t> input);
  Bytes gunzipSync(std::span<const std::uint8_t> input);

  // Borrowed pieces of up to 64 KiB, valid during the callback. The callback may throw to stop.
  void forEachDecompressedPiece(std::span<const std::uint8_t> input, Format format, const std::function<void(std::span<const std::uint8_t>)>& onPiece);

  // Owned input is scrubbed. Success takes one microtask turn, and uncapped failures are immediate.
  [[nodiscard]] Task<Bytes> gzip(Bytes input);
  [[nodiscard]] Task<Bytes> gunzip(Bytes input);
  // Capped decoding defers either outcome by one turn and throws JsError on limit overflow.
  [[nodiscard]] Task<Bytes> gunzip(Bytes input, std::size_t limit, std::string_view limitMessage);

  // Test seam: later loads use `library` instead of "libz.so.1" ("" restores it). Inflaters
  // already made keep the library they loaded.
  void setZlibLibraryForTest(std::string library);
}
