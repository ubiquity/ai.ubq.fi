#include "uos/rt/gzip.hpp"

#include "decompression_budget.hpp"

#if defined(UOS_GATEWAY_NATIVE_LIBRARIES)
#define ZLIB_CONST
#include <zlib.h>
#undef deflateInit2
#undef inflateInit2
#endif
#include <dlfcn.h>
#include <openssl/crypto.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <list>
#include <stdexcept>
#include <utility>

namespace uos::rt::gzip {
  namespace {
#if defined(UOS_GATEWAY_NATIVE_LIBRARIES)
    using ZStream = ::z_stream;
#else
    // zlib 1.x's z_stream on an LP64 target (zlib.h is not in the SDK). zlib checks the
    // structure size it is handed, so a different layout fails at init, not in use.
    struct ZStream {
      const unsigned char* next_in;
      unsigned int avail_in;
      unsigned long total_in;
      unsigned char* next_out;
      unsigned int avail_out;
      unsigned long total_out;
      const char* msg;
      void* state;
      void* (*zalloc)(void*, unsigned int, unsigned int);
      void (*zfree)(void*, void*);
      void* opaque;
      int data_type;
      unsigned long adler;
      unsigned long reserved;
    };

    constexpr int Z_OK = 0;
    constexpr int Z_STREAM_END = 1;
    constexpr int Z_NEED_DICT = 2;
    constexpr int Z_STREAM_ERROR = -2;
    constexpr int Z_MEM_ERROR = -4;
    constexpr int Z_BUF_ERROR = -5;
    constexpr int Z_VERSION_ERROR = -6;
    constexpr int Z_NO_FLUSH = 0;
    constexpr int Z_FINISH = 4;
    constexpr int Z_DEFLATED = 8;
    constexpr int Z_DEFAULT_COMPRESSION = -1;
    constexpr int Z_DEFAULT_STRATEGY = 0;
    #endif
    // CompressionStream("gzip"): zlib's default level (6), a 15-bit window, the gzip wrapper.
    constexpr int GZIP_WINDOW_BITS = 15 + 16;
    constexpr int MEM_LEVEL = 8;
#if !defined(UOS_GATEWAY_NATIVE_LIBRARIES)
    // The layout above is zlib 1.x's: zlib compares the major version digit and the size.
    constexpr const char* ZLIB_VERSION = "1.2.13";
#endif
#if defined(__APPLE__)
    constexpr const char* DEFAULT_LIBRARY = "libz.1.dylib";
#else
    constexpr const char* DEFAULT_LIBRARY = "libz.so.1";
#endif
    // zlib counts input and output in 32 bits: hand it at most this much per call.
    constexpr std::size_t MAX_SLICE = std::size_t{1} << 30;
    // The output room the compressor asks for per call.
    constexpr std::size_t OUTPUT_STEP = 64 * 1'024;

    struct Zlib {
      const char* (*zlibVersion)() = nullptr;
      int (*deflateInit2)(ZStream*, int, int, int, int, int, const char*, int) = nullptr;
      int (*deflate)(ZStream*, int) = nullptr;
      int (*deflateEnd)(ZStream*) = nullptr;
      int (*inflateInit2)(ZStream*, int, const char*, int) = nullptr;
      int (*inflate)(ZStream*, int) = nullptr;
      int (*inflateReset)(ZStream*) = nullptr;
      int (*inflateEnd)(ZStream*) = nullptr;
    };

    std::string& libraryName() {
      static std::string name = DEFAULT_LIBRARY;
      return name;
    }
    const Zlib*& loadedTable() {
      static const Zlib* table = nullptr;
      return table;
    }
    // Every table ever loaded stays: an inflater keeps calling the library it started with.
    std::list<Zlib>& tables() {
      static std::list<Zlib> all;
      return all;
    }

    template <class Function> void resolve(void* library, const std::string& name, Function& slot, const char* symbol) {
      void* address = ::dlsym(library, symbol);
      if (!address) throw std::runtime_error("rt::gzip: " + name + " has no " + symbol);
      std::memcpy(&slot, &address, sizeof slot);
    }

    const Zlib& zlib() {
      if (const Zlib* table = loadedTable()) return *table;
      const std::string name = libraryName();
#if defined(UOS_GATEWAY_NATIVE_LIBRARIES)
      // A production native build binds direct symbols. Only the explicit existing
      // test-library override below exercises dynamic-loader failure behavior.
      if (name == DEFAULT_LIBRARY) {
        static const Zlib direct{&::zlibVersion, &::deflateInit2_, &::deflate, &::deflateEnd,
                                 &::inflateInit2_, &::inflate, &::inflateReset, &::inflateEnd};
        loadedTable() = &direct;
        return direct;
      }
#endif
      void* library = ::dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
      if (!library) {
        const char* reason = ::dlerror();
        throw std::runtime_error("rt::gzip: cannot load " + name + " (zlib, behind CompressionStream and DecompressionStream): " + (reason ? reason : "unknown error"));
      }
      Zlib table;
      resolve(library, name, table.zlibVersion, "zlibVersion");
      resolve(library, name, table.deflateInit2, "deflateInit2_");
      resolve(library, name, table.deflate, "deflate");
      resolve(library, name, table.deflateEnd, "deflateEnd");
      resolve(library, name, table.inflateInit2, "inflateInit2_");
      resolve(library, name, table.inflate, "inflate");
      resolve(library, name, table.inflateReset, "inflateReset");
      resolve(library, name, table.inflateEnd, "inflateEnd");
      const char* version = table.zlibVersion();
      if (!version || version[0] != '1') {
        throw std::runtime_error("rt::gzip: " + name + " reports zlib " + (version ? version : "(no version)") + "; the z_stream layout used here is zlib 1.x's");
      }
      tables().push_back(table);
      loadedTable() = &tables().back();
      return tables().back();
    }

    // zlib's own allocations (window, hash chains, pending output) hold the data: wipe them
    // before they are freed. The size lives in a header that keeps malloc's 16-byte alignment.
    constexpr std::size_t HEADER_BYTES = 16;

    void* allocate(void*, unsigned int items, unsigned int size) {
      const std::size_t bytes = static_cast<std::size_t>(items) * size;
      void* block = std::malloc(bytes + HEADER_BYTES);
      if (!block) return nullptr;
      std::memcpy(block, &bytes, sizeof bytes);
      return static_cast<unsigned char*>(block) + HEADER_BYTES;
    }

    void release(void*, void* address) {
      if (!address) return;
      unsigned char* block = static_cast<unsigned char*>(address) - HEADER_BYTES;
      std::size_t bytes = 0;
      std::memcpy(&bytes, block, sizeof bytes);
      OPENSSL_cleanse(address, bytes);
      std::free(block);
    }

    void wipe(Bytes& bytes) {
      if (!bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size());
    }

    std::string initFailure(const char* function, int status) {
      if (status == Z_VERSION_ERROR) return std::string("rt::gzip: ") + function + " refused the zlib 1.x z_stream layout (Z_VERSION_ERROR)";
      if (status == Z_MEM_ERROR) return std::string("rt::gzip: ") + function + " is out of memory";
      return std::string("rt::gzip: ") + function + " failed (" + std::to_string(status) + ")";
    }

    constexpr std::size_t UNBOUNDED = std::numeric_limits<std::size_t>::max();
    constexpr std::string_view OUTPUT_LIMIT_ERROR = "gzip output exceeds its size limit";

    // Own input and output across coroutine suspension, including abandoned tasks.
    struct ScrubbedBuffer {
      explicit ScrubbedBuffer(Bytes input = {}) noexcept : bytes(std::move(input)) {}
      ScrubbedBuffer(ScrubbedBuffer&&) noexcept = default;
      ~ScrubbedBuffer() { wipe(bytes); }

      std::uint8_t* extend(std::size_t extra, std::size_t limit = UNBOUNDED, std::string_view limitMessage = OUTPUT_LIMIT_ERROR) {
        const std::size_t used = bytes.size();
        if (used > limit || extra > limit - used) throw JsError(std::string(limitMessage));
        const std::size_t requested = used + extra;
        if (const auto capacity = bytes.capacity(); requested > capacity) {
          Bytes larger;
          larger.reserve(std::max(requested, capacity + std::min(capacity, limit - capacity)));
          larger.assign(bytes.begin(), bytes.end());
          wipe(bytes);
          bytes.swap(larger);
        }
        bytes.resize(requested);
        return bytes.data() + used;
      }

      Bytes bytes;
    };
  }

  // ---- Inflater ----------------------------------------------------------------------------

  struct Inflater::State {
    const Zlib* z = nullptr;
    InflateOptions options;
    // zlib's state points back at this structure: State never moves.
    ZStream stream{};
    bool initialized = false;
    Bytes buffer;
    // The current chunk's piece size: max(pieceBytes, chunk size), within the buffer.
    std::size_t capacity = 0;
    // Bytes of the piece being filled, or of the piece read() handed out last.
    std::size_t filled = 0;
    bool handedOut = false;
    // Written input zlib has not taken yet.
    std::span<const std::uint8_t> pending;
    detail::DecompressionBudget budget{limits::kStoredGzipBytes};
    // The stream (or member) ended and nothing began since.
    bool atEnd = false;
    // Past the end under Trailing::ignore / magicMembers: input is dropped.
    bool discarding = false;
    std::exception_ptr failure;

    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    ~State() {
      if (initialized) z->inflateEnd(&stream);
      wipe(buffer);
    }

    [[noreturn]] void fail(InflateFailure reason, bool exceededLimit = false) {
      wipe(buffer);
      filled = 0;
      handedOut = false;
      pending = {};
      InflateError error(reason);
      if (exceededLimit) error.code = detail::decompressionLimitCode;
      failure = std::make_exception_ptr(std::move(error));
      std::rethrow_exception(failure);
    }

    void beginMember() {
      if (z->inflateReset(&stream) != Z_OK) fail(InflateFailure::corrupt);
      atEnd = false;
    }

    // Input after the end of the stream.
    void trailing() {
      switch (options.trailing) {
        case Trailing::members: beginMember(); return;
        case Trailing::magicMembers:
          if (pending.front() == 0x1f) {
            beginMember();
            return;
          }
          break;
        case Trailing::ignore: break;
        case Trailing::error: fail(InflateFailure::trailingJunk);
      }
      discarding = true;
      pending = {};
    }

    std::optional<std::span<const std::uint8_t>> read() {
      if (failure) std::rethrow_exception(failure);
      if (handedOut) {
        filled = 0;
        handedOut = false;
      }
      for (;;) {
        if (filled == capacity || (pending.empty() && filled > 0)) {
          handedOut = true;
          return std::span<const std::uint8_t>(buffer.data(), filled);
        }
        if (pending.empty()) return std::nullopt;
        if (atEnd) {
          trailing();
          continue;
        }
        const std::size_t slice = std::min(pending.size(), MAX_SLICE);
        stream.next_in = pending.data();
        stream.avail_in = static_cast<unsigned int>(slice);
        stream.next_out = buffer.data() + filled;
        stream.avail_out = static_cast<unsigned int>(std::min(capacity - filled, budget.remainingOutput() + 1));
        const unsigned int room = stream.avail_out;
        const int status = z->inflate(&stream, Z_NO_FLUSH);
        const std::size_t consumed = slice - stream.avail_in;
        const std::size_t produced = room - stream.avail_out;
        pending = pending.subspan(consumed);
        filled += produced;
        stream.next_in = nullptr;
        stream.avail_in = 0;
        if (!budget.advance(consumed, produced)) fail(InflateFailure::corrupt, true);
        if (status == Z_STREAM_END) {
          atEnd = true;
        } else if (status == Z_NEED_DICT) {
          fail(InflateFailure::dictionary);
        } else if (status != Z_OK && !(status == Z_BUF_ERROR && (consumed > 0 || produced > 0))) {
          fail(InflateFailure::corrupt);
        }
      }
    }
  };

  InflateOptions decompressionStreamOptions(Format format) {
    InflateOptions options;
    switch (format) {
      case Format::gzip: break;
      case Format::deflate:
        options.windowBits = 15;
        options.trailing = Trailing::error;
        break;
      case Format::deflateRaw:
        options.windowBits = -15;
        options.trailing = Trailing::error;
        break;
    }
    return options;
  }

  Inflater::Inflater(Format format) : Inflater(decompressionStreamOptions(format)) {}

  Inflater::Inflater(const InflateOptions& options) : state(std::make_unique<State>()) {
    if (options.pieceBytes == 0 || options.pieceBytes > MAX_SLICE) {
      throw std::invalid_argument("rt::gzip::Inflater: pieceBytes " + std::to_string(options.pieceBytes) + " is not in 1 .. 2^30");
    }
    State& s = *state;
    s.z = &zlib();
    s.options = options;
    s.stream.zalloc = allocate;
    s.stream.zfree = release;
    const int status = s.z->inflateInit2(&s.stream, options.windowBits, ZLIB_VERSION, static_cast<int>(sizeof(ZStream)));
    if (status == Z_STREAM_ERROR) throw std::invalid_argument("rt::gzip::Inflater: zlib refuses windowBits " + std::to_string(options.windowBits));
    if (status != Z_OK) throw std::runtime_error(initFailure("inflateInit2", status));
    s.initialized = true;
    s.capacity = options.pieceBytes;
  }

  Inflater::Inflater(Inflater&& other) noexcept = default;
  Inflater& Inflater::operator=(Inflater&& other) noexcept = default;
  Inflater::~Inflater() = default;

  void Inflater::write(std::span<const std::uint8_t> chunk) {
    State& s = *state;
    if (s.failure) std::rethrow_exception(s.failure);
    if (!s.pending.empty()) throw std::logic_error("rt::gzip::Inflater::write: the previous chunk is not used up (read() until it returns std::nullopt)");
    if (!s.budget.feed(chunk.size())) s.fail(InflateFailure::corrupt, true);
    if (s.discarding) return;
    if (s.handedOut) {
      s.filled = 0;
      s.handedOut = false;
    }
    s.capacity = std::min(std::max(s.options.pieceBytes, chunk.size()), limits::kStoredGzipBytes);
    if (s.buffer.size() < s.capacity) {
      wipe(s.buffer);
      s.buffer = Bytes(s.capacity);
    }
    s.pending = chunk;
  }

  std::optional<std::span<const std::uint8_t>> Inflater::read() { return state->read(); }

  void Inflater::end() {
    State& s = *state;
    if (s.failure) std::rethrow_exception(s.failure);
    if (!s.pending.empty()) throw std::logic_error("rt::gzip::Inflater::end: the last chunk is not used up (read() until it returns std::nullopt)");
    if (!s.atEnd && !s.discarding) s.fail(InflateFailure::truncated);
  }

  bool Inflater::complete() const {
    const State& s = *state;
    return !s.failure && s.pending.empty() && (s.atEnd || s.discarding);
  }

  Bytes gzipSync(std::span<const std::uint8_t> input) {
    if (input.size() > limits::kStoredGzipBytes) throw JsError("gzip input exceeds its size limit");
    const Zlib& z = zlib();
    ZStream stream{};
    stream.zalloc = allocate;
    stream.zfree = release;
    const int initialized = z.deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, GZIP_WINDOW_BITS, MEM_LEVEL, Z_DEFAULT_STRATEGY, ZLIB_VERSION,
                                           static_cast<int>(sizeof(ZStream)));
    if (initialized != Z_OK) throw std::runtime_error(initFailure("deflateInit2", initialized));
    const auto finish = scopeExit([&] { z.deflateEnd(&stream); });
    ScrubbedBuffer output;
    for (int status = Z_OK; status != Z_STREAM_END;) {
      if (stream.avail_in == 0 && !input.empty()) {
        const std::size_t slice = std::min(input.size(), MAX_SLICE);
        stream.next_in = input.data();
        stream.avail_in = static_cast<unsigned int>(slice);
        input = input.subspan(slice);
      }
      stream.next_out = output.extend(OUTPUT_STEP);
      stream.avail_out = OUTPUT_STEP;
      status = z.deflate(&stream, input.empty() ? Z_FINISH : Z_NO_FLUSH);
      if (status != Z_OK && status != Z_STREAM_END) throw std::runtime_error("rt::gzip: deflate failed (" + std::to_string(status) + ")");
      output.bytes.resize(output.bytes.size() - stream.avail_out);
    }
    return std::move(output.bytes);
  }

  void forEachDecompressedPiece(std::span<const std::uint8_t> input, Format format, const std::function<void(std::span<const std::uint8_t>)>& onPiece) {
    Inflater inflater(format);
    while (!input.empty()) {
      const std::size_t slice = std::min(input.size(), OUTPUT_STEP);
      inflater.write(input.first(slice));
      while (const auto piece = inflater.read()) onPiece(*piece);
      input = input.subspan(slice);
    }
    inflater.end();
  }

  namespace {
    Bytes inflateBuffer(std::span<const std::uint8_t> input, std::size_t limit = UNBOUNDED, std::string_view limitMessage = OUTPUT_LIMIT_ERROR) {
      ScrubbedBuffer output;
      forEachDecompressedPiece(input, Format::gzip, [&](std::span<const std::uint8_t> piece) {
        std::copy(piece.begin(), piece.end(), output.extend(piece.size(), limit, limitMessage));
      });
      return std::move(output.bytes);
    }

    template <class Codec>
    Task<Bytes> withTurn(ScrubbedBuffer input, Codec codec, bool deferFailure = false) {
      ScrubbedBuffer output;
      std::exception_ptr failure;
      try {
        output.bytes = codec(input.bytes);
      } catch (...) {
        if (!deferFailure) throw;
        failure = std::current_exception();
      }
      co_await microtask();
      if (failure) std::rethrow_exception(failure);
      co_return std::move(output.bytes);
    }
  }

  Bytes gunzipSync(std::span<const std::uint8_t> input) { return inflateBuffer(input); }
  Task<Bytes> gzip(Bytes input) { return withTurn(ScrubbedBuffer(std::move(input)), gzipSync); }
  Task<Bytes> gunzip(Bytes input) { return withTurn(ScrubbedBuffer(std::move(input)), gunzipSync); }

  Task<Bytes> gunzip(Bytes input, std::size_t limit, std::string_view limitMessage) {
    ScrubbedBuffer owned(std::move(input));
    auto decode = [limit, message = std::string(limitMessage)](std::span<const std::uint8_t> bytes) { return inflateBuffer(bytes, limit, message); };
    return withTurn(std::move(owned), std::move(decode), /*deferFailure=*/true);
  }

  void setZlibLibraryForTest(std::string library) {
    libraryName() = library.empty() ? std::string(DEFAULT_LIBRARY) : std::move(library);
    loadedTable() = nullptr;
  }
}
