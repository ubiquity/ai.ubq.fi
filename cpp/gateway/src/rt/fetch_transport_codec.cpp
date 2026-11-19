// Response decompression as Bun's fetch applies it. gzip and deflate inflate
// through rt::gzip (zlib). Native builds bind brotli/zstd through host headers;
// the retained SDK comparison declares and resolves its stable C entry points.
// A coding unavailable in that SDK path is left out of Accept-Encoding.
#include "fetch_transport_internal.hpp"
#include "decompression_budget.hpp"

#include "uos/rt/gzip.hpp"

#if defined(UOS_GATEWAY_NATIVE_LIBRARIES)
#include <brotli/decode.h>
#include <zstd.h>
#include <zstd_errors.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <span>
#include <stdexcept>

namespace uos::rt::detail::fetch {
  namespace {
#if !defined(UOS_GATEWAY_NATIVE_LIBRARIES)
    template <class Function> bool resolveSymbol(void* library, Function& slot, const char* name) {
      void* symbol = ::dlsym(library, name);
      if (!symbol) return false;
      std::memcpy(&slot, &symbol, sizeof slot);
      return true;
    }

#endif

    // Bun reports a failed decompression on stderr besides failing the body.
    [[noreturn]] void decompressionFailed(std::string_view code, const std::string& url) {
      std::fprintf(stderr, "Decompression error: %.*s\n", static_cast<int>(code.size()), code.data());
      std::fflush(stderr);
      throw verboseError(code, url);
    }

    std::span<const std::uint8_t> bytesOf(std::string_view text) { return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()); }

    // ---- zlib (gzip, deflate) ---------------------------------------------------

    bool zlibAvailable() {
      static const bool available = [] {
        try {
          gzip::Inflater probe(gzip::Format::gzip);
          return true;
        } catch (const std::runtime_error&) {
          return false;
        }
      }();
      return available;
    }

    // gzip: members follow one another only when the next byte is 0x1f, other
    // trailing bytes are ignored. deflate: the zlib wrapper when the first
    // chunk has more than one byte and starts with 0x78, raw deflate
    // otherwise; bytes after the end are ignored.
    class ZlibDecoder final : public ContentDecoder {
    public:
      ZlibDecoder(bool gzipCoding, std::string url) : gzip_(gzipCoding), url_(std::move(url)) {}

      void write(std::string_view input, std::string& out) override {
        if (input.empty()) return;
        if (!budget_.feed(input.size())) decompressionFailed("ResponseBodyTooLarge", url_);
        try {
          if (!inflater_) {
            gzip::InflateOptions options;
            if (gzip_) {
              options.windowBits = 15 + 16;
              options.trailing = gzip::Trailing::magicMembers;
            } else {
              options.windowBits = input.size() > 1 && static_cast<unsigned char>(input[0]) == 0x78 ? 0 : -15;
              options.trailing = gzip::Trailing::ignore;
            }
            inflater_.emplace(options);
          }
          inflater_->write(bytesOf(input));
          budget_.advance(input.size(), 0);
          while (const auto piece = inflater_->read()) {
            if (!budget_.advance(0, piece->size())) decompressionFailed("ResponseBodyTooLarge", url_);
            out.append(reinterpret_cast<const char*>(piece->data()), piece->size());
          }
        } catch (const gzip::InflateError& error) {
          decompressionFailed(error.code == decompressionLimitCode ? "ResponseBodyTooLarge" : "ZlibError", url_);
        }
      }

      void finish(std::string&) override {
        if (!inflater_) return;
        try {
          inflater_->end();
        } catch (const gzip::InflateError&) {
          decompressionFailed("ZlibError", url_);
        }
      }

    private:
      bool gzip_;
      std::string url_;
      std::optional<gzip::Inflater> inflater_;
      DecompressionBudget budget_;
    };

    // ---- brotli -----------------------------------------------------------------

#if defined(UOS_GATEWAY_NATIVE_LIBRARIES)
    using BrotliState = ::BrotliDecoderState;
    struct BrotliApi {
      decltype(&::BrotliDecoderCreateInstance) createInstance = &::BrotliDecoderCreateInstance;
      decltype(&::BrotliDecoderDestroyInstance) destroyInstance = &::BrotliDecoderDestroyInstance;
      decltype(&::BrotliDecoderDecompressStream) decompressStream = &::BrotliDecoderDecompressStream;
    };
#else
    // brotli 1.x c/include/brotli/decode.h
    using BrotliState = void;
    struct BrotliApi {
      void* (*createInstance)(void*, void*, void*) = nullptr;
      void (*destroyInstance)(void*) = nullptr;
      int (*decompressStream)(void*, std::size_t*, const std::uint8_t**, std::size_t*, std::uint8_t**, std::size_t*) = nullptr;
    };
#endif
    constexpr int kBrotliError = 0;
    constexpr int kBrotliSuccess = 1;
    constexpr int kBrotliNeedsMoreInput = 2;

    const BrotliApi* brotliApi() {
#if defined(UOS_GATEWAY_NATIVE_LIBRARIES)
      static const BrotliApi api;
      return &api;
#else
      static const std::optional<BrotliApi> api = []() -> std::optional<BrotliApi> {
#if defined(__APPLE__)
        void* library = ::dlopen("libbrotlidec.1.dylib", RTLD_NOW | RTLD_LOCAL);
#else
        void* library = ::dlopen("libbrotlidec.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
        if (!library) return std::nullopt;
        BrotliApi loaded;
        if (!resolveSymbol(library, loaded.createInstance, "BrotliDecoderCreateInstance") || !resolveSymbol(library, loaded.destroyInstance, "BrotliDecoderDestroyInstance") ||
            !resolveSymbol(library, loaded.decompressStream, "BrotliDecoderDecompressStream"))
          return std::nullopt;
        return loaded;
      }();
      return api ? &*api : nullptr;
#endif
    }

    // Bytes after the end of the stream are ignored.
    class BrotliDecoder final : public ContentDecoder {
    public:
      BrotliDecoder(const BrotliApi& api, std::string url) : api_(api), url_(std::move(url)), state_(api.createInstance(nullptr, nullptr, nullptr)) {
        if (!state_) throw std::bad_alloc();
      }
      ~BrotliDecoder() override { api_.destroyInstance(state_); }
      BrotliDecoder(const BrotliDecoder&) = delete;
      BrotliDecoder& operator=(const BrotliDecoder&) = delete;

      void write(std::string_view input, std::string& out) override {
        if (!budget_.feed(input.size())) decompressionFailed("ResponseBodyTooLarge", url_);
        if (input.empty() || done_) return;
        received_ = true;
        const auto* next = reinterpret_cast<const std::uint8_t*>(input.data());
        std::size_t available = input.size();
        for (;;) {
          std::uint8_t buffer[65536];
          std::uint8_t* cursor = buffer;
          std::size_t room = std::min(sizeof buffer, budget_.remainingOutput() + 1);
          const std::size_t before = available;
          const int result = api_.decompressStream(state_, &available, &next, &room, &cursor, nullptr);
          const auto produced = static_cast<std::size_t>(cursor - buffer);
          if (result == kBrotliError) decompressionFailed("BrotliDecompressionError", url_);
          if (!budget_.advance(before - available, produced)) decompressionFailed("ResponseBodyTooLarge", url_);
          out.append(reinterpret_cast<const char*>(buffer), produced);
          if (result == kBrotliSuccess) {
            done_ = true;
            return;
          }
          if (result == kBrotliNeedsMoreInput) return;
        }
      }

      void finish(std::string&) override {
        if (received_ && !done_) decompressionFailed("BrotliDecompressionError", url_);
      }

    private:
      const BrotliApi& api_;
      std::string url_;
      BrotliState* state_;
      DecompressionBudget budget_;
      bool received_ = false;
      bool done_ = false;
    };

    // ---- zstd -------------------------------------------------------------------

#if defined(UOS_GATEWAY_NATIVE_LIBRARIES)
    using ZstdInBuffer = ::ZSTD_inBuffer;
    using ZstdOutBuffer = ::ZSTD_outBuffer;
    using ZstdStream = ::ZSTD_DStream;
    constexpr auto kZstdWindowLogMax = ZSTD_d_windowLogMax;
    constexpr auto kZstdWindowTooLarge = ZSTD_error_frameParameter_windowTooLarge;
    struct ZstdApi {
      decltype(&::ZSTD_createDStream) createDStream = &::ZSTD_createDStream;
      decltype(&::ZSTD_freeDStream) freeDStream = &::ZSTD_freeDStream;
      decltype(&::ZSTD_initDStream) initDStream = &::ZSTD_initDStream;
      decltype(&::ZSTD_DCtx_setParameter) setParameter = &::ZSTD_DCtx_setParameter;
      decltype(&::ZSTD_decompressStream) decompressStream = &::ZSTD_decompressStream;
      decltype(&::ZSTD_isError) isError = &::ZSTD_isError;
      decltype(&::ZSTD_getErrorCode) getErrorCode = &::ZSTD_getErrorCode;
    };
#else
    // zstd 1.x lib/zstd.h (streaming decompression)
    using ZstdStream = void;
    constexpr int kZstdWindowLogMax = 100;  // ZSTD_d_windowLogMax in the zstd 1.x API.
    constexpr int kZstdWindowTooLarge = 16;  // ZSTD_error_frameParameter_windowTooLarge.
    struct ZstdInBuffer {
      const void* src;
      std::size_t size;
      std::size_t pos;
    };
    struct ZstdOutBuffer {
      void* dst;
      std::size_t size;
      std::size_t pos;
    };
    struct ZstdApi {
      void* (*createDStream)() = nullptr;
      std::size_t (*freeDStream)(void*) = nullptr;
      std::size_t (*initDStream)(void*) = nullptr;
      std::size_t (*setParameter)(void*, int, int) = nullptr;
      std::size_t (*decompressStream)(void*, ZstdOutBuffer*, ZstdInBuffer*) = nullptr;
      unsigned (*isError)(std::size_t) = nullptr;
      int (*getErrorCode)(std::size_t) = nullptr;
    };

#endif

    const ZstdApi* zstdApi() {
#if defined(UOS_GATEWAY_NATIVE_LIBRARIES)
      static const ZstdApi api;
      return &api;
#else
      static const std::optional<ZstdApi> api = []() -> std::optional<ZstdApi> {
#if defined(__APPLE__)
        void* library = ::dlopen("libzstd.1.dylib", RTLD_NOW | RTLD_LOCAL);
#else
        void* library = ::dlopen("libzstd.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
        if (!library) return std::nullopt;
        ZstdApi loaded;
        if (!resolveSymbol(library, loaded.createDStream, "ZSTD_createDStream") || !resolveSymbol(library, loaded.freeDStream, "ZSTD_freeDStream") ||
            !resolveSymbol(library, loaded.initDStream, "ZSTD_initDStream") || !resolveSymbol(library, loaded.setParameter, "ZSTD_DCtx_setParameter") ||
            !resolveSymbol(library, loaded.decompressStream, "ZSTD_decompressStream") ||
            !resolveSymbol(library, loaded.isError, "ZSTD_isError") || !resolveSymbol(library, loaded.getErrorCode, "ZSTD_getErrorCode"))
          return std::nullopt;
        return loaded;
      }();
      return api ? &*api : nullptr;
#endif
    }

    // Frames follow one another; anything else after a frame is an error.
    class ZstdDecoder final : public ContentDecoder {
    public:
      ZstdDecoder(const ZstdApi& api, std::string url) : api_(api), url_(std::move(url)), stream_(api.createDStream()) {
        if (!stream_) throw std::bad_alloc();
        if (api_.isError(api_.initDStream(stream_)) || api_.isError(api_.setParameter(stream_, kZstdWindowLogMax, limits::kZstdWindowLog))) {
          api_.freeDStream(stream_);
          decompressionFailed("ZstdDecompressionError", url_);
        }
      }
      ~ZstdDecoder() override { api_.freeDStream(stream_); }
      ZstdDecoder(const ZstdDecoder&) = delete;
      ZstdDecoder& operator=(const ZstdDecoder&) = delete;

      void write(std::string_view input, std::string& out) override {
        if (input.empty()) return;
        if (!budget_.feed(input.size())) decompressionFailed("ResponseBodyTooLarge", url_);
        received_ = true;
        ZstdInBuffer in{input.data(), input.size(), 0};
        for (;;) {
          std::uint8_t buffer[65536];
          ZstdOutBuffer target{buffer, std::min(sizeof buffer, budget_.remainingOutput() + 1), 0};
          const auto before = in.pos;
          const std::size_t result = api_.decompressStream(stream_, &target, &in);
          if (api_.isError(result)) decompressionFailed(api_.getErrorCode(result) == kZstdWindowTooLarge ? "ResponseBodyTooLarge" : "ZstdDecompressionError", url_);
          if (!budget_.advance(in.pos - before, target.pos)) decompressionFailed("ResponseBodyTooLarge", url_);
          out.append(reinterpret_cast<const char*>(buffer), target.pos);
          frameOpen_ = result != 0;
          if (in.pos == in.size && (!frameOpen_ || target.pos < target.size)) return;
        }
      }

      void finish(std::string&) override {
        if (received_ && frameOpen_) decompressionFailed("ZstdDecompressionError", url_);
      }

    private:
      const ZstdApi& api_;
      std::string url_;
      ZstdStream* stream_;
      DecompressionBudget budget_;
      bool received_ = false;
      bool frameOpen_ = false;
    };

    // A coding whose library could not be loaded (it is then not advertised):
    // a body that uses it anyway fails as undecodable when read.
    class UnavailableDecoder final : public ContentDecoder {
    public:
      UnavailableDecoder(std::string code, std::string url) : code_(std::move(code)), url_(std::move(url)) {}
      void write(std::string_view input, std::string&) override {
        if (!input.empty()) decompressionFailed(code_, url_);
      }
      void finish(std::string&) override {}

    private:
      std::string code_;
      std::string url_;
    };

    char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

    std::string lowerTrimmed(std::string_view token) {
      while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.remove_prefix(1);
      while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.remove_suffix(1);
      std::string out(token);
      for (char& c : out) c = lowerAscii(c);
      return out;
    }
  }

  std::unique_ptr<ContentDecoder> contentDecoder(const std::optional<std::string>& contentEncoding, const std::string& url) {
    if (!contentEncoding) return nullptr;
    std::vector<std::string> codings;
    std::size_t start = 0;
    for (;;) {
      const std::size_t comma = contentEncoding->find(',', start);
      const std::string token = lowerTrimmed(std::string_view(*contentEncoding).substr(start, comma == std::string::npos ? std::string::npos : comma - start));
      if (token != "identity") codings.push_back(token);
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    if (codings.size() != 1) return nullptr;
    const std::string& coding = codings.front();
    if (coding == "gzip" || coding == "x-gzip" || coding == "deflate") {
      if (!zlibAvailable()) return std::make_unique<UnavailableDecoder>("ZlibError", url);
      return std::make_unique<ZlibDecoder>(coding != "deflate", url);
    }
    if (coding == "br") {
      const BrotliApi* api = brotliApi();
      if (!api) return std::make_unique<UnavailableDecoder>("BrotliDecompressionError", url);
      return std::make_unique<BrotliDecoder>(*api, url);
    }
    if (coding == "zstd") {
      const ZstdApi* api = zstdApi();
      if (!api) return std::make_unique<UnavailableDecoder>("ZstdDecompressionError", url);
      return std::make_unique<ZstdDecoder>(*api, url);
    }
    return nullptr;
  }

  const std::string& acceptEncoding() {
    static const std::string value = [] {
      std::string codings = zlibAvailable() ? "gzip, deflate" : "";
      if (brotliApi()) codings += codings.empty() ? "br" : ", br";
      if (zstdApi()) codings += codings.empty() ? "zstd" : ", zstd";
      return codings.empty() ? std::string("identity") : codings;
    }();
    return value;
  }
}
