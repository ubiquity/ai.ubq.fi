// Differential tests of rt::HttpClientTransport (src/rt/fetch_transport*.cpp) against Bun 1.4.0's
// fetch. Every expected request head, response, error text and connection pattern below was
// recorded by running Bun's fetch against raw scripted TCP/TLS peers (Bun.listen, loopback,
// OS-assigned ports; docs/rt/fetch-transport.md lists the probes). Here the same scripts run on
// Asio peers on the test loop and the C++ transport must do what Bun did. Self-signed test
// certificates (generated with openssl for these tests only) are embedded below.
// Links: tests/rt/fetch_transport_test.cpp (every src/rt/*.cpp links automatically).
#include "uos/rt/fetch_transport.hpp"

#include "uos/rt/abort.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#include <openssl/ssl.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace uos::rt;
namespace rt = uos::rt;
namespace asio = boost::asio;

namespace {
  // ---- test certificates (EC P-256, self-signed, for these tests only) ------------------

  const std::string kLocalhostCert = R"(-----BEGIN CERTIFICATE-----
MIIBmjCCAUGgAwIBAgIUCirVoSWuq4s8utdMjomv9C0lPQgwCgYIKoZIzj0EAwIw
FDESMBAGA1UEAwwJbG9jYWxob3N0MCAXDTI2MTExNjA4Mzg1OVoYDzIxMjYxMDIz
MDgzODU5WjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwWTATBgcqhkjOPQIBBggqhkjO
PQMBBwNCAARWVwt8rh43t2ahyAlBGvgbcInw6mpsCkqdMUO/y57sANXI0H18btFr
VQggErCgvkauWV8G+5RNrrj5yio6Qywwo28wbTAdBgNVHQ4EFgQUOp/IlPBy5jys
MZUqMd2LWde60HAwHwYDVR0jBBgwFoAUOp/IlPBy5jysMZUqMd2LWde60HAwDwYD
VR0TAQH/BAUwAwEB/zAaBgNVHREEEzARgglsb2NhbGhvc3SHBH8AAAEwCgYIKoZI
zj0EAwIDRwAwRAIgMkdRRF/q4IyRQLlWPGZ33hM62Qz3LUEVheNOhmLTbO8CIBFc
XVAfvRARxdvnO4e8cGZzsMhVi9wntTVCbtJ4DaCz
-----END CERTIFICATE-----
)";
  const std::string kLocalhostKey = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgSSH0ar+1lMSuKmVP
jqfsVKdL/uhZsYJAc8hfg5DcvJehRANCAARWVwt8rh43t2ahyAlBGvgbcInw6mps
CkqdMUO/y57sANXI0H18btFrVQggErCgvkauWV8G+5RNrrj5yio6Qyww
-----END PRIVATE KEY-----
)";
  // CN and DNS SAN example.test only.
  const std::string kOtherCert = R"(-----BEGIN CERTIFICATE-----
MIIBnzCCAUSgAwIBAgIUL6x11x7epUKFFCJbMjZz4ai2JTswCgYIKoZIzj0EAwIw
FzEVMBMGA1UEAwwMZXhhbXBsZS50ZXN0MCAXDTI2MTExNjA4Mzg1OVoYDzIxMjYx
MDIzMDgzODU5WjAXMRUwEwYDVQQDDAxleGFtcGxlLnRlc3QwWTATBgcqhkjOPQIB
BggqhkjOPQMBBwNCAATahrftKV0Mib7fsqBtZ/nIcm2TdvwHqlK9IPxcFo/+VUTZ
T00ltOYIsM0sW54ulId/lLvYAbcN9b8I5B38upt1o2wwajAdBgNVHQ4EFgQUzTvC
LdQkWvaAGZZDUfMrvPzXW2swHwYDVR0jBBgwFoAUzTvCLdQkWvaAGZZDUfMrvPzX
W2swDwYDVR0TAQH/BAUwAwEB/zAXBgNVHREEEDAOggxleGFtcGxlLnRlc3QwCgYI
KoZIzj0EAwIDSQAwRgIhAPCNO3CsnxLjHQaW+Icwb4Ij4BOnwL15LjbQURW3aXHB
AiEA5yb4hX7JO7YbrWqsXCbZyEKvWLLyTV2XLsYiGbmEfpA=
-----END CERTIFICATE-----
)";
  const std::string kOtherKey = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgnL1K5jF/pLnAB85l
+2azQ11rfGH4ZV4xtf6gOQrne8uhRANCAATahrftKV0Mib7fsqBtZ/nIcm2TdvwH
qlK9IPxcFo/+VUTZT00ltOYIsM0sW54ulId/lLvYAbcN9b8I5B38upt1
-----END PRIVATE KEY-----
)";
  // localhost, valid from 2020-01-01 to 2021-01-01.
  const std::string kExpiredCert = R"(-----BEGIN CERTIFICATE-----
MIIBmDCCAT+gAwIBAgIUOStxiDfzAhM1ZfJ0InsdV4Mt2DgwCgYIKoZIzj0EAwIw
FDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTIwMDEwMTAwMDAwMFoXDTIxMDEwMTAw
MDAwMFowFDESMBAGA1UEAwwJbG9jYWxob3N0MFkwEwYHKoZIzj0CAQYIKoZIzj0D
AQcDQgAEzW55+MZP+tpKbgbdbKouJlEdLdgZJN1lC41xXDQcn9c+z0zx6KOOn1sM
5MIVdJfXfJTTeGJ0b26RPFJXnSvo16NvMG0wHQYDVR0OBBYEFJibt8RLoi8ua5OX
ns1OEJeJNohkMB8GA1UdIwQYMBaAFJibt8RLoi8ua5OXns1OEJeJNohkMA8GA1Ud
EwEB/wQFMAMBAf8wGgYDVR0RBBMwEYIJbG9jYWxob3N0hwR/AAABMAoGCCqGSM49
BAMCA0cAMEQCIGXYXgzad8jtQ6IYjZu8mOXvq7yoHngzAaY4Pq6RWZNHAiBFoXAO
9dAb73HfUbEbV9+MG55vgTkLtc8T6FGTFe9emw==
-----END CERTIFICATE-----
)";
  const std::string kExpiredKey = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQg9rYQ/eSynng+PxBv
CFShBFtmBnNFXu0YN3ax8r59aRShRANCAATNbnn4xk/62kpuBt1sqi4mUR0t2Bkk
3WULjXFcNByf1z7PTPHoo46fWwzkwhV0l9d8lNN4YnRvbpE8UledK+jX
-----END PRIVATE KEY-----
)";

  // ---- scripted peers (Bun.listen's raw sockets in the probes) ------------------------

  struct PeerRequest {
    int connection = 0;
    // Head and body exactly as received.
    std::string wire;
  };

  class PeerConnection : public std::enable_shared_from_this<PeerConnection> {
  public:
    PeerConnection(int id_, asio::ip::tcp::socket socket, asio::ssl::context* tls) : id(id_) {
      if (tls) secure_.emplace(std::move(socket), *tls);
      else plain_.emplace(std::move(socket));
    }
    const int id;
    // The client closed or reset its side.
    bool clientClosed = false;
    std::string buffer;

    asio::ip::tcp::socket& socket() { return secure_ ? secure_->next_layer() : *plain_; }
    bool secure() const { return secure_.has_value(); }
    SSL* ssl() { return secure_ ? secure_->native_handle() : nullptr; }

    Task<bool> handshake() {
      boost::system::error_code error;
      auto token = asio::redirect_error(asio::use_awaitable, error);
      co_await secure_->async_handshake(asio::ssl::stream_base::server, token);
      co_return !error;
    }
    Task<std::size_t> readSome(char* data, std::size_t size) {
      boost::system::error_code error;
      auto token = asio::redirect_error(asio::use_awaitable, error);
      std::size_t count = 0;
      if (secure_) count = co_await secure_->async_read_some(asio::buffer(data, size), token);
      else count = co_await plain_->async_read_some(asio::buffer(data, size), token);
      co_return error ? 0 : count;
    }
    Task<void> send(std::string data) {
      boost::system::error_code error;
      auto token = asio::redirect_error(asio::use_awaitable, error);
      if (secure_) co_await asio::async_write(*secure_, asio::buffer(data), token);
      else co_await asio::async_write(*plain_, asio::buffer(data), token);
    }
    // `socket.end()`: FIN.
    void end() {
      boost::system::error_code ignored;
      socket().shutdown(asio::socket_base::shutdown_send, ignored);
    }
    // `socket.terminate()`: RST.
    void reset() {
      boost::system::error_code ignored;
      socket().set_option(asio::socket_base::linger(true, 0), ignored);
      socket().close(ignored);
    }
    void close() {
      boost::system::error_code ignored;
      socket().close(ignored);
    }

  private:
    std::optional<asio::ip::tcp::socket> plain_;
    std::optional<asio::ssl::stream<asio::ip::tcp::socket>> secure_;
  };

  // One complete request (Content-Length or chunked body) from the front of the buffer.
  std::optional<PeerRequest> takeRequest(PeerConnection& connection) {
    std::string& buffer = connection.buffer;
    const std::size_t headEnd = buffer.find("\r\n\r\n");
    if (headEnd == std::string::npos) return std::nullopt;
    std::string head = buffer.substr(0, headEnd + 4);
    std::string lower = head;
    for (char& c : lower) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    std::size_t total = headEnd + 4;
    const std::size_t lengthAt = lower.find("\r\ncontent-length: ");
    if (lengthAt != std::string::npos) {
      total += std::stoul(head.substr(lengthAt + 18));
      if (buffer.size() < total) return std::nullopt;
    } else if (lower.find("\r\ntransfer-encoding: chunked") != std::string::npos) {
      std::size_t offset = headEnd + 4;
      for (;;) {
        const std::size_t lineEnd = buffer.find("\r\n", offset);
        if (lineEnd == std::string::npos) return std::nullopt;
        const std::size_t size = std::stoul(buffer.substr(offset, lineEnd - offset), nullptr, 16);
        offset = lineEnd + 2;
        if (size == 0) {
          if (buffer.size() < offset + 2) return std::nullopt;
          offset += 2;
          break;
        }
        offset += size + 2;
        if (buffer.size() < offset) return std::nullopt;
      }
      total = offset;
    }
    PeerRequest request;
    request.connection = connection.id;
    request.wire = buffer.substr(0, total);
    buffer.erase(0, total);
    return request;
  }

  // TLS peers record the SNI and ALPN offers through these OpenSSL callbacks (defined below).
  int captureServerName(SSL* ssl, int* alert, void* argument);
  int captureAlpn(SSL* ssl, const unsigned char** out, unsigned char* outLength, const unsigned char* in, unsigned int inLength, void* argument);

  class Peer : public std::enable_shared_from_this<Peer> {
  public:
    using Responder = std::function<Task<void>(std::shared_ptr<PeerConnection>, PeerRequest)>;
    using Opener = std::function<Task<bool>(std::shared_ptr<PeerConnection>)>;

    explicit Peer(std::shared_ptr<asio::ssl::context> tls_) : acceptor(rt::executor()), tls(std::move(tls_)) {}

    static std::shared_ptr<Peer> start(std::shared_ptr<asio::ssl::context> tls = nullptr) {
      auto peer = std::make_shared<Peer>(std::move(tls));
      const asio::ip::tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), 0);
      peer->acceptor.open(endpoint.protocol());
      peer->acceptor.set_option(asio::socket_base::reuse_address(true));
      peer->acceptor.bind(endpoint);
      peer->acceptor.listen();
      peer->port = peer->acceptor.local_endpoint().port();
      if (peer->tls) {
        SSL_CTX_set_tlsext_servername_arg(peer->tls->native_handle(), peer.get());
        SSL_CTX_set_alpn_select_cb(peer->tls->native_handle(), captureAlpn, peer.get());
      }
      spawn(acceptLoop(peer));
      return peer;
    }

    std::string url(const std::string& path = "/") const { return std::string(tls ? "https" : "http") + "://127.0.0.1:" + std::to_string(port) + path; }
    std::string host(const std::string& name, const std::string& path = "/") const { return std::string(tls ? "https" : "http") + "://" + name + ":" + std::to_string(port) + path; }
    // The wires received, with the port replaced by PORT.
    std::vector<std::string> wires() const {
      std::vector<std::string> out;
      for (const PeerRequest& request : requests) out.push_back(scrub(request.wire));
      return out;
    }
    std::string scrub(std::string text) const {
      const std::string number = std::to_string(port);
      for (std::size_t at = text.find(number); at != std::string::npos; at = text.find(number, at + 4)) text.replace(at, number.size(), "PORT");
      return text;
    }
    std::vector<int> connectionIds() const {
      std::vector<int> out;
      for (const PeerRequest& request : requests) out.push_back(request.connection);
      return out;
    }
    std::size_t closedByClient() const {
      std::size_t count = 0;
      for (const auto& connection : connections) count += connection->clientClosed ? 1 : 0;
      return count;
    }
    void stop() {
      boost::system::error_code ignored;
      acceptor.close(ignored);
      for (const auto& connection : connections) connection->close();
    }

    asio::ip::tcp::acceptor acceptor;
    std::shared_ptr<asio::ssl::context> tls;
    unsigned short port = 0;
    Responder respond;
    Opener open;
    std::vector<PeerRequest> requests;
    std::vector<std::shared_ptr<PeerConnection>> connections;
    // TLS: the SNI and ALPN protocols the client offered.
    std::vector<std::string> serverNames;
    std::vector<std::vector<std::string>> alpnOffers;

  private:
    static Task<void> serve(std::shared_ptr<Peer> peer, std::shared_ptr<PeerConnection> connection) {
      if (connection->secure()) {
        auto shaking = connection->handshake();
        const bool secured = co_await std::move(shaking);
        if (!secured) {
          connection->clientClosed = true;
          co_return;
        }
      }
      if (peer->open) {
        auto opening = peer->open(connection);
        const bool reading = co_await std::move(opening);
        if (!reading) co_return;
      }
      std::vector<char> chunk(65536);
      for (;;) {
        auto receiving = connection->readSome(chunk.data(), chunk.size());
        const std::size_t count = co_await std::move(receiving);
        if (count == 0) {
          connection->clientClosed = true;
          co_return;
        }
        connection->buffer.append(chunk.data(), count);
        for (;;) {
          std::optional<PeerRequest> request = takeRequest(*connection);
          if (!request) break;
          peer->requests.push_back(*request);
          if (!peer->respond) continue;
          auto answering = peer->respond(connection, *request);
          co_await std::move(answering);
        }
      }
    }

    static Task<void> acceptLoop(std::shared_ptr<Peer> peer) {
      for (;;) {
        boost::system::error_code error;
        auto token = asio::redirect_error(asio::use_awaitable, error);
        asio::ip::tcp::socket socket = co_await peer->acceptor.async_accept(token);
        if (error) co_return;
        auto connection = std::make_shared<PeerConnection>(static_cast<int>(peer->connections.size()), std::move(socket), peer->tls.get());
        peer->connections.push_back(connection);
        spawn(serve(peer, connection));
      }
    }
  };

  int captureServerName(SSL* ssl, int*, void* argument) {
    auto* peer = static_cast<Peer*>(argument);
    const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (peer) peer->serverNames.push_back(name ? name : "<none>");
    return SSL_TLSEXT_ERR_OK;
  }

  int captureAlpn(SSL*, const unsigned char** out, unsigned char* outLength, const unsigned char* in, unsigned int inLength, void* argument) {
    auto* peer = static_cast<Peer*>(argument);
    std::vector<std::string> offered;
    for (unsigned int index = 0; index < inLength;) {
      const unsigned int length = in[index];
      offered.emplace_back(reinterpret_cast<const char*>(in + index + 1), length);
      index += length + 1;
    }
    if (peer) peer->alpnOffers.push_back(offered);
    if (inLength > 0) {
      *out = in + 1;
      *outLength = in[0];
    }
    return SSL_TLSEXT_ERR_OK;
  }

  std::shared_ptr<asio::ssl::context> serverTls(const std::string& cert, const std::string& key) {
    auto context = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_server);
    context->use_certificate_chain(asio::buffer(cert));
    context->use_private_key(asio::buffer(key), asio::ssl::context::pem);
    SSL_CTX_set_tlsext_servername_callback(context->native_handle(), captureServerName);
    return context;
  }

  // A script step of the peer: bytes to write, a pause, FIN or RST.
  struct Step {
    enum class Kind { write, pause, end, reset } kind = Kind::write;
    std::string bytes;
    double ms = 0;
  };
  Step bytes(std::string text) { return Step{Step::Kind::write, std::move(text), 0}; }
  Step pause(double ms) { return Step{Step::Kind::pause, "", ms}; }
  Step fin() { return Step{Step::Kind::end, "", 0}; }
  Step rst() { return Step{Step::Kind::reset, "", 0}; }

  Task<void> play(std::shared_ptr<PeerConnection> connection, std::vector<Step> steps) {
    for (const Step& step : steps) {
      switch (step.kind) {
        case Step::Kind::write: {
          auto sending = connection->send(step.bytes);
          co_await std::move(sending);
          break;
        }
        case Step::Kind::pause: {
          auto pausing = rt::sleep(step.ms);
          co_await std::move(pausing);
          break;
        }
        case Step::Kind::end: connection->end(); break;
        case Step::Kind::reset: connection->reset(); break;
      }
    }
  }

  // A responder answering every request with the same script.
  Peer::Responder script(std::vector<Step> steps) {
    return [steps](std::shared_ptr<PeerConnection> connection, PeerRequest) { return play(std::move(connection), steps); };
  }
  Peer::Responder answer(std::string response) {
    std::vector<Step> steps{bytes(std::move(response))};
    return script(std::move(steps));
  }
  const std::string kOk = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";

  // ---- the transport under test -------------------------------------------------------

  struct TransportScope {
    std::shared_ptr<FetchTransport> previous = fetchTransport();
    std::shared_ptr<HttpClientTransport> transport;
    explicit TransportScope(HttpClientOptions options) : transport(std::make_shared<HttpClientTransport>(std::move(options))) { setFetchTransport(transport); }
    TransportScope() : TransportScope(HttpClientOptions()) {}
    ~TransportScope() { setFetchTransport(previous); }
    TransportScope(const TransportScope&) = delete;
    TransportScope& operator=(const TransportScope&) = delete;
  };

  std::string codeOf(const Reason& reason) {
    try {
      std::rethrow_exception(reason);
    } catch (const CodedTypeError& error) {
      return error.code;
    } catch (const JsError& error) {
      return error.code.value_or("");
    } catch (...) {
      return "";
    }
  }

  // What fetch gave: the response (status, statusText, headers, body text) or the
  // rejection, and a body read failure.
  struct Outcome {
    std::optional<int> status;
    std::string statusText;
    std::vector<Headers::Entry> headers;
    std::string url;
    bool redirected = false;
    std::optional<std::string> body;
    Reason failure;
    std::string error;
    std::string code;
  };

  Task<Outcome> fetchOutcome(std::string url, RequestInit init) {
    Outcome outcome;
    std::optional<Response> response;
    try {
      auto calling = fetch(RequestInfo(url), std::move(init));
      Response received = co_await std::move(calling);
      response = received;
    } catch (...) {
      outcome.failure = std::current_exception();
    }
    if (outcome.failure) {
      outcome.error = reasonToString(outcome.failure);
      outcome.code = codeOf(outcome.failure);
      co_return outcome;
    }
    outcome.status = response->status();
    outcome.statusText = response->statusText();
    outcome.headers = response->headers().entries();
    outcome.url = response->url();
    outcome.redirected = response->redirected();
    try {
      auto reading = response->text();
      std::string text = co_await std::move(reading);
      outcome.body = std::move(text);
    } catch (...) {
      outcome.failure = std::current_exception();
    }
    if (outcome.failure) {
      outcome.error = reasonToString(outcome.failure);
      outcome.code = codeOf(outcome.failure);
    }
    co_return outcome;
  }

  Task<Outcome> fetchOutcome(std::string url) {
    auto running = fetchOutcome(std::move(url), RequestInit());
    Outcome outcome = co_await std::move(running);
    co_return outcome;
  }

  std::string verbose(const std::string& code, const std::string& url) {
    return "TypeError: " + code + " fetching \"" + url + "\". For more information, pass `verbose: true` in the second argument to fetch()";
  }
  const std::string kClosed = "TypeError: The socket connection was closed unexpectedly. For more information, pass `verbose: true` in the second argument to fetch()";

  std::string latin1(std::string_view bytes) {
    std::string out;
    for (const char byte : bytes) appendCodePoint(out, static_cast<unsigned char>(byte));
    return out;
  }

  std::string hexBytes(std::string_view hex) {
    const Bytes decoded = fromHex(hex);
    return std::string(decoded.begin(), decoded.end());
  }

  std::string describe(const std::vector<Headers::Entry>& entries) {
    std::string out;
    for (const auto& [name, value] : entries) out += name + ": " + value + "\n";
    return out;
  }

  const std::string kDefaults = "Connection: keep-alive\r\nUser-Agent: Bun/1.4.0\r\nAccept: */*\r\nHost: 127.0.0.1:PORT\r\nAccept-Encoding: gzip, deflate, br, zstd\r\n";
}

// ---- request heads ----------------------------------------------------------------------

ASYNC_TEST("request heads match Bun byte for byte") {
  auto peer = Peer::start();
  peer->respond = answer(kOk);
  TransportScope scope;
  struct Case {
    std::string path;
    RequestInit init;
    std::string expected;
  };
  std::vector<Case> cases;
  cases.push_back({"/a?b=1#frag", RequestInit(), "GET /a?b=1 HTTP/1.1\r\n" + kDefaults + "\r\n"});
  cases.push_back({"/h", RequestInit{.method = "HEAD"}, "HEAD /h HTTP/1.1\r\n" + kDefaults + "\r\n"});
  cases.push_back({"/o", RequestInit{.method = "OPTIONS"}, "OPTIONS /o HTTP/1.1\r\n" + kDefaults + "Content-Length: 0\r\n\r\n"});
  cases.push_back({"/p", RequestInit{.method = "POST"}, "POST /p HTTP/1.1\r\n" + kDefaults + "Content-Length: 0\r\n\r\n"});
  cases.push_back({"/p", RequestInit{.method = "POST", .body = BodyInit(std::string("hello"))}, "POST /p HTTP/1.1\r\n" + kDefaults + "Content-Length: 5\r\n\r\nhello"});
  cases.push_back({"/p", RequestInit{.method = "POST", .body = BodyInit(std::string(""))}, "POST /p HTTP/1.1\r\n" + kDefaults + "Content-Length: 0\r\n\r\n"});
  cases.push_back({"/p", RequestInit{.method = "POST", .body = BodyInit(Bytes{1, 2, 3})}, "POST /p HTTP/1.1\r\n" + kDefaults + "Content-Length: 3\r\n\r\n\x01\x02\x03"});
  cases.push_back({"/p", RequestInit{.method = "POST", .headers = Headers{{"Content-Type", "text/x-custom"}}, .body = BodyInit(Bytes{'b', 'l', 'o', 'b'})},
                   "POST /p HTTP/1.1\r\nContent-Type: text/x-custom\r\n" + kDefaults + "Content-Length: 4\r\n\r\nblob"});
  cases.push_back({"/p", RequestInit{.method = "POST", .body = BodyInit(URLSearchParams("a=1"))},
                   "POST /p HTTP/1.1\r\nContent-Type: application/x-www-form-urlencoded;charset=UTF-8\r\n" + kDefaults + "Content-Length: 3\r\n\r\na=1"});
  cases.push_back({"/d", RequestInit{.method = "DELETE"}, "DELETE /d HTTP/1.1\r\n" + kDefaults + "Content-Length: 0\r\n\r\n"});
  cases.push_back({"/d", RequestInit{.method = "DELETE", .body = BodyInit(std::string("gone"))}, "DELETE /d HTTP/1.1\r\n" + kDefaults + "Content-Length: 4\r\n\r\ngone"});
  cases.push_back({"/d", RequestInit{.method = "PATCH", .body = BodyInit(std::string("x"))}, "PATCH /d HTTP/1.1\r\n" + kDefaults + "Content-Length: 1\r\n\r\nx"});
  cases.push_back({"/m", RequestInit{.method = "PROPFIND"}, "PROPFIND /m HTTP/1.1\r\n" + kDefaults + "Content-Length: 0\r\n\r\n"});
  cases.push_back({"/m", RequestInit{.method = "TRACE"}, "TRACE /m HTTP/1.1\r\n" + kDefaults + "\r\n"});
  cases.push_back({"/m", RequestInit{.method = "post"}, "POST /m HTTP/1.1\r\n" + kDefaults + "Content-Length: 0\r\n\r\n"});
  // Common header names in WebKit's spelling, sorted; lowercase names as given; duplicates joined.
  cases.push_back({"/lc",
                   RequestInit{.method = "POST",
                               .headers = Headers{{"x-b", "1"}, {"Content-Type", "application/json"}, {"x-a", "2"}, {"authorization", "Bearer t"}, {"accept", "text/event-stream"}, {"x-b", "4"}, {"originator", "codex_cli_rs"}, {"openai-beta", "responses=experimental"}},
                               .body = BodyInit(std::string("{}"))},
                   "POST /lc HTTP/1.1\r\nAccept: text/event-stream\r\nAuthorization: Bearer t\r\nContent-Type: application/json\r\nopenai-beta: responses=experimental\r\noriginator: codex_cli_rs\r\nx-a: 2\r\nx-b: 1, 4\r\nConnection: keep-alive\r\nUser-Agent: Bun/1.4.0\r\nHost: 127.0.0.1:PORT\r\nAccept-Encoding: gzip, deflate, br, zstd\r\nContent-Length: 2\r\n\r\n{}"});
  // The request's own Connection, User-Agent, Accept, Host and Accept-Encoding replace the defaults.
  cases.push_back({"/o", RequestInit{.headers = Headers{{"User-Agent", "mine/1"}, {"Accept", "text/plain"}, {"Accept-Encoding", "identity"}, {"Connection", "close"}, {"Host", "example.test"}}},
                   "GET /o HTTP/1.1\r\nAccept: text/plain\r\nAccept-Encoding: identity\r\nConnection: close\r\nHost: example.test\r\nUser-Agent: mine/1\r\n\r\n"});
  // Set-Cookie values come last, one line each.
  Headers cookies;
  cookies.append("Set-Cookie", "a=1");
  cookies.append("Set-Cookie", "b=2");
  cookies.append("Cookie", "c=3");
  cookies.append("x-z", "z");
  cases.push_back({"/sc", RequestInit{.headers = cookies}, "GET /sc HTTP/1.1\r\nCookie: c=3\r\nx-z: z\r\nset-cookie: a=1\r\nset-cookie: b=2\r\n" + kDefaults + "\r\n"});
  // Content-Length and Transfer-Encoding of the request are Bun's own; Expect is sent as is.
  cases.push_back({"/cl", RequestInit{.method = "POST", .headers = Headers{{"Content-Length", "3"}}, .body = BodyInit(std::string("abc"))},
                   "POST /cl HTTP/1.1\r\n" + kDefaults + "Content-Length: 3\r\n\r\nabc"});
  cases.push_back({"/te", RequestInit{.method = "POST", .headers = Headers{{"Transfer-Encoding", "chunked"}}, .body = BodyInit(std::string("abc"))},
                   "POST /te HTTP/1.1\r\n" + kDefaults + "Content-Length: 3\r\n\r\nabc"});
  cases.push_back({"/ex", RequestInit{.method = "POST", .headers = Headers{{"Expect", "100-continue"}}, .body = BodyInit(std::string("abc"))},
                   "POST /ex HTTP/1.1\r\nExpect: 100-continue\r\n" + kDefaults + "Content-Length: 3\r\n\r\nabc"});
  cases.push_back({"/l", RequestInit{.headers = Headers{{"x-l", "caf\xc3\xa9"}}}, "GET /l HTTP/1.1\r\nx-l: caf\xe9\r\n" + kDefaults + "\r\n"});
  cases.push_back({"/p\xc3\xa4th/\xe2\x82\xac?q=\xc3\xbc", RequestInit(), "GET /p%C3%A4th/%E2%82%AC?q=%C3%BC HTTP/1.1\r\n" + kDefaults + "\r\n"});
  for (Case& item : cases) {
    const std::size_t before = peer->requests.size();
    auto running = fetchOutcome(peer->url(item.path), std::move(item.init));
    const Outcome outcome = co_await std::move(running);
    EXPECT_EQ(outcome.status, std::optional<int>(200));
    ASSERT_EQ(peer->requests.size(), before + 1);
    EXPECT_EQ(peer->scrub(peer->requests.back().wire), item.expected);
  }
  // userinfo is not sent.
  auto userinfo = fetchOutcome("http://user:pa%20ss@127.0.0.1:" + std::to_string(peer->port) + "/ui");
  const Outcome withUserinfo = co_await std::move(userinfo);
  EXPECT_EQ(withUserinfo.status, std::optional<int>(200));
  EXPECT_EQ(peer->scrub(peer->requests.back().wire), "GET /ui HTTP/1.1\r\n" + kDefaults + "\r\n");
  peer->stop();
}

ASYNC_TEST("Host omits the default port and keeps names as the URL spells them") {
  auto peer = Peer::start();
  peer->respond = answer(kOk);
  TransportScope scope;
  auto running = fetchOutcome(peer->host("LOCALHOST", "/lh"));
  const Outcome outcome = co_await std::move(running);
  EXPECT_EQ(outcome.status, std::optional<int>(200));
  EXPECT_EQ(peer->scrub(peer->requests.back().wire), "GET /lh HTTP/1.1\r\nConnection: keep-alive\r\nUser-Agent: Bun/1.4.0\r\nAccept: */*\r\nHost: localhost:PORT\r\nAccept-Encoding: gzip, deflate, br, zstd\r\n\r\n");
  peer->stop();
}

ASYNC_TEST("the User-Agent default is a transport option") {
  auto peer = Peer::start();
  peer->respond = answer(kOk);
  HttpClientOptions options;
  options.userAgent = "ai.ubq.fi-gateway/1";
  TransportScope scope(options);
  auto running = fetchOutcome(peer->url("/ua"));
  const Outcome outcome = co_await std::move(running);
  EXPECT_EQ(outcome.status, std::optional<int>(200));
  EXPECT(peer->requests.back().wire.find("\r\nUser-Agent: ai.ubq.fi-gateway/1\r\n") != std::string::npos);
  peer->stop();
}

ASYNC_TEST("names are written as the caller spelled them, sorted among the common names") {
  // Bun 1.4 (wire4.ts "/ho2"): the uncommon name keeps its spelling and sorts by it in byte order.
  auto peer = Peer::start();
  peer->respond = answer(kOk);
  TransportScope scope;
  RequestInit init{.headers = Headers{{"originator", "codex"}, {"ChatGPT-Account-ID", "acc"}, {"user-agent", "ua/1"}, {"Accept", "application/json"}, {"Authorization", "Bearer t"}}};
  auto running = fetchOutcome(peer->url("/ho2"), std::move(init));
  const Outcome outcome = co_await std::move(running);
  EXPECT_EQ(outcome.status, std::optional<int>(200));
  EXPECT_EQ(peer->scrub(peer->requests.back().wire),
            "GET /ho2 HTTP/1.1\r\nAccept: application/json\r\nAuthorization: Bearer t\r\nChatGPT-Account-ID: acc\r\nUser-Agent: ua/1\r\noriginator: codex\r\nConnection: keep-alive\r\nHost: 127.0.0.1:PORT\r\nAccept-Encoding: gzip, deflate, br, zstd\r\n\r\n");
  peer->stop();
}

namespace {
  // fetch(input, init) and read the body (the Request-input paths of the spelling test).
  Task<void> fetchAndRead(RequestInfo input, RequestInit init) {
    auto calling = fetch(std::move(input), std::move(init));
    Response response = co_await std::move(calling);
    auto reading = response.text();
    co_await std::move(reading);
  }
}

ASYNC_TEST("request header names keep the caller's first spelling and Cookie values join with \"; \", as Bun writes them") {
  // Every expected head was recorded from Bun 1.4.0's fetch on a raw loopback listener
  // (wire4.ts, same scenario per path; JS records become Headers{...} in key order, entry lists
  // Headers::fromSequence or the vector constructor).
  auto peer = Peer::start();
  peer->respond = answer(kOk);
  TransportScope scope;
  struct Case {
    std::string path;
    RequestInit init;
    std::string expected;
  };
  std::vector<Case> cases;
  // Mixed-case custom names, common names in any case (written in Bun's spelling), sorted in
  // byte order of the written names.
  cases.push_back({"/s1",
                   RequestInit{.headers = Headers{{"ChatGPT-Account-ID", "a"}, {"x-lower", "b"}, {"X-Upper", "c"}, {"content-type", "text/plain"}, {"AUTHORIZATION", "Bearer t"}, {"originator", "o"}}},
                   "GET /s1 HTTP/1.1\r\nAuthorization: Bearer t\r\nChatGPT-Account-ID: a\r\nContent-Type: text/plain\r\nX-Upper: c\r\noriginator: o\r\nx-lower: b\r\n" + kDefaults + "\r\n"});
  // set() three times in three spellings: the first spelling stays.
  Headers setThrice;
  setThrice.set("X-Custom-Name", "1");
  setThrice.set("x-custom-name", "2");
  setThrice.set("X-CUSTOM-NAME", "3");
  cases.push_back({"/s2", RequestInit{.headers = setThrice}, "GET /s2 HTTP/1.1\r\nX-Custom-Name: 3\r\n" + kDefaults + "\r\n"});
  // append() twice in two spellings.
  Headers appendTwice;
  appendTwice.append("x-multi", "1");
  appendTwice.append("X-Multi", "2");
  cases.push_back({"/s3", RequestInit{.headers = appendTwice}, "GET /s3 HTTP/1.1\r\nx-multi: 1, 2\r\n" + kDefaults + "\r\n"});
  // set() after append() keeps the appended spelling.
  Headers appendThenSet;
  appendThenSet.append("x-a", "1");
  appendThenSet.set("X-A", "2");
  cases.push_back({"/s4", RequestInit{.headers = appendThenSet}, "GET /s4 HTTP/1.1\r\nx-a: 2\r\n" + kDefaults + "\r\n"});
  // After a delete the next set() or append() spells it anew.
  Headers deleted;
  deleted.set("x-d", "1");
  deleted.delete_("X-D");
  deleted.set("X-D", "2");
  deleted.set("X-E", "1");
  deleted.delete_("x-e");
  deleted.append("x-E", "2");
  cases.push_back({"/s5", RequestInit{.headers = deleted}, "GET /s5 HTTP/1.1\r\nX-D: 2\r\nx-E: 2\r\n" + kDefaults + "\r\n"});
  // new Headers(other): the copy keeps the spellings; later changes to the source do not reach it.
  Headers source{{"My-Header", "1"}, {"other", "2"}};
  Headers copied(source);
  copied.append("my-header", "3");
  source.set("OTHER", "changed");
  cases.push_back({"/s6", RequestInit{.headers = copied}, "GET /s6 HTTP/1.1\r\nMy-Header: 1, 3\r\nother: 2\r\n" + kDefaults + "\r\n"});
  // An entry list.
  cases.push_back({"/s7", RequestInit{.headers = Headers::fromSequence({{"Zeta-Last", "z"}, {"alpha-first", "a"}, {"Beta", "b"}, {"zeta-last", "z2"}})},
                   "GET /s7 HTTP/1.1\r\nBeta: b\r\nZeta-Last: z, z2\r\nalpha-first: a\r\n" + kDefaults + "\r\n"});
  // Cookie: appended values join with "; ", also after set(), and inside a value ", " stays.
  Headers cookies;
  cookies.append("Cookie", "a=1");
  cookies.append("cookie", "b=2");
  cases.push_back({"/s11", RequestInit{.headers = cookies}, "GET /s11 HTTP/1.1\r\nCookie: a=1; b=2\r\n" + kDefaults + "\r\n"});
  Headers cookieSet{{"cookie", "a=1"}};
  cookieSet.set("Cookie", "c=3");
  cookieSet.append("COOKIE", "d=4");
  cases.push_back({"/s12", RequestInit{.headers = cookieSet}, "GET /s12 HTTP/1.1\r\nCookie: c=3; d=4\r\n" + kDefaults + "\r\n"});
  Headers cookieComma{{"cookie", "a=1, b=2"}};
  cookieComma.append("Cookie", "c=3");
  cookieComma.append("x-z", "z");
  cases.push_back({"/s13", RequestInit{.headers = cookieComma}, "GET /s13 HTTP/1.1\r\nCookie: a=1, b=2; c=3\r\nx-z: z\r\n" + kDefaults + "\r\n"});
  // Bun's spelling of the common names (four differ from WebKit's: Icy-Metaint, Last-Event-Id,
  // Sourcemap, X-Sourcemap), whatever the caller's case.
  cases.push_back({"/s14",
                   RequestInit{.headers = Headers{{"accept", "x"}, {"cache-control", "no"}, {"if-none-match", "e"}, {"user-agent", "ua"}, {"dnt", "1"}, {"etag", "t"}, {"te", "trailers"},
                                                  {"x-frame-options", "d"}, {"last-event-id", "7"}, {"icy-metaint", "1"}, {"sec-websocket-key", "k"}, {"sourcemap", "s"}, {"x-sourcemap", "x"}}},
                   "GET /s14 HTTP/1.1\r\nAccept: x\r\nCache-Control: no\r\nDNT: 1\r\nETag: t\r\nIcy-Metaint: 1\r\nIf-None-Match: e\r\nLast-Event-Id: 7\r\nSec-WebSocket-Key: k\r\nSourcemap: s\r\nTE: trailers\r\nUser-Agent: ua\r\nX-Frame-Options: d\r\nX-Sourcemap: x\r\nConnection: keep-alive\r\nHost: 127.0.0.1:PORT\r\nAccept-Encoding: gzip, deflate, br, zstd\r\n\r\n"});
  cases.push_back({"/s15",
                   RequestInit{.headers = Headers{{"Icy-MetaInt", "1"}, {"Last-Event-ID", "2"}, {"SourceMap", "3"}, {"X-SourceMap", "4"}, {"ETAG", "5"}, {"dNt", "6"}, {"CONTENT-type", "t"}}},
                   "GET /s15 HTTP/1.1\r\nContent-Type: t\r\nDNT: 6\r\nETag: 5\r\nIcy-Metaint: 1\r\nLast-Event-Id: 2\r\nSourcemap: 3\r\nX-Sourcemap: 4\r\n" + kDefaults + "\r\n"});
  // Byte order of the written names: digits, capitals, "_", lowercase, "~" ("ETag" before "Eb").
  cases.push_back({"/s16",
                   RequestInit{.headers = Headers{{"b-lower", "1"}, {"A-Upper", "2"}, {"_under", "3"}, {"Zed", "4"}, {"accept-encoding", "identity"}, {"a-lower", "5"}, {"~tilde", "6"}, {"0digit", "7"}, {"Eb", "8"}, {"etag", "9"}}},
                   "GET /s16 HTTP/1.1\r\n0digit: 7\r\nA-Upper: 2\r\nAccept-Encoding: identity\r\nETag: 9\r\nEb: 8\r\nZed: 4\r\n_under: 3\r\na-lower: 5\r\nb-lower: 1\r\n~tilde: 6\r\nConnection: keep-alive\r\nUser-Agent: Bun/1.4.0\r\nAccept: */*\r\nHost: 127.0.0.1:PORT\r\n\r\n"});
  // Set-Cookie values stay lowercase lines after the others.
  Headers setCookies;
  setCookies.append("Set-Cookie", "s=1");
  setCookies.append("set-cookie", "t=2");
  setCookies.append("X-After", "1");
  cases.push_back({"/s17", RequestInit{.headers = setCookies}, "GET /s17 HTTP/1.1\r\nX-After: 1\r\nset-cookie: s=1\r\nset-cookie: t=2\r\n" + kDefaults + "\r\n"});
  cases.push_back({"/s18", RequestInit{.method = "POST", .headers = Headers{{"Content-Type", "application/json"}, {"X-Api-Key", "k"}}, .body = BodyInit(std::string("{}"))},
                   "POST /s18 HTTP/1.1\r\nContent-Type: application/json\r\nX-Api-Key: k\r\n" + kDefaults + "Content-Length: 2\r\n\r\n{}"});
  for (Case& item : cases) {
    const std::size_t before = peer->requests.size();
    auto running = fetchOutcome(peer->url(item.path), std::move(item.init));
    const Outcome outcome = co_await std::move(running);
    EXPECT_EQ(outcome.status, std::optional<int>(200));
    ASSERT_EQ(peer->requests.size(), before + 1);
    EXPECT_EQ(peer->scrub(peer->requests.back().wire), item.expected);
  }
  // Request(init.headers): a record (its Accept replaces the default), a Request copied from a
  // Request, and fetch(request, { headers }) whose init headers replace the request's.
  RequestInit recordInit{.headers = Headers{{"X-Req-Case", "1"}, {"accept", "text/html"}}};
  const Request fromRecord(peer->url("/s8"), std::move(recordInit));
  auto first = fetchAndRead(RequestInfo(fromRecord), RequestInit());
  co_await std::move(first);
  EXPECT_EQ(peer->scrub(peer->requests.back().wire), "GET /s8 HTTP/1.1\r\nAccept: text/html\r\nX-Req-Case: 1\r\nConnection: keep-alive\r\nUser-Agent: Bun/1.4.0\r\nHost: 127.0.0.1:PORT\r\nAccept-Encoding: gzip, deflate, br, zstd\r\n\r\n");
  RequestInit innerInit{.headers = Headers{{"X-Copied", "1"}}};
  const Request inner(peer->url("/s9"), std::move(innerInit));
  const Request outer(inner, RequestInit());
  auto second = fetchAndRead(RequestInfo(outer), RequestInit());
  co_await std::move(second);
  EXPECT_EQ(peer->scrub(peer->requests.back().wire), "GET /s9 HTTP/1.1\r\nX-Copied: 1\r\n" + kDefaults + "\r\n");
  RequestInit requestInit{.headers = Headers{{"X-From-Request", "1"}}};
  const Request withHeaders(peer->url("/s10"), std::move(requestInit));
  RequestInit replacing{.headers = Headers{{"X-From-Init", "2"}}};
  auto third = fetchAndRead(RequestInfo(withHeaders), std::move(replacing));
  co_await std::move(third);
  EXPECT_EQ(peer->scrub(peer->requests.back().wire), "GET /s10 HTTP/1.1\r\nX-From-Init: 2\r\n" + kDefaults + "\r\n");
  peer->stop();
}

ASYNC_TEST("stream bodies are sent chunked, one HTTP chunk per stream chunk") {
  auto peer = Peer::start();
  peer->respond = answer(kOk);
  TransportScope scope;
  UnderlyingSource<ByteChunk> source;
  source.start = [](ReadableStreamDefaultController<ByteChunk> controller) {
    controller.enqueue(ByteChunk(300, 'a'));
    controller.enqueue(ByteChunk());
    controller.enqueue(ByteChunk(17, 'b'));
    controller.close();
  };
  RequestInit init{.method = "PUT", .body = BodyInit(ByteStream(std::move(source))), .duplex = "half"};
  auto running = fetchOutcome(peer->url("/big"), std::move(init));
  const Outcome outcome = co_await std::move(running);
  EXPECT_EQ(outcome.status, std::optional<int>(200));
  EXPECT_EQ(peer->scrub(peer->requests.back().wire), "PUT /big HTTP/1.1\r\n" + kDefaults + "Transfer-Encoding: chunked\r\n\r\n12c\r\n" + std::string(300, 'a') + "\r\n11\r\n" + std::string(17, 'b') + "\r\n0\r\n\r\n");
  UnderlyingSource<ByteChunk> empty;
  empty.start = [](ReadableStreamDefaultController<ByteChunk> controller) { controller.close(); };
  RequestInit emptyInit{.method = "POST", .body = BodyInit(ByteStream(std::move(empty))), .duplex = "half"};
  auto emptyRunning = fetchOutcome(peer->url("/s"), std::move(emptyInit));
  const Outcome emptyOutcome = co_await std::move(emptyRunning);
  EXPECT_EQ(emptyOutcome.status, std::optional<int>(200));
  EXPECT_EQ(peer->scrub(peer->requests.back().wire), "POST /s HTTP/1.1\r\n" + kDefaults + "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
  // A slow stream: each chunk goes out as it is produced.
  auto produced = std::make_shared<int>(0);
  UnderlyingSource<ByteChunk> slow;
  slow.pull = [produced](ReadableStreamDefaultController<ByteChunk> controller) -> Task<void> {
    auto waiting = rt::sleep(30);
    co_await std::move(waiting);
    if (*produced == 1) {
      controller.close();
      co_return;
    }
    *produced = 1;
    controller.enqueue(ByteChunk{'s', 'l', 'o', 'w'});
  };
  RequestInit slowInit{.method = "POST", .body = BodyInit(ByteStream(std::move(slow))), .duplex = "half"};
  auto slowRunning = fetchOutcome(peer->url("/s"), std::move(slowInit));
  const Outcome slowOutcome = co_await std::move(slowRunning);
  EXPECT_EQ(slowOutcome.status, std::optional<int>(200));
  EXPECT_EQ(peer->scrub(peer->requests.back().wire), "POST /s HTTP/1.1\r\n" + kDefaults + "Transfer-Encoding: chunked\r\n\r\n4\r\nslow\r\n0\r\n\r\n");
  peer->stop();
}

// ---- responses ------------------------------------------------------------------------------

namespace {
  struct ResponseCase {
    std::string name;
    std::vector<Step> steps;
    std::string method = "GET";
    std::optional<int> status;
    std::string statusText;
    std::vector<Headers::Entry> headers;
    std::optional<std::string> body;
    // FetchError code for a rejected head or failed body read.
    std::string code;
  };

  ResponseCase ok(std::string name, std::string response, int status, std::string statusText, std::vector<Headers::Entry> headers, std::string body) {
    ResponseCase item;
    item.name = std::move(name);
    item.steps.push_back(bytes(std::move(response)));
    item.status = status;
    item.statusText = std::move(statusText);
    item.headers = std::move(headers);
    item.body = std::move(body);
    return item;
  }

  ResponseCase failing(std::string name, std::vector<Step> steps, std::string code) {
    ResponseCase item;
    item.name = std::move(name);
    item.steps = std::move(steps);
    item.code = std::move(code);
    return item;
  }

  ResponseCase malformed(std::string name, std::string response) {
    std::vector<Step> steps{bytes(std::move(response)), fin()};
    return failing(std::move(name), std::move(steps), "Malformed_HTTP_Response");
  }

  std::string manyFields(std::size_t count) {
    std::string fields;
    for (std::size_t index = 0; index + 1 < count; ++index) fields += "X-H" + std::to_string(index) + ": v\r\n";
    return "HTTP/1.1 200 OK\r\n" + fields + "Content-Length: 2\r\n\r\nok";
  }

  std::vector<ResponseCase> responseCases() {
    std::vector<ResponseCase> cases;
    cases.push_back(ok("duplicates join, Set-Cookie stays a list", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX-A: 1\r\nx-a: 2\r\nSet-Cookie: a=1\r\nSet-Cookie: b=2\r\n\r\nhello", 200, "OK",
                       {{"content-length", "5"}, {"x-a", "1, 2"}, {"set-cookie", "a=1"}, {"set-cookie", "b=2"}}, "hello"));
    cases.push_back(ok("no reason phrase", "HTTP/1.1 201\r\nContent-Length: 0\r\n\r\n", 201, "", {{"content-length", "0"}}, ""));
    cases.push_back(ok("empty reason phrase", "HTTP/1.1 202 \r\nContent-Length: 0\r\n\r\n", 202, "", {{"content-length", "0"}}, ""));
    cases.push_back(ok("statusText is UTF-8, values Latin-1", "HTTP/1.1 200 Caf\xe9 OK\r\nX-V: caf\xe9\r\nContent-Length: 0\r\n\r\n", 200, "Caf\xef\xbf\xbd OK",
                       {{"content-length", "0"}, {"x-v", "caf\xc3\xa9"}}, ""));
    cases.push_back(ok("UTF-8 reason, UTF-8 bytes in a value", "HTTP/1.1 200 Gr\xc3\xbc\xc3\x9f" "e \xe2\x82\xac\r\nX-U: \xc3\xa9\r\nContent-Length: 2\r\n\r\nok", 200,
                       "Gr\xc3\xbc\xc3\x9f" "e \xe2\x82\xac", {{"content-length", "2"}, {"x-u", latin1("\xc3\xa9")}}, "ok"));
    cases.push_back(ok("tab in the reason", "HTTP/1.1 200 A\tB\r\nContent-Length: 2\r\n\r\nok", 200, "A\tB", {{"content-length", "2"}}, "ok"));
    cases.push_back(malformed("two spaces before the status", "HTTP/1.1  200 OK\r\nContent-Length: 2\r\n\r\nok"));
    cases.push_back(malformed("HTTP/1.2", "HTTP/1.2 200 OK\r\nContent-Length: 2\r\n\r\nok"));
    cases.push_back(malformed("LF line ends", "HTTP/1.1 200 OK\nContent-Length: 2\nX-L: 1\n\nok"));
    cases.push_back(ok("values are trimmed, empty values kept", "HTTP/1.1 200 OK\r\nX-S:   padded value   \r\nX-E:\r\nContent-Length: 0\r\n\r\n", 200, "OK",
                       {{"content-length", "0"}, {"x-e", ""}, {"x-s", "padded value"}}, ""));
    {
      ResponseCase item = ok("HTTP/1.0 body runs until close", "HTTP/1.0 200 OK\r\n\r\nuntil-close", 200, "OK", {}, "until-close");
      item.steps.push_back(fin());
      cases.push_back(item);
      ResponseCase eleven = ok("HTTP/1.1 body without framing runs until close", "HTTP/1.1 200 OK\r\n\r\nuntil-close-11", 200, "OK", {}, "until-close-11");
      eleven.steps.push_back(fin());
      cases.push_back(eleven);
    }
    cases.push_back(ok("chunked with extensions and trailers", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\nTrailer-X: t\r\n\r\n", 200, "OK",
                       {{"transfer-encoding", "chunked"}}, "hello world"));
    cases.push_back(malformed("chunked and Content-Length", "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nwxyz\r\n0\r\n\r\n"));
    cases.push_back(ok("100 Continue is skipped", "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", 200, "OK", {{"content-length", "2"}}, "ok"));
    cases.push_back(ok("103 Early Hints is skipped", "HTTP/1.1 103 Early Hints\r\nLink: </a>\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", 200, "OK", {{"content-length", "2"}}, "ok"));
    cases.push_back(ok("199 is skipped", "HTTP/1.1 199 Odd\r\nContent-Length: 0\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 200, "OK", {{"content-length", "0"}}, ""));
    cases.push_back(ok("204 has no body whatever Content-Length says", "HTTP/1.1 204 No Content\r\nContent-Length: 5\r\n\r\n", 204, "No Content", {{"content-length", "5"}}, ""));
    cases.push_back(ok("304 has no body", "HTTP/1.1 304 Not Modified\r\nETag: x\r\n\r\n", 304, "Not Modified", {{"etag", "x"}}, ""));
    {
      ResponseCase head = ok("HEAD has no body", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n", 200, "OK", {{"content-length", "5"}}, "");
      head.method = "HEAD";
      cases.push_back(head);
      ResponseCase trace = ok("TRACE can return a body", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\ntrace", 200, "OK", {{"content-length", "5"}}, "trace");
      trace.method = "TRACE";
      cases.push_back(trace);
    }
    cases.push_back(ok("Content-Length with spaces", "HTTP/1.1 200 OK\r\nContent-Length:  2 \r\n\r\nok", 200, "OK", {{"content-length", "2"}}, "ok"));
    cases.push_back(malformed("equal Content-Length twice", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nok"));
    cases.push_back(ok("Transfer-Encoding gzip, chunked: chunked only", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n2\r\nab\r\n0\r\n\r\n", 200, "OK",
                       {{"transfer-encoding", "gzip, chunked"}}, "ab"));
    cases.push_back(malformed("Transfer-Encoding identity with a length", "HTTP/1.1 200 OK\r\nTransfer-Encoding: identity\r\nContent-Length: 2\r\n\r\nab"));
    cases.push_back(ok("Transfer-Encoding CHUNKED", "HTTP/1.1 200 OK\r\nTransfer-Encoding: CHUNKED\r\n\r\n2\r\nok\r\n0\r\n\r\n", 200, "OK", {{"transfer-encoding", "CHUNKED"}}, "ok"));
    cases.push_back(malformed("Transfer-Encoding gzip with a length", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\nContent-Length: 2\r\n\r\nok"));
    cases.push_back(ok("two Transfer-Encoding fields", "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\n\r\n", 200, "OK",
                       {{"transfer-encoding", "gzip, chunked"}}, "ok"));
    {
      ResponseCase item = ok("Transfer-Encoding identity without a length", "HTTP/1.1 200 OK\r\nTransfer-Encoding: identity\r\n\r\nuntil-close", 200, "OK", {{"transfer-encoding", "identity"}}, "until-close");
      item.steps.push_back(fin());
      cases.push_back(item);
    }
    for (const auto& [name, chunks] : std::vector<std::pair<std::string, std::string>>{{"quoted chunk extension", "3;name=\"v;x\"\r\nabc\r\n0\r\n\r\n"},
                                                                                       {"leading zeros", "0003\r\nabc\r\n0\r\n\r\n"},
                                                                                       {"trailers", "3\r\nabc\r\n0\r\nX-T: 1\r\nY: 2\r\n\r\n"}}) {
      cases.push_back(ok(name, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + chunks, 200, "OK", {{"transfer-encoding", "chunked"}}, "abc"));
    }
    cases.push_back(ok("upper-case hex", "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nA\r\n0123456789\r\n0\r\n\r\n", 200, "OK", {{"transfer-encoding", "chunked"}}, "0123456789"));
    cases.push_back(ok("256 fields", manyFields(256), 200, "OK", {}, "ok"));
    cases.push_back(ok("a 1 MiB field", "HTTP/1.1 200 OK\r\nX-Big: " + std::string(1 << 20, 'a') + "\r\nContent-Length: 2\r\n\r\nok", 200, "OK", {}, "ok"));
    // Failures before a response.
    {
      std::vector<Step> closeAtOnce{fin()};
      ResponseCase item;
      item.name = "connection closed before the head";
      item.steps = closeAtOnce;
      item.code = "ECONNRESET";
      cases.push_back(item);
    }
    cases.push_back(malformed("garbage status line", "FOO/1.1 200 OK\r\n\r\n"));
    cases.push_back(malformed("letters in the status", "HTTP/1.1 2x0 OK\r\nContent-Length: 0\r\n\r\n"));
    cases.push_back(malformed("four-digit status", "HTTP/1.1 2000 OK\r\nContent-Length: 2\r\n\r\nok"));
    cases.push_back(malformed("lower-case http", "http/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"));
    cases.push_back(malformed("HTTP/2 status line", "HTTP/2 200 OK\r\nContent-Length: 2\r\n\r\nab"));
    cases.push_back(malformed("field without colon", "HTTP/1.1 200 OK\r\nBadHeader\r\nContent-Length: 0\r\n\r\n"));
    cases.push_back(malformed("obs-fold", "HTTP/1.1 200 OK\r\nX-F: a\r\n b\r\nContent-Length: 0\r\n\r\n"));
    cases.push_back(malformed("space in a name", "HTTP/1.1 200 OK\r\nX Bad: 1\r\nContent-Length: 0\r\n\r\n"));
    cases.push_back(malformed("space before the colon", "HTTP/1.1 200 OK\r\nX-A : 1\r\nContent-Length: 2\r\n\r\nok"));
    cases.push_back(malformed("empty name", "HTTP/1.1 200 OK\r\n: v\r\nContent-Length: 2\r\n\r\nok"));
    cases.push_back(malformed("control byte in a value", "HTTP/1.1 200 OK\r\nX-C: a\x01" "b\r\nContent-Length: 2\r\n\r\nok"));
    cases.push_back(malformed("DEL in a value", "HTTP/1.1 200 OK\r\nX-C: a\x7f" "b\r\nContent-Length: 2\r\n\r\nok"));
    cases.push_back(malformed("NUL in a value", std::string("HTTP/1.1 200 OK\r\nX-C: a\0b\r\nContent-Length: 2\r\n\r\nok", 50)));
    cases.push_back(malformed("257 fields", manyFields(257)));
    for (const auto& [name, field] : std::vector<std::pair<std::string, std::string>>{{"Content-Length abc", "Content-Length: abc"},
                                                                                     {"Content-Length +2", "Content-Length: +2"},
                                                                                     {"Content-Length -1", "Content-Length: -1"},
                                                                                     {"Content-Length list", "Content-Length: 2, 2"},
                                                                                     {"Content-Length overflow", "Content-Length: 99999999999999999999999"},
                                                                                     {"different Content-Lengths", "Content-Length: 2\r\nContent-Length: 3"}}) {
      std::vector<Step> steps{bytes("HTTP/1.1 200 OK\r\n" + field + "\r\n\r\nzz"), fin()};
      cases.push_back(failing(name, steps, "Malformed_HTTP_Response"));
    }
    {
      std::vector<Step> upgrade{bytes("HTTP/1.1 101 Switching Protocols\r\nUpgrade: x\r\nConnection: upgrade\r\n\r\n"), fin()};
      cases.push_back(failing("101 without an upgrade request", upgrade, "UnrequestedUpgrade"));
      std::vector<Step> late{bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip\r\n\r\n2\r\nok\r\n0\r\n\r\n"), fin()};
      cases.push_back(failing("chunked not last", late, "UnsupportedTransferEncoding"));
      std::vector<Step> twice{bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, chunked\r\n\r\n2\r\nok\r\n0\r\n\r\n"), fin()};
      cases.push_back(failing("chunked twice", twice, "UnsupportedTransferEncoding"));
      std::vector<Step> huge{bytes("HTTP/1.1 200 OK\r\nX-Big: " + std::string(2 << 20, 'a') + "\r\nContent-Length: 2\r\n\r\nok")};
      cases.push_back(failing("a 2 MiB head", huge, "ResponseHeadersTooLarge"));
    }
    // Failures inside the body (the response resolved first).
    for (const auto& [name, chunks] : std::vector<std::pair<std::string, std::string>>{{"chunk sizes with LF only", "3\nabc\n0\n\n"},
                                                                                       {"space before CRLF", "3 \r\nabc\r\n0\r\n\r\n"},
                                                                                       {"bad chunk size", "zz\r\nabc\r\n0\r\n\r\n"},
                                                                                       {"no CRLF after chunk data", "3\r\nabcXX0\r\n\r\n"},
                                                                                       {"chunk size over 16 hex digits", "ffffffffffffffffff\r\nabc"},
                                                                                       {"negative chunk size", "-3\r\nabc\r\n0\r\n\r\n"}}) {
      std::vector<Step> steps{bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + chunks), fin()};
      ResponseCase item = failing(name, steps, "InvalidHTTPResponse");
      item.status = 200;
      item.statusText = "OK";
      item.headers = {{"transfer-encoding", "chunked"}};
      cases.push_back(item);
    }
    {
      std::vector<Step> shortBody{bytes("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc"), pause(20), fin()};
      ResponseCase item;
      item.name = "body shorter than Content-Length";
      item.steps = shortBody;
      item.status = 200;
      item.statusText = "OK";
      item.headers = {{"content-length", "10"}};
      item.code = "ECONNRESET";
      cases.push_back(item);
      std::vector<Step> truncated{bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel"), pause(20), fin()};
      ResponseCase chunked = item;
      chunked.name = "chunked body cut short";
      chunked.steps = truncated;
      chunked.headers = {{"transfer-encoding", "chunked"}};
      cases.push_back(chunked);
    }
    return cases;
  }
}

ASYNC_TEST("responses preserve HTTP status, fields and body and reject invalid framing") {
  auto peer = Peer::start();
  TransportScope scope;
  for (const ResponseCase& item : responseCases()) {
    scope.transport->closeIdleConnections();
    peer->respond = script(item.steps);
    RequestInit init{.method = item.method};
    auto running = fetchOutcome(peer->url("/"), std::move(init));
    const Outcome outcome = co_await std::move(running);
    std::printf("      case: %s\n", item.name.c_str());
    EXPECT_EQ(outcome.status, item.status);
    if (item.status) {
      EXPECT_EQ(outcome.statusText, item.statusText);
      if (item.name != "256 fields" && item.name != "a 1 MiB field") EXPECT_EQ(describe(outcome.headers), describe(item.headers));
    }
    EXPECT_EQ(outcome.body, item.body);
    EXPECT_EQ(static_cast<bool>(outcome.failure), !item.code.empty());
    if (outcome.failure) EXPECT_THROWS(std::rethrow_exception(outcome.failure), FetchError);
    EXPECT_EQ(outcome.code, item.code);
  }
  peer->stop();
}

ASYNC_TEST("statuses Bun accepts outside 200..599 cannot be represented by rt::Response") {
  // Bun resolves 600 (ok false) and 099 (status 99, type "error"); rt::Response refuses both.
  auto peer = Peer::start();
  TransportScope scope;
  peer->respond = answer("HTTP/1.1 600 Weird\r\nContent-Length: 0\r\n\r\n");
  auto running = fetchOutcome(peer->url("/"));
  const Outcome outcome = co_await std::move(running);
  EXPECT_EQ(outcome.error, std::string("RangeError: The status provided (600) must be 101 or in the range of [200, 599]"));
  peer->stop();
}

// ---- keep-alive -----------------------------------------------------------------------------

namespace {
  Task<std::string> get(std::shared_ptr<Peer> peer, std::string path) {
    auto running = fetchOutcome(peer->url(path));
    const Outcome outcome = co_await std::move(running);
    co_return outcome.body ? *outcome.body : outcome.error;
  }

  std::vector<int> lastConnections(const Peer& peer, std::size_t count) {
    const std::vector<int> all = peer.connectionIds();
    return std::vector<int>(all.end() - static_cast<std::ptrdiff_t>(std::min(count, all.size())), all.end());
  }
}

ASYNC_TEST("keep-alive connections are reused and released as Bun does") {
  auto peer = Peer::start();
  TransportScope scope;
  peer->respond = answer(kOk);
  auto first = get(peer, "/1");
  const std::string firstBody = co_await std::move(first);
  EXPECT_EQ(firstBody, std::string("ok"));
  auto second = get(peer, "/2");
  const std::string secondBody = co_await std::move(second);
  EXPECT_EQ(secondBody, std::string("ok"));
  EXPECT_EQ(lastConnections(*peer, 2), (std::vector<int>{0, 0}));
  EXPECT_EQ(scope.transport->idleConnections(), std::size_t{1});

  // Concurrent requests open connections of their own; afterwards the first slot is reused.
  peer->respond = script({pause(30), bytes(kOk)});
  std::vector<Promise<std::string>> concurrent;
  for (const char* path : {"/c1", "/c2", "/c3"}) concurrent.push_back(Promise<std::string>(get(peer, path)));
  auto all = rt::all(std::move(concurrent));
  const std::vector<std::string> bodies = co_await all.wait();
  EXPECT_EQ(bodies, (std::vector<std::string>{"ok", "ok", "ok"}));
  EXPECT_EQ(peer->connections.size(), std::size_t{3});
  peer->respond = answer(kOk);
  for (const char* path : {"/s1", "/s2", "/s3"}) {
    auto sequential = get(peer, path);
    const std::string sequentialBody = co_await std::move(sequential);
    EXPECT_EQ(sequentialBody, std::string("ok"));
  }
  const std::vector<int> reused = lastConnections(*peer, 3);
  EXPECT(reused[0] == reused[1] && reused[1] == reused[2]);
  EXPECT_EQ(peer->connections.size(), std::size_t{3});
  peer->stop();
}

ASYNC_TEST("connections that cannot be reused are not") {
  struct Case {
    std::string name;
    std::string response;
    RequestInit init;
  };
  std::vector<Case> cases;
  cases.push_back({"Connection: close response", "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok", RequestInit()});
  cases.push_back({"HTTP/1.0 response", "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok", RequestInit()});
  cases.push_back({"Connection: close request", kOk, RequestInit{.headers = Headers{{"Connection", "close"}}}});
  for (Case& item : cases) {
    auto peer = Peer::start();
    TransportScope scope;
    peer->respond = answer(item.response);
    auto running = fetchOutcome(peer->url("/a"), std::move(item.init));
    const Outcome outcome = co_await std::move(running);
    EXPECT_EQ(outcome.body, std::optional<std::string>("ok"));
    peer->respond = answer(kOk);
    auto next = get(peer, "/b");
    const std::string nextBody = co_await std::move(next);
    EXPECT_EQ(nextBody, std::string("ok"));
    std::printf("      case: %s\n", item.name.c_str());
    EXPECT_EQ(peer->connectionIds(), (std::vector<int>{0, 1}));
    peer->stop();
  }
  // HTTP/1.0 with Connection: keep-alive is reused.
  auto peer = Peer::start();
  TransportScope scope;
  peer->respond = answer("HTTP/1.0 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nok");
  auto first = get(peer, "/a");
  const std::string firstBody = co_await std::move(first);
  EXPECT_EQ(firstBody, std::string("ok"));
  peer->respond = answer(kOk);
  auto second = get(peer, "/b");
  const std::string secondBody = co_await std::move(second);
  EXPECT_EQ(secondBody, std::string("ok"));
  EXPECT_EQ(peer->connectionIds(), (std::vector<int>{0, 0}));
  peer->stop();
}

ASYNC_TEST("bytes after a response are never read as the next response") {
  auto peer = Peer::start();
  TransportScope scope;
  peer->respond = answer(kOk + "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nextra");
  auto first = get(peer, "/g1");
  const std::string firstBody = co_await std::move(first);
  EXPECT_EQ(firstBody, std::string("ok"));
  peer->respond = answer("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nnext");
  auto second = get(peer, "/g2");
  const std::string secondBody = co_await std::move(second);
  EXPECT_EQ(secondBody, std::string("next"));
  EXPECT_EQ(peer->connectionIds(), (std::vector<int>{0, 1}));
  // HEAD cannot consume a body. TRACE can consume its framed response and reuse the socket.
  for (const char* method : {"HEAD", "TRACE"}) {
    scope.transport->closeIdleConnections();
    peer->respond = answer("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\ntrace");
    RequestInit init{.method = method};
    auto running = fetchOutcome(peer->url("/t"), std::move(init));
    const Outcome outcome = co_await std::move(running);
    const bool head = std::string_view(method) == "HEAD";
    EXPECT_EQ(outcome.body, std::optional<std::string>(head ? "" : "trace"));
    auto after = get(peer, "/after");
    const std::string afterBody = co_await std::move(after);
    EXPECT_EQ(afterBody, std::string("trace"));
    const std::vector<int> ids = lastConnections(*peer, 2);
    EXPECT_EQ(ids[0] != ids[1], head);
  }
  peer->stop();
}

ASYNC_TEST("a connection the server closed while idle is replaced without an error") {
  auto peer = Peer::start();
  TransportScope scope;
  peer->respond = script({bytes(kOk), pause(10), fin()});
  auto first = get(peer, "/fin1");
  const std::string firstBody = co_await std::move(first);
  EXPECT_EQ(firstBody, std::string("ok"));
  auto settling = rt::sleep(100);
  co_await std::move(settling);
  peer->respond = answer(kOk);
  auto second = get(peer, "/fin2");
  const std::string secondBody = co_await std::move(second);
  EXPECT_EQ(secondBody, std::string("ok"));
  EXPECT_EQ(peer->connectionIds(), (std::vector<int>{0, 1}));
  peer->stop();
}

ASYNC_TEST("a fully received body nobody read still frees its connection; a cancelled one closes it") {
  auto peer = Peer::start();
  TransportScope scope;
  peer->respond = answer(kOk);
  auto calling = fetch(RequestInfo(peer->url("/unread1")));
  const Response unread = co_await std::move(calling);
  EXPECT_EQ(unread.status(), 200);
  auto settling = rt::sleep(50);
  co_await std::move(settling);
  auto next = get(peer, "/unread2");
  const std::string nextBody = co_await std::move(next);
  EXPECT_EQ(nextBody, std::string("ok"));
  EXPECT_EQ(peer->connectionIds(), (std::vector<int>{0, 0}));

  peer->respond = answer("HTTP/1.1 200 OK\r\nContent-Length: 10000000\r\n\r\n" + std::string(10000000, 'x'));
  auto bigCall = fetch(RequestInfo(peer->url("/big")));
  const Response big = co_await std::move(bigCall);
  auto cancelling = big.body()->cancel();
  co_await std::move(cancelling);
  peer->respond = answer(kOk);
  auto settle = rt::sleep(50);
  co_await std::move(settle);
  auto after = get(peer, "/afterbig");
  const std::string afterBody = co_await std::move(after);
  EXPECT_EQ(afterBody, std::string("ok"));
  EXPECT_EQ(lastConnections(*peer, 2), (std::vector<int>{0, 1}));
  EXPECT(peer->connections[0]->clientClosed);
  peer->stop();
}

ASYNC_TEST("a response that arrives while the request body is still being sent is delivered") {
  // Bun: a 413 + Connection: close sent right after the head of a large upload resolves with 413
  // "too big!"; a 200 sent early while the server keeps reading resolves with 200 "early".
  for (const bool closeAfter : {true, false}) {
    auto peer = Peer::start();
    TransportScope scope;
    peer->open = [closeAfter](std::shared_ptr<PeerConnection> connection) -> Task<bool> {
      std::vector<char> data(65536);
      std::string received;
      while (received.find("\r\n\r\n") == std::string::npos) {
        auto receiving = connection->readSome(data.data(), data.size());
        const std::size_t count = co_await std::move(receiving);
        if (count == 0) co_return false;
        received.append(data.data(), count);
      }
      const std::string response =
          closeAfter ? std::string("HTTP/1.1 413 Payload Too Large\r\nContent-Length: 8\r\nConnection: close\r\n\r\ntoo big!") : std::string("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nearly");
      auto sending = connection->send(response);
      co_await std::move(sending);
      if (closeAfter) {
        connection->end();
        co_return false;
      }
      for (;;) {
        auto draining = connection->readSome(data.data(), data.size());
        const std::size_t count = co_await std::move(draining);
        if (count == 0) co_return false;
      }
    };
    RequestInit init{.method = "POST", .body = BodyInit(std::string(32 << 20, 'x'))};
    auto running = fetchOutcome(peer->url("/upload"), std::move(init));
    const Outcome outcome = co_await std::move(running);
    EXPECT_EQ(outcome.status, std::optional<int>(closeAfter ? 413 : 200));
    EXPECT_EQ(outcome.body, std::optional<std::string>(closeAfter ? "too big!" : "early"));
    EXPECT_EQ(scope.transport->idleConnections(), std::size_t{0});
    peer->stop();
  }
}

ASYNC_TEST("connection close releases a parked request upload before loop teardown") {
  for (const bool earlyResponse : {false, true}) {
    auto peer = Peer::start();
    TransportScope scope;
    auto pulling = withResolvers();
    peer->open = [ready = pulling.promise, earlyResponse](std::shared_ptr<PeerConnection> connection) -> Task<bool> {
      co_await ready.wait();
      if (earlyResponse) {
        auto sending = connection->send("HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        co_await std::move(sending);
      }
      connection->end();
      co_return false;
    };
    std::weak_ptr<rt::detail::StreamState<ByteChunk>> upload;
    {
      UnderlyingSource<ByteChunk> source;
      source.pull = [ready = pulling.resolve](ReadableStreamDefaultController<ByteChunk>) { ready(); };
      ByteStream body(std::move(source), QueuingStrategy{0});
      upload = body.state();
      RequestInit init{.method = "POST", .body = BodyInit(std::move(body)), .duplex = "half"};
      auto fetching = fetchOutcome(peer->url("/upload"), std::move(init));
      const auto outcome = co_await std::move(fetching);
      if (earlyResponse) EXPECT_EQ(outcome.status, std::optional<int>(413));
      else EXPECT_EQ(outcome.code, "ECONNRESET");
    }
    co_await rt::sleep(1);
    EXPECT(upload.expired());
    peer->stop();
  }
}

ASYNC_TEST("an upload failure after response headers rejects a parked response read") {
  auto peer = Peer::start();
  TransportScope scope;
  auto pulling = withResolvers();
  peer->open = [ready = pulling.promise](std::shared_ptr<PeerConnection> connection) -> Task<bool> {
    co_await ready.wait();
    auto sending = connection->send("HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\n");
    co_await std::move(sending);
    co_return false;
  };
  std::optional<ReadableStreamDefaultController<ByteChunk>> upload;
  UnderlyingSource<ByteChunk> source;
  source.start = [&upload](ReadableStreamDefaultController<ByteChunk> controller) { upload.emplace(std::move(controller)); };
  source.pull = [ready = pulling.resolve](ReadableStreamDefaultController<ByteChunk>) { ready(); };
  RequestInit init{.method = "POST", .body = BodyInit(ByteStream(std::move(source), QueuingStrategy{0})), .duplex = "half"};
  auto fetching = fetch(RequestInfo(peer->url("/upload")), std::move(init));
  const auto response = co_await std::move(fetching);
  EXPECT_EQ(response.status(), 200);
  auto reader = response.body()->getReader();
  auto reading = reader.readPromise();
  EXPECT(reading.pending());
  reading.catch_(ignoreRejection);

  const auto failure = makeReason(TypeError("upload failed"));
  upload->error(failure);
  upload.reset();
  co_await rt::sleep(1);
  EXPECT(reading.rejected());
  EXPECT(reading.reason() == failure);
  peer->stop();
}

ASYNC_TEST("every fetched response has a body stream, an empty one when the response has no body") {
  auto peer = Peer::start();
  TransportScope scope;
  for (const auto& [method, response] : std::vector<std::pair<std::string, std::string>>{{"HEAD", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"},
                                                                                         {"GET", "HTTP/1.1 204 No Content\r\n\r\n"},
                                                                                         {"GET", "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"}}) {
    peer->respond = answer(response);
    RequestInit init{.method = method};
    auto calling = fetch(RequestInfo(peer->url("/")), std::move(init));
    const Response fetched = co_await std::move(calling);
    EXPECT(fetched.body().has_value());
    auto reading = fetched.text();
    const std::string text = co_await std::move(reading);
    EXPECT_EQ(text, std::string(""));
  }
  EXPECT_EQ(peer->connectionIds(), (std::vector<int>{0, 0, 0}));
  peer->stop();
}

// ---- stale keep-alive connections ---------------------------------------------------------

ASYNC_TEST("idempotent requests on a stale reused connection are retried, others fail") {
  struct Case {
    std::string method;
    bool retried;
  };
  const std::vector<Case> cases{{"GET", true}, {"HEAD", true}, {"PUT", true}, {"DELETE", true}, {"OPTIONS", true}, {"TRACE", true}, {"PATCH", false}, {"POST", false}, {"PROPFIND", false}};
  for (const Case& item : cases) {
    for (const Step::Kind kind : {Step::Kind::end, Step::Kind::reset}) {
      auto peer = Peer::start();
      TransportScope scope;
      peer->respond = answer(kOk);
      auto warming = get(peer, "/warm");
      const std::string warmingBody = co_await std::move(warming);
      EXPECT_EQ(warmingBody, std::string("ok"));
      auto calls = std::make_shared<int>(0);
      peer->respond = [calls, kind](std::shared_ptr<PeerConnection> connection, PeerRequest) -> Task<void> {
        if (++*calls == 1) {
          if (kind == Step::Kind::end) connection->end();
          else connection->reset();
          co_return;
        }
        auto sending = connection->send("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
        co_await std::move(sending);
      };
      RequestInit init{.method = item.method};
      if (item.method == "PUT" || item.method == "PATCH" || item.method == "POST") init.body = BodyInit(std::string("b"));
      auto running = fetchOutcome(peer->url("/" + item.method), std::move(init));
      const Outcome outcome = co_await std::move(running);
      std::printf("      case: %s %s\n", item.method.c_str(), kind == Step::Kind::end ? "FIN" : "RST");
      if (item.retried) {
        EXPECT_EQ(outcome.status, std::optional<int>(200));
        EXPECT_EQ(peer->connectionIds(), (std::vector<int>{0, 0, 1}));
      } else {
        EXPECT_EQ(outcome.error, kClosed);
        EXPECT_EQ(outcome.code, std::string("ECONNRESET"));
        EXPECT_EQ(peer->connectionIds(), (std::vector<int>{0, 0}));
      }
      peer->stop();
    }
  }
}

ASYNC_TEST("stale retries: a partial head, several stale connections, stream bodies") {
  {
    auto peer = Peer::start();
    TransportScope scope;
    peer->respond = answer(kOk);
    auto warming = get(peer, "/warm");
    const std::string warmingBody = co_await std::move(warming);
    EXPECT_EQ(warmingBody, std::string("ok"));
    auto calls = std::make_shared<int>(0);
    peer->respond = [calls](std::shared_ptr<PeerConnection> connection, PeerRequest) -> Task<void> {
      const std::string reply = ++*calls == 1 ? std::string("HTTP/1.1 200 OK\r\nContent-Len") : std::string("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
      auto sending = connection->send(reply);
      co_await std::move(sending);
      if (*calls == 1) connection->end();
    };
    auto partial = get(peer, "/partial");
    const std::string partialBody = co_await std::move(partial);
    EXPECT_EQ(partialBody, std::string("ok"));
    EXPECT_EQ(peer->connectionIds(), (std::vector<int>{0, 0, 1}));
    peer->stop();
  }
  {
    // Two stale pooled connections: each is tried, then the third pooled one serves.
    auto peer = Peer::start();
    TransportScope scope;
    peer->respond = script({pause(30), bytes(kOk)});
    std::vector<Promise<std::string>> warm;
    for (const char* path : {"/w1", "/w2", "/w3"}) warm.push_back(Promise<std::string>(get(peer, path)));
    auto warming = rt::all(std::move(warm));
    co_await warming.wait();
    EXPECT_EQ(scope.transport->idleConnections(), std::size_t{3});
    auto calls = std::make_shared<int>(0);
    peer->respond = [calls](std::shared_ptr<PeerConnection> connection, PeerRequest) -> Task<void> {
      if (++*calls <= 2) {
        connection->end();
        co_return;
      }
      auto sending = connection->send("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
      co_await std::move(sending);
    };
    auto stale = get(peer, "/twostale");
    const std::string staleBody = co_await std::move(stale);
    EXPECT_EQ(staleBody, std::string("ok"));
    EXPECT_EQ(peer->requests.size(), std::size_t{6});
    EXPECT_EQ(peer->connections.size(), std::size_t{3});
    peer->stop();
  }
  {
    // A stream body cannot be replayed: no retry.
    auto peer = Peer::start();
    TransportScope scope;
    peer->respond = answer(kOk);
    auto warming = get(peer, "/warm");
    const std::string warmingBody = co_await std::move(warming);
    EXPECT_EQ(warmingBody, std::string("ok"));
    peer->respond = [](std::shared_ptr<PeerConnection> connection, PeerRequest) -> Task<void> {
      connection->end();
      co_return;
    };
    UnderlyingSource<ByteChunk> source;
    source.start = [](ReadableStreamDefaultController<ByteChunk> controller) {
      controller.enqueue(ByteChunk{'s'});
      controller.close();
    };
    RequestInit init{.method = "PUT", .body = BodyInit(ByteStream(std::move(source))), .duplex = "half"};
    auto running = fetchOutcome(peer->url("/putstream"), std::move(init));
    const Outcome outcome = co_await std::move(running);
    EXPECT_EQ(outcome.error, kClosed);
    EXPECT_EQ(peer->connectionIds(), (std::vector<int>{0, 0}));
    peer->stop();
  }
}

// ---- the pool -------------------------------------------------------------------------------

ASYNC_TEST("the pool keeps 64 idle connections per scheme") {
  auto peer = Peer::start();
  TransportScope scope;
  peer->respond = script({pause(200), bytes(kOk)});
  std::vector<Promise<std::string>> pending;
  for (int index = 0; index < 70; ++index) pending.push_back(Promise<std::string>(get(peer, "/p" + std::to_string(index))));
  auto all = rt::all(std::move(pending));
  co_await all.wait();
  auto settling = rt::sleep(100);
  co_await std::move(settling);
  EXPECT_EQ(peer->connections.size(), std::size_t{70});
  EXPECT_EQ(peer->closedByClient(), std::size_t{6});
  EXPECT_EQ(scope.transport->idleConnections(), std::size_t{64});
  scope.transport->closeIdleConnections();
  EXPECT_EQ(scope.transport->idleConnections(), std::size_t{0});
  peer->stop();
}

ASYNC_TEST("idle connections expire after 300 seconds") {
  auto peer = Peer::start();
  TransportScope scope;
  peer->respond = answer(kOk);
  auto first = get(peer, "/a");
  const std::string firstBody = co_await std::move(first);
  EXPECT_EQ(firstBody, std::string("ok"));
  test::clock().advance(299000);
  auto turn = rt::sleep(1);
  co_await std::move(turn);
  auto second = get(peer, "/b");
  const std::string secondBody = co_await std::move(second);
  EXPECT_EQ(secondBody, std::string("ok"));
  EXPECT_EQ(peer->connectionIds(), (std::vector<int>{0, 0}));
  test::clock().advance(300000);
  auto expiry = rt::sleep(1);
  co_await std::move(expiry);
  EXPECT_EQ(scope.transport->idleConnections(), std::size_t{0});
  auto settling = rt::sleep(20);
  co_await std::move(settling);
  EXPECT(peer->connections[0]->clientClosed);
  auto third = get(peer, "/c");
  const std::string thirdBody = co_await std::move(third);
  EXPECT_EQ(thirdBody, std::string("ok"));
  EXPECT_EQ(peer->connectionIds(), (std::vector<int>{0, 0, 1}));
  peer->stop();
}

TEST("idle pooled connections do not keep a loop running, and die with it") {
  std::shared_ptr<HttpClientTransport> transport;
  {
    Loop loop;
    blockOn(loop, rt::async([&transport]() -> Task<void> {
    auto peer = Peer::start();
    peer->respond = answer(kOk);
    transport = std::make_shared<HttpClientTransport>();
    setFetchTransport(transport);
    auto running = fetchOutcome(peer->url("/idle"));
    const Outcome outcome = co_await std::move(running);
    EXPECT_EQ(outcome.body, std::optional<std::string>("ok"));
    EXPECT_EQ(transport->idleConnections(), std::size_t{1});
    peer->stop();
  }));
    // Only the idle connection is left: run() returns at once.
    loop.run();
    setFetchTransport(nullptr);
  }
  // The loop's end closed the pool; the transport outlives it.
  EXPECT_EQ(transport->idleConnections(), std::size_t{0});
}

// ---- streaming --------------------------------------------------------------------------------

ASYNC_TEST("a body chunk is delivered as it arrives") {
  auto peer = Peer::start();
  TransportScope scope;
  auto secondSent = std::make_shared<bool>(false);
  peer->respond = [secondSent](std::shared_ptr<PeerConnection> connection, PeerRequest) -> Task<void> {
    auto head = connection->send("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nTransfer-Encoding: chunked\r\n\r\nb\r\ndata: one\n\n\r\n");
    co_await std::move(head);
    auto waiting = rt::sleep(300);
    co_await std::move(waiting);
    *secondSent = true;
    auto second = connection->send("b\r\ndata: two\n\n\r\n");
    co_await std::move(second);
    auto last = connection->send("0\r\n\r\n");
    co_await std::move(last);
  };
  auto calling = fetch(RequestInfo(peer->url("/sse")));
  const Response response = co_await std::move(calling);
  auto reader = response.body()->getReader();
  auto reading = reader.read();
  const ReadableStreamReadResult<ByteChunk> first = co_await std::move(reading);
  EXPECT(!*secondSent);
  EXPECT_EQ(std::string(first.value->begin(), first.value->end()), std::string("data: one\n\n"));
  auto readingSecond = reader.read();
  const ReadableStreamReadResult<ByteChunk> second = co_await std::move(readingSecond);
  EXPECT(*secondSent);
  EXPECT_EQ(std::string(second.value->begin(), second.value->end()), std::string("data: two\n\n"));
  auto readingEnd = reader.read();
  const ReadableStreamReadResult<ByteChunk> end = co_await std::move(readingEnd);
  EXPECT(end.done);
  peer->stop();
}

ASYNC_TEST("reads get what one socket read produced (Bun's chunk boundaries)") {
  struct Case {
    std::string name;
    std::vector<Step> steps;
    std::vector<std::string> chunks;
  };
  std::vector<Case> cases;
  cases.push_back({"close-delimited event stream", {bytes("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n"), bytes("data: one\n\n"), pause(150), bytes("data: two\n\n"), pause(50), fin()},
                   {"data: one\n\n", "data: two\n\n"}});
  cases.push_back({"Content-Length body in two writes", {bytes("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello"), pause(150), bytes("world")}, {"hello", "world"}});
  cases.push_back({"three chunks in one write", {bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\naaa\r\n3\r\nbbb\r\n3\r\nccc\r\n0\r\n\r\n")}, {"aaabbbccc"}});
  cases.push_back({"head and body in one write", {bytes("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello")}, {"hello"}});
  cases.push_back({"head split across writes", {bytes("HTTP/1.1 200 OK\r\nX-A: 1\r\n"), pause(150), bytes("Content-Length: 2\r\n\r\nok")}, {"ok"}});
  cases.push_back({"chunk data split across writes", {bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel"), pause(150), bytes("lo\r\n0\r\n\r\n")}, {"hel", "lo"}});
  for (const Case& item : cases) {
    auto peer = Peer::start();
    TransportScope scope;
    peer->respond = script(item.steps);
    auto calling = fetch(RequestInfo(peer->url("/s")));
    const Response response = co_await std::move(calling);
    auto reader = response.body()->getReader();
    std::vector<std::string> chunks;
    for (;;) {
      auto reading = reader.read();
      const ReadableStreamReadResult<ByteChunk> step = co_await std::move(reading);
      if (step.done) break;
      chunks.emplace_back(step.value->begin(), step.value->end());
    }
    std::printf("      case: %s\n", item.name.c_str());
    EXPECT_EQ(chunks, item.chunks);
    peer->stop();
  }
}

ASYNC_TEST("large bodies both ways") {
  auto peer = Peer::start();
  TransportScope scope;
  std::string big(3000000, 'z');
  for (std::size_t index = 0; index < big.size(); index += 997) big[index] = static_cast<char>('a' + index % 26);
  peer->respond = answer("HTTP/1.1 200 OK\r\nContent-Length: 3000000\r\n\r\n" + big);
  auto calling = fetch(RequestInfo(peer->url("/big")));
  const Response response = co_await std::move(calling);
  auto reading = response.text();
  const std::string text = co_await std::move(reading);
  EXPECT(text == big);
  peer->respond = answer("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2dc6c0\r\n" + big + "\r\n0\r\n\r\n");
  auto chunkedCall = fetch(RequestInfo(peer->url("/chunked")));
  const Response chunked = co_await std::move(chunkedCall);
  auto chunkedReading = chunked.text();
  const std::string chunkedText = co_await std::move(chunkedReading);
  EXPECT(chunkedText == big);
  std::string upload(5000000, 'u');
  upload[4999999] = '!';
  peer->respond = answer(kOk);
  RequestInit init{.method = "POST", .body = BodyInit(upload)};
  auto running = fetchOutcome(peer->url("/upload"), std::move(init));
  const Outcome outcome = co_await std::move(running);
  EXPECT_EQ(outcome.body, std::optional<std::string>("ok"));
  const std::string& wire = peer->requests.back().wire;
  EXPECT(wire.find("\r\nContent-Length: 5000000\r\n\r\n") != std::string::npos);
  EXPECT(wire.size() > 5000000 && wire.substr(wire.size() - 5000000) == upload);
  peer->stop();
}

// ---- aborts -----------------------------------------------------------------------------------

ASYNC_TEST("an abort rejects with the signal's reason at every stage and closes the connection") {
  // Before the request: nothing is sent.
  {
    auto peer = Peer::start();
    TransportScope scope;
    peer->respond = answer(kOk);
    AbortController controller;
    const Reason reason = makeReason(JsError("stop early"));
    controller.abort(reason);
    RequestInit init{.signal = controller.signal()};
    auto calling = fetch(RequestInfo(peer->url("/early")), std::move(init));
    EXPECT_REJECTS_REASON(std::move(calling), reason);
    EXPECT_EQ(peer->connections.size(), std::size_t{0});
    peer->stop();
  }
  // While waiting for the response head.
  {
    auto peer = Peer::start();
    TransportScope scope;
    AbortController controller;
    RequestInit init{.signal = controller.signal()};
    Promise<Response> pending(fetch(RequestInfo(peer->url("/hang")), std::move(init)));
    auto waiting = rt::sleep(50);
    co_await std::move(waiting);
    EXPECT_EQ(peer->requests.size(), std::size_t{1});
    const Reason reason = makeReason(JsError("stop waiting"));
    controller.abort(reason);
    EXPECT_REJECTS_REASON(pending.wait(), reason);
    auto settling = rt::sleep(50);
    co_await std::move(settling);
    EXPECT(peer->connections[0]->clientClosed);
    EXPECT_EQ(scope.transport->idleConnections(), std::size_t{0});
    peer->stop();
  }
  // In the middle of the body.
  {
    auto peer = Peer::start();
    TransportScope scope;
    peer->respond = answer("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\npart");
    AbortController controller;
    RequestInit init{.signal = controller.signal()};
    auto calling = fetch(RequestInfo(peer->url("/abort")), std::move(init));
    const Response response = co_await std::move(calling);
    auto reader = response.body()->getReader();
    auto reading = reader.read();
    const ReadableStreamReadResult<ByteChunk> first = co_await std::move(reading);
    EXPECT_EQ(std::string(first.value->begin(), first.value->end()), std::string("part"));
    controller.abort();
    auto next = reader.read();
    EXPECT_REJECTS_MESSAGE(std::move(next), DOMException, "The operation was aborted.");
    auto settling = rt::sleep(50);
    co_await std::move(settling);
    EXPECT(peer->connections[0]->clientClosed);
    peer->respond = answer(kOk);
    auto after = get(peer, "/after-abort");
    const std::string afterBody = co_await std::move(after);
    EXPECT_EQ(afterBody, std::string("ok"));
    EXPECT_EQ(peer->connectionIds(), (std::vector<int>{0, 1}));
    peer->stop();
  }
  // While the request body is still being produced: the stream is cancelled with the reason.
  {
    auto peer = Peer::start();
    TransportScope scope;
    auto cancelledWith = std::make_shared<Reason>();
    UnderlyingSource<ByteChunk> source;
    source.start = [](ReadableStreamDefaultController<ByteChunk> controller) { controller.enqueue(ByteChunk{'u', 'p'}); };
    source.cancel = [cancelledWith](Reason reason) { *cancelledWith = reason; };
    AbortController controller;
    RequestInit init{.method = "POST", .body = BodyInit(ByteStream(std::move(source))), .signal = controller.signal(), .duplex = "half"};
    Promise<Response> pending(fetch(RequestInfo(peer->url("/upload")), std::move(init)));
    auto waiting = rt::sleep(50);
    co_await std::move(waiting);
    const Reason reason = makeReason(JsError("stop uploading"));
    controller.abort(reason);
    EXPECT_REJECTS_REASON(pending.wait(), reason);
    auto settling = rt::sleep(50);
    co_await std::move(settling);
    EXPECT(*cancelledWith == reason);
    EXPECT_EQ(peer->scrub(peer->connections[0]->buffer), "POST /upload HTTP/1.1\r\n" + kDefaults + "Transfer-Encoding: chunked\r\n\r\n2\r\nup\r\n");
    EXPECT(peer->connections[0]->clientClosed);
    peer->stop();
  }
  // AbortSignal.timeout.
  {
    auto peer = Peer::start();
    TransportScope scope;
    RequestInit init{.signal = AbortSignal::timeout(50)};
    auto calling = fetch(RequestInfo(peer->url("/slow")), std::move(init));
    EXPECT_REJECTS_MESSAGE(std::move(calling), DOMException, "The operation timed out.");
    peer->stop();
  }
}

ASYNC_TEST("Bun's 300 second timeout fails a silent request, then a stalled body, with TimeoutError") {
  // Bun 1.4.0: a server that never answers, and one that stops in the middle of the body, both
  // fail after 300 s with DOMException TimeoutError "The operation timed out." (fake time here).
  auto peer = Peer::start();
  TransportScope scope;
  Promise<Response> pending(fetch(RequestInfo(peer->url("/silent"))));
  auto reaching = rt::sleep(50);
  co_await std::move(reaching);
  EXPECT_EQ(peer->requests.size(), std::size_t{1});
  test::clock().advance(299000);
  auto turn = rt::sleep(1);
  co_await std::move(turn);
  EXPECT(pending.state()->pending());
  test::clock().advance(1100);
  EXPECT_REJECTS_MESSAGE(pending.wait(), DOMException, "The operation timed out.");
  auto settling = rt::sleep(20);
  co_await std::move(settling);
  EXPECT(peer->connections[0]->clientClosed);

  peer->respond = answer("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\npart");
  auto calling = fetch(RequestInfo(peer->url("/stall")));
  const Response response = co_await std::move(calling);
  auto reader = response.body()->getReader();
  auto reading = reader.read();
  const ReadableStreamReadResult<ByteChunk> first = co_await std::move(reading);
  EXPECT_EQ(std::string(first.value->begin(), first.value->end()), std::string("part"));
  auto next = reader.read();
  test::clock().advance(300100);
  EXPECT_REJECTS_MESSAGE(std::move(next), DOMException, "The operation timed out.");
  peer->stop();
}

ASYNC_TEST("the timeout counts idle time: a body trickling every 200 seconds completes") {
  // Bun 1.4.0: a body sending one byte every 100 s for 500 s completed (the 300 s limit is idle time).
  auto peer = Peer::start();
  TransportScope scope;
  peer->respond = answer("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\n");
  auto calling = fetch(RequestInfo(peer->url("/trickle")));
  const Response response = co_await std::move(calling);
  auto reader = response.body()->getReader();
  for (int index = 0; index < 3; ++index) {
    test::clock().advance(200000);
    auto turn = rt::sleep(1);
    co_await std::move(turn);
    auto sending = peer->connections[0]->send("x");
    co_await std::move(sending);
    auto reading = reader.read();
    const ReadableStreamReadResult<ByteChunk> step = co_await std::move(reading);
    ASSERT(!step.done);
    EXPECT_EQ(std::string(step.value->begin(), step.value->end()), std::string("x"));
  }
  auto ending = reader.read();
  const ReadableStreamReadResult<ByteChunk> end = co_await std::move(ending);
  EXPECT(end.done);
  peer->stop();
}

// ---- redirects --------------------------------------------------------------------------------

namespace {
  std::string redirect(int status, const std::string& location, const std::string& body = "") {
    return "HTTP/1.1 " + std::to_string(status) + " R\r\nLocation: " + location + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
  }
  std::string okBody(const std::string& body) { return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body; }

  // Answers the first request with `first` and every later one with `then`.
  Peer::Responder firstThen(std::string first, std::string then) {
    auto calls = std::make_shared<int>(0);
    return [calls, first, then](std::shared_ptr<PeerConnection> connection, PeerRequest) {
      std::vector<Step> steps{bytes(++*calls == 1 ? first : then)};
      return play(std::move(connection), std::move(steps));
    };
  }
}

ASYNC_TEST("redirects rewrite the method, body and headers as Bun does") {
  const std::string sent = "Authorization: Bearer secret\r\nContent-Type: text/plain\r\nCookie: c=1\r\nProxy-Authorization: p\r\nx-custom: keep\r\n";
  auto headers = [] { return Headers{{"Authorization", "Bearer secret"}, {"Cookie", "c=1"}, {"Content-Type", "text/plain"}, {"x-custom", "keep"}, {"Proxy-Authorization", "p"}}; };
  for (const int status : {301, 302, 303, 307, 308}) {
    auto peer = Peer::start();
    TransportScope scope;
    peer->respond = firstThen(redirect(status, "/next?x=1"), okBody("done"));
    RequestInit init{.method = "POST", .headers = headers(), .body = BodyInit(std::string("payload"))};
    auto running = fetchOutcome(peer->url("/start"), std::move(init));
    const Outcome outcome = co_await std::move(running);
    std::printf("      case: POST %d same origin\n", status);
    EXPECT_EQ(outcome.body, std::optional<std::string>("done"));
    EXPECT_EQ(peer->scrub(outcome.url), std::string("http://127.0.0.1:PORT/next?x=1"));
    EXPECT(outcome.redirected);
    const std::vector<std::string> wires = peer->wires();
    ASSERT_EQ(wires.size(), std::size_t{2});
    EXPECT_EQ(wires[0], "POST /start HTTP/1.1\r\n" + sent + kDefaults + "Content-Length: 7\r\n\r\npayload");
    if (status == 307 || status == 308) EXPECT_EQ(wires[1], "POST /next?x=1 HTTP/1.1\r\n" + sent + kDefaults + "Content-Length: 7\r\n\r\npayload");
    else EXPECT_EQ(wires[1], "GET /next?x=1 HTTP/1.1\r\nAuthorization: Bearer secret\r\nCookie: c=1\r\nProxy-Authorization: p\r\nx-custom: keep\r\n" + kDefaults + "\r\n");
    EXPECT_EQ(peer->connectionIds(), (std::vector<int>{0, 0}));
    peer->stop();
  }
  // Cross-origin: Authorization, Cookie and Proxy-Authorization are dropped.
  for (const auto& [method, status] : std::vector<std::pair<std::string, int>>{{"POST", 302}, {"POST", 307}, {"GET", 301}}) {
    auto a = Peer::start();
    auto b = Peer::start();
    TransportScope scope;
    a->respond = answer(redirect(status, b->url("/other")));
    b->respond = answer(okBody("cross"));
    RequestInit init{.method = method, .headers = headers()};
    if (method == "POST") init.body = BodyInit(std::string("payload"));
    auto running = fetchOutcome(a->url("/start"), std::move(init));
    const Outcome outcome = co_await std::move(running);
    std::printf("      case: %s %d cross origin\n", method.c_str(), status);
    EXPECT_EQ(outcome.body, std::optional<std::string>("cross"));
    EXPECT_EQ(b->scrub(outcome.url), std::string("http://127.0.0.1:PORT/other"));
    ASSERT_EQ(b->requests.size(), std::size_t{1});
    if (status == 302) EXPECT_EQ(b->wires()[0], "GET /other HTTP/1.1\r\nx-custom: keep\r\n" + kDefaults + "\r\n");
    else if (status == 307) EXPECT_EQ(b->wires()[0], "POST /other HTTP/1.1\r\nContent-Type: text/plain\r\nx-custom: keep\r\n" + kDefaults + "Content-Length: 7\r\n\r\npayload");
    else EXPECT_EQ(b->wires()[0], "GET /other HTTP/1.1\r\nContent-Type: text/plain\r\nx-custom: keep\r\n" + kDefaults + "\r\n");
    a->stop();
    b->stop();
  }
  // PUT 303 becomes GET, HEAD 303 stays HEAD, PUT 301 stays PUT.
  struct Case {
    std::string method;
    int status;
    std::string second;
  };
  for (const Case& item : std::vector<Case>{{"PUT", 303, "GET /g HTTP/1.1\r\n" + kDefaults + "\r\n"},
                                            {"HEAD", 303, "HEAD /g HTTP/1.1\r\n" + kDefaults + "\r\n"},
                                            {"PUT", 301, "PUT /g HTTP/1.1\r\n" + kDefaults + "Content-Length: 2\r\n\r\npp"}}) {
    auto peer = Peer::start();
    TransportScope scope;
    peer->respond = firstThen(redirect(item.status, "/g"), item.method == "HEAD" ? std::string("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n") : okBody("g"));
    RequestInit init{.method = item.method};
    if (item.method == "PUT") init.body = BodyInit(std::string("pp"));
    auto running = fetchOutcome(peer->url("/s"), std::move(init));
    const Outcome outcome = co_await std::move(running);
    EXPECT_EQ(outcome.status, std::optional<int>(200));
    EXPECT_EQ(peer->wires().back(), item.second);
    peer->stop();
  }
}

ASYNC_TEST("redirect targets, modes, limits and errors") {
  auto peer = Peer::start();
  TransportScope scope;
  // A relative Location keeps its own fragment; the request's fragment is not inherited.
  peer->respond = firstThen(redirect(302, "../x?y#frag"), okBody("rel"));
  auto relative = fetchOutcome(peer->url("/a/b/c"));
  const Outcome relativeOutcome = co_await std::move(relative);
  EXPECT_EQ(peer->scrub(relativeOutcome.url), std::string("http://127.0.0.1:PORT/a/x?y#frag"));
  EXPECT_EQ(peer->wires().back(), "GET /a/x?y HTTP/1.1\r\n" + kDefaults + "\r\n");
  peer->respond = firstThen(redirect(302, "/g"), okBody("frag"));
  auto fragment = fetchOutcome(peer->url("/f#orig"));
  const Outcome fragmentOutcome = co_await std::move(fragment);
  EXPECT_EQ(peer->scrub(fragmentOutcome.url), std::string("http://127.0.0.1:PORT/g"));
  // Without a redirect the URL keeps the request's fragment.
  peer->respond = answer(kOk);
  auto plain = fetchOutcome(peer->url("/a?b=1#frag"));
  const Outcome plainOutcome = co_await std::move(plain);
  EXPECT_EQ(peer->scrub(plainOutcome.url), std::string("http://127.0.0.1:PORT/a?b=1#frag"));
  EXPECT(!plainOutcome.redirected);
  // A Latin-1 Location is a URL like any other.
  peer->respond = firstThen("HTTP/1.1 302 R\r\nLocation: /caf\xe9 x\r\nContent-Length: 0\r\n\r\n", okBody("l"));
  auto latin = fetchOutcome(peer->url("/l1"));
  const Outcome latinOutcome = co_await std::move(latin);
  EXPECT_EQ(peer->scrub(latinOutcome.url), std::string("http://127.0.0.1:PORT/caf%C3%A9%20x"));
  // No Location: the 3xx is the response.
  peer->respond = answer("HTTP/1.1 302 R\r\nContent-Length: 7\r\n\r\nbody302");
  auto noLocation = fetchOutcome(peer->url("/nl"));
  const Outcome noLocationOutcome = co_await std::move(noLocation);
  EXPECT_EQ(noLocationOutcome.status, std::optional<int>(302));
  EXPECT_EQ(noLocationOutcome.body, std::optional<std::string>("body302"));
  EXPECT(!noLocationOutcome.redirected);
  // 300, 304 and 305 are not followed.
  for (const int status : {300, 304, 305}) {
    peer->respond = answer(redirect(status, "/x"));
    auto running = fetchOutcome(peer->url("/" + std::to_string(status)));
    const Outcome outcome = co_await std::move(running);
    EXPECT_EQ(outcome.status, std::optional<int>(status));
  }
  // manual: the 3xx as is.
  peer->respond = answer(redirect(302, "/elsewhere", "mbody"));
  RequestInit manualInit{.redirect = "manual"};
  auto manual = fetchOutcome(peer->url("/m"), std::move(manualInit));
  const Outcome manualOutcome = co_await std::move(manual);
  EXPECT_EQ(manualOutcome.status, std::optional<int>(302));
  EXPECT_EQ(manualOutcome.body, std::optional<std::string>("mbody"));
  EXPECT_EQ(peer->scrub(manualOutcome.url), std::string("http://127.0.0.1:PORT/m"));
  EXPECT(!manualOutcome.redirected);
  // error: any 301/302/303/307/308, with or without Location.
  for (const std::string& response : {redirect(302, "/elsewhere"), std::string("HTTP/1.1 302 R\r\nContent-Length: 0\r\n\r\n")}) {
    peer->respond = answer(response);
    RequestInit errorInit{.redirect = "error"};
    auto running = fetchOutcome(peer->url("/e"), std::move(errorInit));
    const Outcome outcome = co_await std::move(running);
    EXPECT_EQ(peer->scrub(outcome.error), verbose("UnexpectedRedirect", "http://127.0.0.1:PORT/e"));
    EXPECT_EQ(outcome.code, std::string("UnexpectedRedirect"));
  }
  RequestInit fineInit{.redirect = "error"};
  peer->respond = answer(okBody("fine"));
  auto fine = fetchOutcome(peer->url("/e3"), std::move(fineInit));
  const Outcome fineOutcome = co_await std::move(fine);
  EXPECT_EQ(fineOutcome.body, std::optional<std::string>("fine"));
  // An invalid Location, a non-http(s) target; errors name the original URL.
  peer->respond = firstThen(redirect(302, "/hop2"), redirect(302, "http://[bad"));
  auto invalid = fetchOutcome(peer->url("/hop1"));
  const Outcome invalidOutcome = co_await std::move(invalid);
  EXPECT_EQ(invalidOutcome.error, std::string("TypeError: Redirect URL in Location header is invalid."));
  EXPECT_EQ(invalidOutcome.code, std::string("RedirectURLInvalid"));
  for (const std::string& target : {std::string("data:text/plain,hi"), std::string("ftp://example.test/x")}) {
    peer->respond = firstThen(redirect(302, "/hop2"), redirect(302, target));
    auto running = fetchOutcome(peer->url("/hop1"));
    const Outcome outcome = co_await std::move(running);
    EXPECT_EQ(peer->scrub(outcome.error), verbose("UnsupportedRedirectProtocol", "http://127.0.0.1:PORT/hop1"));
  }
  // 127 requests, then TooManyRedirects.
  const std::size_t before = peer->requests.size();
  auto loops = std::make_shared<int>(0);
  peer->respond = [loops](std::shared_ptr<PeerConnection> connection, PeerRequest) {
    std::vector<Step> steps{bytes(redirect(302, "/loop" + std::to_string(++*loops)))};
    return play(std::move(connection), std::move(steps));
  };
  auto loop = fetchOutcome(peer->url("/loop"));
  const Outcome loopOutcome = co_await std::move(loop);
  EXPECT_EQ(loopOutcome.error, std::string("TypeError: The response redirected too many times. For more information, pass `verbose: true` in the second argument to fetch()"));
  EXPECT_EQ(loopOutcome.code, std::string("TooManyRedirects"));
  EXPECT_EQ(peer->requests.size() - before, std::size_t{127});
  // A stream body cannot follow 307/308; 303 drops it.
  for (const int status : {307, 303}) {
    UnderlyingSource<ByteChunk> source;
    source.start = [](ReadableStreamDefaultController<ByteChunk> controller) {
      controller.enqueue(ByteChunk{'s', 'b'});
      controller.close();
    };
    peer->respond = firstThen(redirect(status, "/next"), okBody("next"));
    RequestInit init{.method = "POST", .body = BodyInit(ByteStream(std::move(source))), .duplex = "half"};
    auto running = fetchOutcome(peer->url("/stream"), std::move(init));
    const Outcome outcome = co_await std::move(running);
    if (status == 307) {
      EXPECT_EQ(outcome.error, std::string("TypeError: Request body is a ReadableStream and cannot be replayed for this redirect"));
      EXPECT_EQ(outcome.code, std::string(""));
    } else {
      EXPECT_EQ(outcome.body, std::optional<std::string>("next"));
      EXPECT_EQ(peer->wires().back(), "GET /next HTTP/1.1\r\n" + kDefaults + "\r\n");
    }
  }
  peer->stop();
}

// ---- decompression ----------------------------------------------------------------------------

namespace {
  const std::string kPlain = [] {
    std::string text;
    for (int index = 0; index < 20; ++index) text += "hello hello hello compressed world ";
    return text;
  }();
  // node:zlib under Bun 1.4.0: gzipSync, deflateSync, deflateRawSync, brotliCompressSync, zstdCompressSync of kPlain.
  const std::string kGzip = hexBytes("1f8b0800000000000003cb48cdc9c957c8402293f3730b8a528b8b535314caf38b7252502447958c864bce60490c0067c9d769bc020000");
  const std::string kDeflate = hexBytes("789ccb48cdc9c957c8402293f3730b8a528b8b535314caf38b7252502447958c864bce60490c0049140884");
  const std::string kDeflateRaw = hexBytes("cb48cdc9c957c8402293f3730b8a528b8b535314caf38b7252502447958c864bce60490c00");
  const std::string kBrotli = hexBytes("1bbb0288043c64a93e77b3094c0fd2493c8d49d06117717b52dfa980a720");
  const std::string kZstd = hexBytes("28b52ffd60bc01250100b068656c6c6f20636f6d7072657373656420776f726c640400848caa000600a863852b45");
  const std::string kGzipTwoMembers = hexBytes("1f8b08000000000000034bcb2c2a2ed1050041064062060000001f8b08000000000000032b4e4dcecf4b010069111fb606000000");
  const std::string kZstdTwoFrames = hexBytes("28b52ffd2002110000612d28b52ffd200109000062");
  const std::string kGzipGzip = hexBytes("1f8b080000000000000393efe6600003e6d31e674f9e0c3fe1a034f97331775750777770b0c8a9cfdd4541012aee537bdabccf2578f230a49fbc9eb98789810100b781ab6637000000");

  std::string encoded(const std::string& coding, const std::string& body) {
    return "HTTP/1.1 200 OK\r\nContent-Encoding: " + coding + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
  }

  Task<Outcome> fetchEncoded(std::shared_ptr<Peer> peer, std::string response, std::string method) {
    peer->respond = answer(std::move(response));
    RequestInit init{.method = std::move(method)};
    auto running = fetchOutcome(peer->url("/"), std::move(init));
    Outcome outcome = co_await std::move(running);
    co_return outcome;
  }
}

ASYNC_TEST("response bodies are decompressed as Bun decompresses them, headers kept") {
  auto peer = Peer::start();
  TransportScope scope;
  struct Case {
    std::string name;
    std::string response;
    std::optional<std::string> body;
    std::string code;
  };
  const std::string chunkedGzip = [&] {
    std::string out = "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nTransfer-Encoding: chunked\r\n\r\n";
    for (std::size_t index = 0; index < kGzip.size(); index += 7) {
      const std::string piece = kGzip.substr(index, 7);
      char size[16];
      std::snprintf(size, sizeof size, "%zx\r\n", piece.size());
      out += size + piece + "\r\n";
    }
    return out + "0\r\n\r\n";
  }();
  std::vector<Case> cases{
      {"gzip", encoded("gzip", kGzip), kPlain, ""},
      {"gzip chunked", chunkedGzip, kPlain, ""},
      {"deflate (zlib)", encoded("deflate", kDeflate), kPlain, ""},
      {"deflate (raw)", encoded("deflate", kDeflateRaw), kPlain, ""},
      {"br", encoded("br", kBrotli), kPlain, ""},
      {"zstd", encoded("zstd", kZstd), kPlain, ""},
      {"x-gzip", encoded("x-gzip", kGzip), kPlain, ""},
      {"GZIP", encoded("GZIP", kGzip), kPlain, ""},
      {"BR", encoded("BR", kBrotli), kPlain, ""},
      {"tab gzip", encoded("\tgzip", kGzip), kPlain, ""},
      {"gzip, identity", encoded("gzip, identity", kGzip), kPlain, ""},
      {"identity, gzip", encoded("identity, gzip", kGzip), kPlain, ""},
      {"x-gzip, identity", encoded("x-gzip, identity", kGzip), kPlain, ""},
      {"two fields gzip + identity", "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Encoding: identity\r\nContent-Length: " + std::to_string(kGzip.size()) + "\r\n\r\n" + kGzip, kPlain, ""},
      {"identity", encoded("identity", kPlain), kPlain, ""},
      {"gzip two members", encoded("gzip", kGzipTwoMembers), std::string("first-second"), ""},
      {"zstd two frames", encoded("zstd", kZstdTwoFrames), std::string("a-b"), ""},
      {"gzip then garbage", encoded("gzip", kGzip + "XYZ"), kPlain, ""},
      {"deflate then garbage", encoded("deflate", kDeflate + "XYZ"), kPlain, ""},
      {"br then garbage", encoded("br", kBrotli + "XYZ"), kPlain, ""},
      {"empty gzip body", encoded("gzip", ""), std::string(""), ""},
      {"empty deflate body", encoded("deflate", ""), std::string(""), ""},
      {"empty br body", encoded("br", ""), std::string(""), ""},
      {"empty zstd body", encoded("zstd", ""), std::string(""), ""},
      {"204 with gzip", "HTTP/1.1 204 No Content\r\nContent-Encoding: gzip\r\n\r\n", std::string(""), ""},
      {"gzip corrupt", encoded("gzip", kGzip.substr(0, 12) + "garbagegarbage" + kGzip.substr(26)), std::nullopt, "ZlibError"},
      {"gzip that is not gzip", encoded("gzip", "plain text not gzip"), std::nullopt, "ZlibError"},
      {"gzip truncated", encoded("gzip", kGzip.substr(0, kGzip.size() - 6)), std::nullopt, "ZlibError"},
      {"gzip bad CRC", encoded("gzip", kGzip.substr(0, kGzip.size() - 8) + std::string(4, '\0') + kGzip.substr(kGzip.size() - 4)), std::nullopt, "ZlibError"},
      {"gzip header only", encoded("gzip", kGzip.substr(0, 10)), std::nullopt, "ZlibError"},
      {"gzip then 0x1f", encoded("gzip", kGzip + "\x1f"), std::nullopt, "ZlibError"},
      {"deflate corrupt", encoded("deflate", "\x78\x9c\xff\xff\xff"), std::nullopt, "ZlibError"},
      {"deflate truncated", encoded("deflate", kDeflate.substr(0, kDeflate.size() - 4)), std::nullopt, "ZlibError"},
      {"br corrupt", encoded("br", "not brotli at all"), std::nullopt, "BrotliDecompressionError"},
      {"br truncated", encoded("br", kBrotli.substr(0, kBrotli.size() - 3)), std::nullopt, "BrotliDecompressionError"},
      {"zstd corrupt", encoded("zstd", "not zstd data at all"), std::nullopt, "ZstdDecompressionError"},
      {"zstd truncated", encoded("zstd", kZstd.substr(0, kZstd.size() - 5)), std::nullopt, "ZstdDecompressionError"},
      {"zstd then garbage", encoded("zstd", kZstd + "XYZ"), std::nullopt, "ZstdDecompressionError"},
  };
  for (const Case& item : cases) {
    scope.transport->closeIdleConnections();
    auto running = fetchEncoded(peer, item.response, "GET");
    const Outcome outcome = co_await std::move(running);
    std::printf("      case: %s\n", item.name.c_str());
    EXPECT_EQ(outcome.status, std::optional<int>(item.name == "204 with gzip" ? 204 : 200));
    EXPECT_EQ(outcome.body, item.body);
    if (!item.code.empty()) {
      EXPECT_EQ(peer->scrub(outcome.error), verbose(item.code, "http://127.0.0.1:PORT/"));
      EXPECT_EQ(outcome.code, item.code);
    }
  }
  // Codings Bun does not pick pass the bytes through, and the headers stay as received.
  for (const std::string coding : {"gzip, gzip", "deflate, gzip", "gzip;q=1", "compress, gzip", "gzipx", "x-deflate", ""}) {
    scope.transport->closeIdleConnections();
    const std::string& payload = coding == "gzip, gzip" ? kGzipGzip : kGzip;
    peer->respond = answer(encoded(coding, payload));
    auto calling = fetch(RequestInfo(peer->url("/raw")));
    const Response response = co_await std::move(calling);
    auto reading = response.bytes();
    const Bytes body = co_await std::move(reading);
    EXPECT(std::string(body.begin(), body.end()) == payload);
    EXPECT_EQ(response.headers().get("content-encoding"), std::optional<std::string>(coding));
  }
  scope.transport->closeIdleConnections();
  auto headed = fetchEncoded(peer, encoded("gzip", kGzip), "HEAD");
  const Outcome headOutcome = co_await std::move(headed);
  EXPECT_EQ(headOutcome.body, std::optional<std::string>(""));
  scope.transport->closeIdleConnections();
  auto kept = fetchEncoded(peer, encoded("gzip", kGzip), "GET");
  const Outcome keptOutcome = co_await std::move(kept);
  EXPECT_EQ(describe(keptOutcome.headers), "content-encoding: gzip\ncontent-length: " + std::to_string(kGzip.size()) + "\n");
  peer->stop();
}

ASYNC_TEST("Fetch enforces an SSE line budget across socket reads") {
  auto peer = Peer::start();
  TransportScope scope;
  for (const std::size_t extra : {0u, 1u}) {
    std::string data = "data: ";
    data.resize(rt::limits::kSseLineBytes + extra, 'a');
    data += "\r\n\r\ndata: done\n\n";
    peer->respond = answer("HTTP/1.1 200 OK\r\nContent-Type: Text/Event-Stream; charset=utf-8\r\nContent-Length: " +
                           std::to_string(data.size()) + "\r\n\r\n" + data);
    auto reading = fetchOutcome(peer->url("/events"));
    const Outcome outcome = co_await std::move(reading);
    if (extra == 0) {
      ASSERT(outcome.body);
      EXPECT(*outcome.body == data);
    } else EXPECT_EQ(outcome.code, "ResponseSseLineTooLarge");
    scope.transport->closeIdleConnections();
  }
  peer->stop();
}

ASYNC_TEST("Fetch admits the last informational head and rejects one beyond it") {
  auto peer = Peer::start();
  TransportScope scope;
  for (const std::size_t extra : {0u, 1u}) {
    std::string response;
    for (std::size_t i = 0; i < rt::limits::kHttpInformationalResponses + extra; ++i) response += "HTTP/1.1 103 Early Hints\r\n\r\n";
    peer->respond = answer(response + kOk);
    auto reading = fetchOutcome(peer->url("/hints"));
    const Outcome outcome = co_await std::move(reading);
    if (extra == 0) EXPECT_EQ(outcome.body, std::optional<std::string>("ok"));
    else EXPECT_EQ(outcome.code, "TooManyInformationalResponses");
    scope.transport->closeIdleConnections();
  }
  peer->stop();
}

// ---- TLS --------------------------------------------------------------------------------------

ASYNC_TEST("https: SNI, ALPN, certificate and host name checks with Bun's errors") {
  auto localhost = Peer::start(serverTls(kLocalhostCert, kLocalhostKey));
  localhost->respond = answer("HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsecure");
  {
    TransportScope scope;
    auto untrusted = fetchOutcome(localhost->host("localhost", "/"));
    const Outcome outcome = co_await std::move(untrusted);
    EXPECT_EQ(outcome.error, std::string("TypeError: self signed certificate"));
    EXPECT_EQ(outcome.code, std::string("DEPTH_ZERO_SELF_SIGNED_CERT"));
    auto untrustedIp = fetchOutcome(localhost->url("/"));
    const Outcome ipOutcome = co_await std::move(untrustedIp);
    EXPECT_EQ(ipOutcome.code, std::string("DEPTH_ZERO_SELF_SIGNED_CERT"));
  }
  {
    HttpClientOptions options;
    options.ca = kLocalhostCert;
    TransportScope scope(options);
    auto trusted = fetchOutcome(localhost->host("localhost", "/"));
    const Outcome outcome = co_await std::move(trusted);
    EXPECT_EQ(outcome.body, std::optional<std::string>("secure"));
    auto trustedIp = fetchOutcome(localhost->url("/"));
    const Outcome ipOutcome = co_await std::move(trustedIp);
    EXPECT_EQ(ipOutcome.body, std::optional<std::string>("secure"));
    EXPECT_EQ(localhost->serverNames, (std::vector<std::string>{"localhost", "<none>", "localhost", "<none>"}));
    for (const auto& offer : localhost->alpnOffers) EXPECT_EQ(offer, (std::vector<std::string>{"http/1.1"}));
    // Keep-alive over TLS.
    auto again = fetchOutcome(localhost->host("localhost", "/again"));
    const Outcome againOutcome = co_await std::move(again);
    EXPECT_EQ(againOutcome.body, std::optional<std::string>("secure"));
    EXPECT_EQ(lastConnections(*localhost, 3), (std::vector<int>{2, 3, 2}));
  }
  {
    HttpClientOptions options;
    options.rejectUnauthorized = false;
    TransportScope scope(options);
    auto accepted = fetchOutcome(localhost->host("localhost", "/"));
    const Outcome outcome = co_await std::move(accepted);
    EXPECT_EQ(outcome.body, std::optional<std::string>("secure"));
  }
  localhost->stop();
  auto other = Peer::start(serverTls(kOtherCert, kOtherKey));
  other->respond = answer(kOk);
  {
    HttpClientOptions options;
    options.ca = kOtherCert;
    TransportScope scope(options);
    for (const std::string& url : {other->host("localhost", "/"), other->url("/")}) {
      auto running = fetchOutcome(url);
      const Outcome outcome = co_await std::move(running);
      EXPECT_EQ(outcome.error, verbose("ERR_TLS_CERT_ALTNAME_INVALID", url));
      EXPECT_EQ(outcome.code, std::string("ERR_TLS_CERT_ALTNAME_INVALID"));
    }
  }
  other->stop();
  auto expired = Peer::start(serverTls(kExpiredCert, kExpiredKey));
  expired->respond = answer(kOk);
  {
    HttpClientOptions options;
    options.ca = kExpiredCert;
    TransportScope scope(options);
    auto running = fetchOutcome(expired->host("localhost", "/"));
    const Outcome outcome = co_await std::move(running);
    EXPECT_EQ(outcome.error, std::string("TypeError: certificate has expired"));
    EXPECT_EQ(outcome.code, std::string("CERT_HAS_EXPIRED"));
  }
  expired->stop();
}

ASYNC_TEST("https: handshake failures") {
  TransportScope scope;
  // A plain HTTP server and a server closing at once look the same to Bun.
  auto plain = Peer::start();
  plain->open = [](std::shared_ptr<PeerConnection> connection) -> Task<bool> {
    std::vector<char> hello(4096);
    auto receiving = connection->readSome(hello.data(), hello.size());
    co_await std::move(receiving);
    auto sending = connection->send("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    co_await std::move(sending);
    co_return false;
  };
  auto closer = Peer::start();
  closer->open = [](std::shared_ptr<PeerConnection> connection) -> Task<bool> {
    connection->end();
    co_return false;
  };
  for (const std::shared_ptr<Peer>& peer : {plain, closer}) {
    auto running = fetchOutcome("https://localhost:" + std::to_string(peer->port) + "/");
    const Outcome outcome = co_await std::move(running);
    EXPECT_EQ(outcome.error, std::string("TypeError: unknown certificate verification error"));
    EXPECT_EQ(outcome.code, std::string("UNKNOWN_CERTIFICATE_VERIFICATION_ERROR"));
  }
  auto resetter = Peer::start();
  resetter->open = [](std::shared_ptr<PeerConnection> connection) -> Task<bool> {
    auto waiting = rt::sleep(50);
    co_await std::move(waiting);
    connection->reset();
    co_return false;
  };
  auto running = fetchOutcome("https://localhost:" + std::to_string(resetter->port) + "/");
  const Outcome outcome = co_await std::move(running);
  EXPECT_EQ(outcome.error, kClosed);
  EXPECT_EQ(outcome.code, std::string("ECONNRESET"));
  plain->stop();
  closer->stop();
  resetter->stop();
}

// ---- network errors ---------------------------------------------------------------------------

ASYNC_TEST("connection refused, DNS failure and a reset in the middle of the body") {
  TransportScope scope;
  for (const char* url : {"http://127.0.0.1:1/", "https://127.0.0.1:1/", "http://localhost:1/"}) {
    auto running = fetchOutcome(url);
    const Outcome outcome = co_await std::move(running);
    EXPECT_EQ(outcome.error, std::string("TypeError: Unable to connect. Is the computer able to access the url?"));
    EXPECT_EQ(outcome.code, std::string("ConnectionRefused"));
  }
  // A 64-character label fails in the resolver itself, without a query.
  const std::string host = std::string(64, 'a') + ".invalid";
  Reason failure;
  try {
    auto calling = fetch(RequestInfo("http://" + host + "/"));
    co_await std::move(calling);
  } catch (...) {
    failure = std::current_exception();
  }
  try {
    std::rethrow_exception(failure);
  } catch (const FetchError& error) {
    EXPECT_EQ(error.name, std::string("TypeError"));
    EXPECT_EQ(error.message, "getaddrinfo ENOTFOUND " + host);
    EXPECT_EQ(error.code, std::optional<std::string>("ENOTFOUND"));
    EXPECT_EQ(error.errno_, 4);
    EXPECT_EQ(error.syscall, std::optional<std::string>("getaddrinfo"));
    EXPECT_EQ(error.hostname, std::optional<std::string>(host));
    EXPECT_EQ(error.path, "http://" + host + "/");
  } catch (...) {
    EXPECT(false);
  }
  auto peer = Peer::start();
  peer->respond = script({bytes("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\npartial"), pause(30), rst()});
  auto calling = fetch(RequestInfo(peer->url("/")));
  const Response response = co_await std::move(calling);
  auto reader = response.body()->getReader();
  auto reading = reader.read();
  const ReadableStreamReadResult<ByteChunk> first = co_await std::move(reading);
  EXPECT_EQ(std::string(first.value->begin(), first.value->end()), std::string("partial"));
  auto next = reader.read();
  const std::string closedMessage = kClosed.substr(11);
  EXPECT_REJECTS_MESSAGE(std::move(next), FetchError, closedMessage.c_str());
  peer->stop();
}

// ---- installation -------------------------------------------------------------------------------

ASYNC_TEST("installHttpClientTransport makes defaultFetch use the network client") {
  const std::shared_ptr<FetchTransport> previous = fetchTransport();
  auto peer = Peer::start();
  peer->respond = answer(kOk);
  const std::shared_ptr<HttpClientTransport> installed = installHttpClientTransport();
  EXPECT(fetchTransport() == installed);
  auto calling = defaultFetch()(RequestInfo(peer->url("/installed")));
  const Response response = co_await std::move(calling);
  auto reading = response.text();
  const std::string text = co_await std::move(reading);
  EXPECT_EQ(text, std::string("ok"));
  setFetchTransport(previous);
  peer->stop();
}
