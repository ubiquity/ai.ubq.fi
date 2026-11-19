#pragma once
// Internals of the HTTP/1.1 client transport (uos/rt/fetch_transport.hpp):
// Bun's wire rules for the request head, the response parser, the content
// decoders, the connection (TCP, or TLS over
// OpenSSL memory BIOs) and the per-loop keep-alive pool.
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/fetch_transport.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/limits.hpp"
#include "third_party/llhttp/llhttp.h"

#include <boost/asio/execution_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <openssl/ssl.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uos::rt::detail::fetch {
  // ---- errors: Bun's texts ------------------------------------------------------

  // "<code> fetching \"<url>\". For more information, pass `verbose: true` in
  // the second argument to fetch()"
  FetchError verboseError(std::string_view code, const std::string& url);
  // "The socket connection was closed unexpectedly. ..." (ECONNRESET).
  FetchError closedError(const std::string& url);
  // "Unable to connect. Is the computer able to access the url?" (ConnectionRefused).
  FetchError connectError(const std::string& url);
  // "getaddrinfo ENOTFOUND <host>" (ENOTFOUND, errno 4, syscall, hostname).
  FetchError dnsError(const std::string& host, const std::string& url);

  // ---- request head -------------------------------------------------------------

  enum class RequestFraming : std::uint8_t { none, length, chunked };

  // How a header name is written: Bun's spelling of WebKit's common header names
  // ("Content-Type", "Last-Event-Id"), whatever the given case; otherwise the
  // name as given (rt::Headers keeps the first spelling: detail::headerWireFields).
  std::string wireName(const std::string& name);

  // The request head Bun writes: the request line (path and query, no
  // fragment), the request's headers under their wire names (content-length and
  // transfer-encoding dropped; Cookie values appended to one another joined with
  // "; ") sorted by wire name in byte order, each Set-Cookie value as a
  // "set-cookie" line, then
  // Connection, User-Agent, Accept, Host and Accept-Encoding unless the
  // request has them, then the body framing (Content-Length for a buffered
  // body and for methods other than GET, HEAD and TRACE without one,
  // Transfer-Encoding: chunked for a stream). Values are written as Latin-1.
  std::string requestHead(const std::string& method, const URL& url, const Headers& headers, RequestFraming framing, std::uint64_t length, const std::string& userAgent,
                          const std::string& acceptEncoding);

  // ---- response head ------------------------------------------------------------

  enum class Framing : std::uint8_t { empty, length, chunked, close };
  struct FramingRule {
    Framing framing = Framing::empty;
    std::uint64_t length = 0;
    const char* error = nullptr;
  };

  struct ResponseHead {
    int minorVersion = 1;
    int status = 0;
    // The reason phrase and the fields as raw bytes (field values trimmed).
    std::string reason;
    std::vector<std::pair<std::string, std::string>> fields;
    FramingRule framing;
    // The values joined with ", ", std::nullopt without any.
    std::optional<std::string> field(std::string_view lowercaseName) const;
    // A Connection token (case-insensitive) is present.
    bool connectionHas(std::string_view token) const;
  };

  enum class HeadStatus : std::uint8_t { incomplete, complete, malformed };
  struct HeadParse {
    HeadStatus status = HeadStatus::incomplete;
    // The head's length in bytes (complete only).
    std::size_t length = 0;
    ResponseHead head;
    llhttp_errno_t error = HPE_OK;
    const char* errorReason = nullptr;
  };

  // Stops at the end of the head and again at the end of the message. All
  // byte counts are relative to the input passed to that call.
  class ResponseParser {
  public:
    explicit ResponseParser(std::size_t maxFields = limits::kHttpHeaderFields, std::size_t maxHeadBytes = limits::kHttpHeaderBlockBytes);
    ResponseParser(const ResponseParser&) = delete;
    ResponseParser& operator=(const ResponseParser&) = delete;

    llhttp_errno_t readHead(std::string_view input, std::size_t& consumed);
    const ResponseHead& head() const { return head_; }
    ResponseHead takeHead() { return std::move(head_); }
    const char* errorReason() const { return llhttp_get_error_reason(&parser_); }
    bool headLimitReached() const { return headBytes_ == maxHeadBytes_; }
    bool bodyLimitExceeded() const { return bodyLimitExceeded_; }
    // Completes immediately if llhttp determines the response has no body.
    llhttp_errno_t beginBody(bool headRequest = false);
    bool needsEof() const { return llhttp_message_needs_eof(&parser_); }
    bool keepsAlive() const { return !(parser_.flags & F_CONNECTION_CLOSE) && llhttp_should_keep_alive(&parser_); }
    bool paused() const { return llhttp_get_errno(&parser_) == HPE_PAUSED; }
    void reset();
    llhttp_errno_t readBody(std::string_view input, std::size_t& consumed, ByteChunk* out = nullptr);
    llhttp_errno_t readBody(std::string_view input, std::size_t& consumed, std::string& out);
    llhttp_errno_t finish() { return llhttp_finish(&parser_); }

  private:
    enum class ChunkPart { size, data, trailers };
    static const llhttp_settings_t settings_;
    llhttp_errno_t execute(std::string_view input, std::size_t& consumed);

    llhttp_t parser_;
    ResponseHead head_;
    ByteChunk* bodyBytes_ = nullptr;
    std::size_t bodyInputSize_ = 0;
    std::string* body_ = nullptr;
    std::size_t maxFields_;
    std::size_t maxHeadBytes_;
    std::size_t headBytes_ = 0;
    std::size_t bodyBytesRemaining_ = limits::kHttpBodyBytes;
    std::size_t metadataBytes_ = 0;
    std::size_t metadataRemaining_ = limits::kHttpChunkMetadataBytes;
    std::size_t trailerCount_ = 0;
    ChunkPart chunkPart_ = ChunkPart::size;
    bool bodyComplete_ = false;
    bool bodyLimitExceeded_ = false;
    bool chunked_ = false;
    bool firstHeadBytes_ = true;
    bool startField_ = true;
  };

  HeadParse parseResponseHead(std::string_view data, std::size_t maxBytes = limits::kHttpHeaderBlockBytes, std::size_t maxFields = limits::kHttpHeaderFields);
  FramingRule responseFraming(const std::string& method, const ResponseHead& head);

  // Standalone entry point for decoding an already identified chunked body.
  class ChunkedDecoder {
  public:
    enum class Result : std::uint8_t { more, done, invalid };
    // Consumes what it can from `input` from `offset` on (advancing it) and
    // appends the chunk data to `out`.
    Result decode(std::string_view input, std::size_t& offset, std::string& out);

  private:
    ResponseParser parser_;
    bool started_ = false;
  };

  // A Content-Encoding decoder. write() and finish() throw the FetchError Bun
  // reports for corrupt or truncated data (ZlibError, BrotliDecompressionError,
  // ZstdDecompressionError).
  class ContentDecoder {
  public:
    virtual ~ContentDecoder() = default;
    virtual void write(std::string_view input, std::string& out) = 0;
    virtual void finish(std::string& out) = 0;
  };
  // The decoder for a Content-Encoding value as Bun picks it: tokens split at
  // commas, trimmed, compared case-insensitively, "identity" ignored; exactly
  // one of gzip, x-gzip, deflate, br, zstd left decodes, anything else passes
  // the bytes through (nullptr).
  std::unique_ptr<ContentDecoder> contentDecoder(const std::optional<std::string>& contentEncoding, const std::string& url);
  // The Accept-Encoding the transport sends: Bun's "gzip, deflate, br, zstd",
  // without the codings whose library is not available.
  const std::string& acceptEncoding();

  // ---- connection -----------------------------------------------------------------

  // Received bytes not yet consumed.
  class ByteQueue {
  public:
    std::string_view view() const noexcept { return std::string_view(data_).substr(begin_); }
    std::size_t size() const noexcept { return data_.size() - begin_; }
    bool empty() const noexcept { return size() == 0; }
    void consume(std::size_t count);
    void append(const char* bytes, std::size_t count);
    void clear() noexcept;

  private:
    std::string data_;
    std::size_t begin_ = 0;
  };

  enum class ReadStatus : std::uint8_t { data, eof, failed, aborted };

  // One HTTP/1.1 connection: a connected TCP socket on the loop, with an
  // optional TLS client layer over memory BIOs (every socket operation stays
  // a plain Asio operation, so Asio cancellation reaches it).
  class Connection : public std::enable_shared_from_this<Connection> {
  public:
    Connection(boost::asio::ip::tcp::socket socket, std::string key, bool secure);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // The pool key: "<scheme>//<host>:<port>".
    const std::string key;
    const bool secure;
    // Plaintext received and not yet consumed.
    ByteQueue input;
    // Taken from the pool for the current exchange.
    bool reused = false;
    // When it became idle (loop clock, monotonic ms).
    double idleSince = 0;

    bool isOpen() const noexcept { return open_; }
    // Installed only while the upload writer can be waiting on a stream read.
    Job stopUpload;
    // Closes the socket: pending operations complete with operation_aborted.
    void close() noexcept;
    // Bun's fetch timeout: once `ms` pass without a read or a write, the
    // connection is closed and timedOut() turns true. An unreferenced timer.
    void watchIdle(double ms);
    void unwatchIdle() noexcept;
    bool timedOut() const noexcept { return timedOut_; }
    // The TLS handshake: SNI (unless an IP address), ALPN "http/1.1", then the
    // chain (when rejectUnauthorized) and the host name are judged. The
    // failure as a FetchError, or a null Reason; aborted on cancellation.
    struct HandshakeResult {
      Reason failure;
      bool aborted = false;
    };
    Task<HandshakeResult> handshake(SSL_CTX* context, std::string host, bool rejectUnauthorized, std::string url);
    // Reads more plaintext into `input`: what one socket read gave (TLS:
    // every record it completed).
    Task<ReadStatus> fill();
    // Writes `size` bytes at `data` (kept alive by `owner`) in full; false
    // when the connection failed or was closed.
    Task<bool> write(std::shared_ptr<const void> owner, const std::uint8_t* data, std::size_t size);
    // A pooled connection is usable: nothing arrived on it (no data, no EOF,
    // no error) and nothing is buffered. TLS records that carry no
    // application data (session tickets) are consumed.
    bool idleHealthy();
    // Bytes received beyond `input` (TLS plaintext or ciphertext not yet read).
    bool hasBufferedInput() const;

  private:
    Task<ReadStatus> readSome();
    Task<bool> flushTls();
    // Restarts the idle countdown.
    void touch();
    boost::asio::ip::tcp::socket socket_;
    SSL* ssl_ = nullptr;
    BIO* rbio_ = nullptr;
    BIO* wbio_ = nullptr;
    std::vector<std::uint8_t> readBuffer_;
    bool open_ = true;
    Timeout idleTimer_;
    bool timedOut_ = false;
  };

  // ---- pool -------------------------------------------------------------------------

  // The idle connections of one transport on one loop, in Bun's slot order
  // (a released connection takes the first free slot; acquire scans slots in
  // order), one slot array per scheme.
  class Pool : public std::enable_shared_from_this<Pool> {
  public:
    Pool(std::size_t capacity, double keepAliveTimeoutMs);
    ~Pool();
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    // A healthy idle connection for `key`, or null. Unhealthy and expired ones are closed.
    std::shared_ptr<Connection> acquire(const std::string& key, bool secure);
    // Keeps `connection` for reuse, or closes it when its scheme's slots are full.
    void release(std::shared_ptr<Connection> connection);
    std::size_t idleCount() const noexcept;
    void closeAll() noexcept;

  private:
    void sweep();
    void arm();
    std::vector<std::shared_ptr<Connection>>& slots(bool secure) { return secure ? secure_ : plain_; }
    double timeoutMs_;
    std::vector<std::shared_ptr<Connection>> plain_;
    std::vector<std::shared_ptr<Connection>> secure_;
    Timeout timer_;
    double timerDeadline_ = 0;
  };

  // The pools living on one io_context (one rt::Loop), keyed by transport. The
  // context's shutdown destroys them while its socket services still exist.
  class PoolRegistry final : public boost::asio::execution_context::service {
  public:
    using key_type = PoolRegistry;
    inline static boost::asio::execution_context::id id;
    explicit PoolRegistry(boost::asio::execution_context& context);
    std::shared_ptr<Pool> pool(std::uint64_t owner, std::size_t capacity, double keepAliveTimeoutMs);
    void remove(std::uint64_t owner) noexcept;

  private:
    void shutdown() override;
    std::map<std::uint64_t, std::shared_ptr<Pool>> pools_;
  };

  // ---- transport state ----------------------------------------------------------

  struct ClientState {
    HttpClientOptions options;
    std::string userAgent;
    std::uint64_t id = 0;
    // The TLS context (owned when options.ca is set).
    SSL_CTX* context = nullptr;
    // The pools this transport created, by loop.
    struct LoopPool {
      PoolRegistry* registry = nullptr;
      std::weak_ptr<Pool> pool;
    };
    std::vector<LoopPool> pools;

    explicit ClientState(HttpClientOptions options_);
    ~ClientState();
    ClientState(const ClientState&) = delete;
    ClientState& operator=(const ClientState&) = delete;
    // This transport's pool on the current loop.
    std::shared_ptr<Pool> pool();
    // The pool on the current loop if one exists.
    std::shared_ptr<Pool> existingPool() const;
    bool rejectUnauthorized() const;
  };

  // The request side of fetch: Bun's redirect handling around exchanges.
  Task<Response> send(std::shared_ptr<ClientState> client, Request request);
}
