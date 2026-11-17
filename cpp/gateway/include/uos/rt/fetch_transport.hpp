#pragma once
// The network client behind rt::defaultFetch(): Bun 1.4's fetch transport over
// HTTP/1.1 (TCP or TLS), with its keep-alive pool, stale-connection retry,
// redirects, response decompression and error texts. Reference:
// docs/rt/fetch-transport.md.
#include "uos/rt/error.hpp"
#include "uos/rt/fetch.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace uos::rt {
  // The TypeError Bun's fetch rejects with, or errors a response body with, on
  // a network or protocol failure. `code` (JsError::code) is Bun's code
  // ("ConnectionRefused", "ECONNRESET", "ENOTFOUND", "ZlibError", ...), `path`
  // the requested URL and `errno_` Bun's errno (4 for ENOTFOUND, else 0). A DNS
  // failure also carries `syscall` ("getaddrinfo") and `hostname`.
  struct FetchError : TypeError {
    std::string path;
    int errno_ = 0;
    std::optional<std::string> syscall;
    std::optional<std::string> hostname;
    FetchError(std::string message_, std::string code_, std::string path_) : TypeError(std::move(message_)), path(std::move(path_)) { code = std::move(code_); }
  };

  struct HttpClientOptions {
    // The User-Agent sent when the request has none. std::nullopt is Bun's
    // "Bun/<version>" (rt::bun::version).
    std::optional<std::string> userAgent{};
    // Idle keep-alive connections kept for http: and, separately, for https:
    // (Bun keeps 64 in each of its two HTTP contexts).
    std::size_t maxIdleConnections = 64;
    // How long a connection stays pooled while idle (Bun: 300 seconds).
    double keepAliveTimeoutMs = 300000;
    // Bun's fetch timeout: a request whose connection sees no activity for
    // this long fails with DOMException TimeoutError "The operation timed
    // out." (before the response) or errors the body with it (after).
    double timeoutMs = 300000;
    // PEM certificates trusted instead of the default store (Bun's `tls.ca`).
    // std::nullopt: the system store plus NODE_EXTRA_CA_CERTS, as Bun.
    std::optional<std::string> ca{};
    // false accepts any certificate (Bun's `tls.rejectUnauthorized: false`).
    // NODE_TLS_REJECT_UNAUTHORIZED=0 in the environment has the same effect.
    bool rejectUnauthorized = true;
  };

  namespace detail::fetch {
    struct ClientState;
  }

  // rt::FetchTransport speaking HTTP/1.1 to the network on the current rt loop.
  // Connections are pooled per loop and released when that loop ends; idle
  // connections never keep a loop running.
  class HttpClientTransport final : public FetchTransport {
  public:
    HttpClientTransport();
    explicit HttpClientTransport(HttpClientOptions options);
    ~HttpClientTransport() override;
    HttpClientTransport(const HttpClientTransport&) = delete;
    HttpClientTransport& operator=(const HttpClientTransport&) = delete;

    [[nodiscard]] Task<Response> send(Request request) override;
    // Idle pooled connections on the current loop.
    std::size_t idleConnections() const;
    // Closes the idle pooled connections on the current loop.
    void closeIdleConnections();

  private:
    std::shared_ptr<detail::fetch::ClientState> state_;
  };

  // Makes an HttpClientTransport the transport rt::defaultFetch() sends
  // through (serve's main calls it once at start-up) and returns it.
  std::shared_ptr<HttpClientTransport> installHttpClientTransport();
  std::shared_ptr<HttpClientTransport> installHttpClientTransport(HttpClientOptions options);
}
