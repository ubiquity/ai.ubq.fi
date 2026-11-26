#pragma once
// Request and Response are handles. Copies share headers, body and identity.
// Inbound form fields and file contents are owned values.
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/url.hpp"
#include "uos/rt/weak.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace uos::rt {
  // `Uint8Array` / `ArrayBuffer` contents.
  using Bytes = std::vector<std::uint8_t>;
  // `ReadableStream<Uint8Array>`.
  using ByteStream = ReadableStream<ByteChunk>;

  // A TypeError carrying Bun's `code` property, e.g. "Body already used" with
  // code "ERR_BODY_ALREADY_USED".
  struct CodedTypeError : TypeError {
    std::string code;
    CodedTypeError(std::string message_, std::string code_) : TypeError(std::move(message_)), code(std::move(code_)) {}
  };

  struct FormFile {
    std::optional<std::string> filename;
    std::string mediaType;
    Bytes bytes;
  };
  using FormValue = std::variant<std::string, FormFile>;
  using FormParts = std::vector<std::pair<std::string, FormValue>>;

  // The result borrows from parts, which the caller keeps alive.
  const FormValue* findFormPart(const FormParts& parts, std::string_view name);

  class InboundFormDecoder {
  public:
    explicit InboundFormDecoder(std::string_view contentType);
    // The caller bounds the body before decoding. Returned parts own their data.
    FormParts decode(std::span<const std::uint8_t> body) const;

  private:
    std::optional<std::string> boundary_;  // Absent for URL-encoded forms.
  };

  using BodyInit = std::variant<std::nullptr_t, std::string, Bytes, ByteStream, URLSearchParams>;

  // `other.body` passed on as a body (`new Response(response.body, init)`): a
  // null body stays null.
  inline BodyInit bodyInit(std::optional<ByteStream> body) { return body ? BodyInit(std::move(*body)) : BodyInit(nullptr); }

  namespace detail {
    struct BodyState;
    struct ResponseState;
    struct RequestState;
  }

  // `ResponseInit`. A status is a JS number: it is truncated toward zero (NaN is
  // 0, infinities saturate) and must then be 101 or 200..599.
  struct ResponseInit {
    std::optional<double> status{};
    std::optional<std::string> statusText{};
    std::optional<Headers> headers{};
  };

  // A JS Response object (handle; see the file comment).
  class Response {
  public:
    // `new Response(body, init)`. A string body gets no content-type (Bun);
    // URLSearchParams adds its content type unless the headers
    // have one: at once with init.headers, else at the first headers() access
    // and only while the body is an untouched buffer. Throws RangeError for a
    // bad status and TypeError for a locked or disturbed stream.
    Response();
    explicit Response(BodyInit body, ResponseInit init = {});
    // `Response.json(data, init)`: content-type "application/json;charset=utf-8"
    // unless init.headers has one.
    static Response json(const json::value& data, ResponseInit init = {});
    // `Response.redirect(url, status)`: RangeError unless status is 301, 302,
    // 303, 307 or 308. The Location is the parsed URL's href, or `url` itself
    // when it does not parse.
    static Response redirect(std::string_view url, double status = 302);
    // `Response.error()`
    static Response error();

    int status() const;
    const std::string& statusText() const;
    bool ok() const;
    // `response.headers`: the response's own Headers object.
    Headers& headers() const;
    const std::string& url() const;
    bool redirected() const;
    // "default", or "error" for Response.error() and status 101 (Bun).
    std::string type() const;

    // `response.body`: std::nullopt for a null body. A buffered body is exposed
    // as a stream on first access (a chunk of up to 16 KiB, then chunks of up
    // to 2 MiB) and the same stream is returned afterwards.
    std::optional<ByteStream> body() const;
    bool bodyUsed() const;
    // Body readers reject with CodedTypeError "Body already used" once the body
    // was read, and read a null body as empty without using it.
    [[nodiscard]] Task<std::string> text() const;
    // `response.json()`: for a buffered body SyntaxError "Unexpected end of JSON
    // input" when it has no bytes, "Failed to parse JSON" for other invalid
    // text; for a stream body JSON.parse's own SyntaxError.
    [[nodiscard]] Task<json::value> json() const;
    [[nodiscard]] Task<Bytes> arrayBuffer() const;
    [[nodiscard]] Task<Bytes> bytes() const;
    // `response.clone()`: CodedTypeError "Body is disturbed or locked" once
    // used; a stream body is teed between the two.
    Response clone() const;

    // Identity, for `===` and WeakMap keys.
    friend bool operator==(const Response& left, const Response& right) noexcept { return left.state_ == right.state_; }
    const void* identity() const noexcept { return state_.get(); }
    std::weak_ptr<const void> weakIdentity() const noexcept { return state_; }

    // Transport side (no JS equivalent): the url and redirected flag a fetch
    // implementation records on the Response it resolves with, and the
    // replacement of its body by a stream (std::nullopt: null body).
    void setFetchResult(std::string url, bool redirected) const;
    void setBodyStream(std::optional<ByteStream> stream) const;
    const std::shared_ptr<detail::ResponseState>& state() const noexcept { return state_; }

  private:
    struct Adopt {};
    Response(Adopt, std::shared_ptr<detail::ResponseState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::ResponseState> state_;
  };

  class Request;

  // `RequestInit`. `body` std::nullopt is undefined (keep the input request's
  // body); BodyInit(nullptr) is null (no body). `redirect` must be "follow",
  // "manual" or "error"; `cache` one of the six Fetch cache modes; `duplex` is
  // accepted and ignored (Bun).
  struct RequestInit {
    std::optional<std::string> method{};
    std::optional<Headers> headers{};
    std::optional<BodyInit> body{};
    std::optional<AbortSignal> signal{};
    std::optional<std::string> redirect{};
    std::optional<std::string> cache{};
    std::optional<std::string> duplex{};
  };

  // A JS Request object (handle; see the file comment).
  class Request {
  public:
    // `new Request(url, init)`: Error "Failed to construct 'Request': url is
    // required." for "", CodedTypeError "Failed to construct 'Request': Invalid
    // URL \"<url>\"" (ERR_INVALID_URL) for anything that does not parse.
    explicit Request(std::string_view url, RequestInit init = {});
    explicit Request(const char* url, RequestInit init = {}) : Request(std::string_view(url), std::move(init)) {}
    explicit Request(const std::string& url, RequestInit init = {}) : Request(std::string_view(url), std::move(init)) {}
    // `new Request(urlObject, init)`
    explicit Request(const URL& url, RequestInit init = {});
    // `new Request(url, request)`: the fields of `init` are read off a Request
    // object (method, headers, signal, redirect, cache, and a clone of its body).
    Request(std::string_view url, const Request& init);
    Request(const URL& url, const Request& init);
    // `new Request(request)` (init std::nullopt, written `Request(input, {})`)
    // and `new Request(request, init)` (written `Request(input, RequestInit{...})`):
    // a new Request (new identity, copied headers) whose fields default to
    // input's. Without init.body its body is a clone of input's, so input stays
    // readable (Bun): a stream is teed (a disturbed one is shared, a locked one
    // throws CodedTypeError "Invalid state: ReadableStream is locked"), and a
    // read body becomes empty without init but throws TypeError "Body object
    // should not be disturbed or locked" with one. The one-argument copy
    // constructor aliases instead.
    Request(const Request& input, std::optional<RequestInit> init);
    Request(const Request&) = default;
    Request(Request&&) noexcept = default;
    Request& operator=(const Request&) = default;
    Request& operator=(Request&&) noexcept = default;

    // `request.method`: Bun keeps only the methods it knows (written in upper
    // or lower case, returned upper case); anything else becomes "GET".
    const std::string& method() const;
    // `request.url`: the parsed URL's href.
    const std::string& url() const;
    Headers& headers() const;
    // `request.signal`: init.signal itself, else input's, else a fresh signal.
    const AbortSignal& signal() const;
    const std::string& redirect() const;
    const std::string& cache() const;

    std::optional<ByteStream> body() const;
    // Retained buffer size or the native transport's stream bound. An opaque
    // stream without transport metadata has no known bound.
    std::optional<std::uint64_t> bodySizeUpperBound() const;
    bool bodyUsed() const;
    [[nodiscard]] Task<std::string> text() const;
    [[nodiscard]] Task<json::value> json() const;
    [[nodiscard]] Task<Bytes> arrayBuffer() const;
    [[nodiscard]] Task<Bytes> bytes() const;
    Request clone() const;

    friend bool operator==(const Request& left, const Request& right) noexcept { return left.state_ == right.state_; }
    const void* identity() const noexcept { return state_.get(); }
    std::weak_ptr<const void> weakIdentity() const noexcept { return state_; }
    const std::shared_ptr<detail::RequestState>& state() const noexcept { return state_; }

  private:
    struct Adopt {};
    Request(Adopt, std::shared_ptr<detail::RequestState> state) : state_(std::move(state)) {}
    void initialize(std::string href, const Request* input, RequestInit init, bool initGiven);
    std::shared_ptr<detail::RequestState> state_;
  };

  // `RequestInfo | URL`: what fetch accepts as its first argument.
  using RequestInfo = std::variant<std::string, URL, Request>;

  namespace detail {
    // Inbound adapters attach parser-validated metadata before dispatch.
    void setRequestBodyStreamSizeUpperBound(const Request& request, std::uint64_t upperBound);
    // fetch(request): moves `input`'s body into `outgoing` (which has none),
    // leaving `input` used, as Bun's fetch consumes a Request's body.
    void transferBody(const Request& input, const Request& outgoing);
  }
}
