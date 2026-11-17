#pragma once
// Fetch `Request`, `Response` and their body, plus `Blob`, `File` and
// `FormData`, as Bun 1.4 implements them (including its departures from the
// Fetch standard; see docs/rt/web.md). Request and Response are handles: copies
// refer to the same JS object (same headers, same body, same identity), exactly
// like JS references. Blob, File, FormData and Headers are values.
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

  class Blob;
  namespace detail {
    struct FormDataAccess;
  }

  // One item of `new Blob(parts)` / `new File(parts, name)`: a string (encoded as
  // UTF-8, lone surrogates as U+FFFD), bytes, or another Blob.
  struct BlobPart {
    BlobPart(std::string text);
    BlobPart(const char* text) : BlobPart(std::string(text)) {}
    BlobPart(Bytes bytes);
    BlobPart(const Blob& blob);
    std::shared_ptr<const Bytes> bytes;
  };

  // `Blob`: immutable bytes and a type.
  class Blob {
  public:
    // `new Blob()`
    Blob();
    // `new Blob(parts, { type })`: the type is lowercased, or "" when it holds a
    // character outside U+0020..U+007E.
    explicit Blob(const std::vector<BlobPart>& parts, std::string_view type = "");
    virtual ~Blob() = default;

    double size() const { return static_cast<double>(bytes_->size()); }
    const std::string& type() const { return type_; }
    // `blob.text()`: UTF-8 decoded, BOM stripped, ill-formed bytes as U+FFFD.
    [[nodiscard]] Task<std::string> text() const;
    [[nodiscard]] Task<Bytes> arrayBuffer() const;
    [[nodiscard]] Task<Bytes> bytes() const;
    // `blob.stream()`: one chunk holding every byte (none when empty), then close.
    ByteStream stream() const;

    // The bytes, synchronously (no JS equivalent; for transports and tests).
    const Bytes& data() const { return *bytes_; }
    const std::shared_ptr<const Bytes>& shared() const { return bytes_; }

  protected:
    struct Internal {};
    Blob(Internal, std::shared_ptr<const Bytes> bytes, std::string type) : bytes_(std::move(bytes)), type_(std::move(type)) {}
    std::shared_ptr<const Bytes> bytes_;
    std::string type_;
    friend class FormData;
    friend struct detail::FormDataAccess;
  };

  // `File`: a Blob with a name and lastModified.
  class File : public Blob {
  public:
    // `new File(parts, name, { type, lastModified })`; lastModified defaults to Date.now().
    File(const std::vector<BlobPart>& parts, std::string name, std::string_view type = "", std::optional<double> lastModified = std::nullopt);
    // `file.name`. std::nullopt where Bun leaves it undefined: a File parsed from
    // a multipart part whose filename or content is empty, and a Blob appended to
    // FormData without a filename. TS code reading `.name` there throws.
    const std::optional<std::string>& name() const { return name_; }
    double lastModified() const { return lastModified_; }

  private:
    friend class FormData;
    friend struct detail::FormDataAccess;
    File(Internal, std::shared_ptr<const Bytes> bytes, std::string type, std::optional<std::string> name, double lastModified);
    std::optional<std::string> name_;
    double lastModified_ = 0;
  };

  // `FormData`. Names and string values are USVStrings (lone surrogates become
  // U+FFFD). Copying copies the entry list.
  class FormData {
  public:
    // `FormDataEntryValue`: `typeof value === "string"` is std::holds_alternative<std::string>.
    using Value = std::variant<std::string, File>;
    using Entry = std::pair<std::string, Value>;

    FormData() = default;
    // `form.append(name, value)` / `form.append(name, blob, filename?)`. A File
    // keeps its own name even when a filename is given (Bun); a plain Blob
    // becomes a File named `filename`, or with no name (Bun: undefined).
    void append(std::string_view name, std::string_view value);
    void append(std::string_view name, const char* value) { append(name, std::string_view(value)); }
    void append(std::string_view name, const Blob& value, std::optional<std::string_view> filename = std::nullopt);
    void append(std::string_view name, const File& value, std::optional<std::string_view> filename = std::nullopt);
    void delete_(std::string_view name);
    // `form.get(name)`: std::nullopt where JS returns null.
    std::optional<Value> get(std::string_view name) const;
    std::vector<Value> getAll(std::string_view name) const;
    bool has(std::string_view name) const;
    // `form.set(...)`: replaces the first entry with this name in place and
    // removes the others.
    void set(std::string_view name, std::string_view value);
    void set(std::string_view name, const char* value) { set(name, std::string_view(value)); }
    void set(std::string_view name, const Blob& value, std::optional<std::string_view> filename = std::nullopt);
    void set(std::string_view name, const File& value, std::optional<std::string_view> filename = std::nullopt);

    // `[...form.entries()]`, `[...form.keys()]`, `[...form.values()]` snapshots.
    const std::vector<Entry>& entries() const { return entries_; }
    std::vector<std::string> keys() const;
    std::vector<Value> values() const;
    std::vector<Entry>::const_iterator begin() const { return entries_.begin(); }
    std::vector<Entry>::const_iterator end() const { return entries_.end(); }

  private:
    void setValue(std::string_view name, Value value);
    std::vector<Entry> entries_;
  };

  // `BodyInit`: what `new Response(body)` and `RequestInit.body` accept.
  // nullptr is JS `null`.
  using BodyInit = std::variant<std::nullptr_t, std::string, Bytes, ByteStream, URLSearchParams, FormData, Blob>;

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
    // URLSearchParams, FormData and typed Blobs add theirs unless the headers
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
    // `response.formData()`: multipart/form-data (Bun's parser) or
    // application/x-www-form-urlencoded by the content-type header (or the type
    // of an untouched buffered body), else CodedTypeError
    // (ERR_FORMDATA_PARSE_ERROR) without using the body.
    [[nodiscard]] Task<FormData> formData() const;
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
    bool bodyUsed() const;
    [[nodiscard]] Task<std::string> text() const;
    [[nodiscard]] Task<json::value> json() const;
    [[nodiscard]] Task<Bytes> arrayBuffer() const;
    [[nodiscard]] Task<Bytes> bytes() const;
    [[nodiscard]] Task<FormData> formData() const;
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
    // fetch(request): moves `input`'s body into `outgoing` (which has none),
    // leaving `input` used, as Bun's fetch consumes a Request's body.
    void transferBody(const Request& input, const Request& outgoing);
  }
}
