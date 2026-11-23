#pragma once
// Loopback HTTP peers for the tests of src/runtime/http.ts (no TS counterpart: the bun
// tests use Bun's fetch, node:http's client and node:net, which rt does not provide).
//
// - ScopedLoopbackFetch installs an rt::FetchTransport speaking HTTP/1.1 over TCP to
//   numeric hosts for the scope, so `rt::fetch("http://127.0.0.1:<port>/...")` reaches a
//   real server. Each request uses its own connection (sent with Connection: keep-alive,
//   as Bun's fetch sends it), closed once the response body ended; bodies are sent with
//   Content-Length (respecting an explicit length); an explicit Transfer-Encoding: chunked
//   sends the request stream with real chunk frames and no Content-Length. Responses are read by Content-Length, chunked framing or until close.
// - RawClient is node:net's `connect({ host, port })` as the tests drive it: "connect",
//   "data" (each read as a Latin-1 string), "error" and "close" listeners, write(),
//   pause()/resume() (no reads while paused) and destroy().
#include "uos/rt/async.hpp"
#include "uos/rt/fetch.hpp"

#include <boost/asio/ip/tcp.hpp>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace uos::tests::helpers::loopback_http {
  class ScopedLoopbackFetch {
  public:
    ScopedLoopbackFetch();
    ~ScopedLoopbackFetch();
    ScopedLoopbackFetch(const ScopedLoopbackFetch&) = delete;
    ScopedLoopbackFetch& operator=(const ScopedLoopbackFetch&) = delete;

  private:
    std::shared_ptr<rt::FetchTransport> previous_;
  };

  class RawClient : public std::enable_shared_from_this<RawClient> {
  public:
    using Handler = std::function<void(const std::string& data)>;

    // `connect({ host, port })`: connecting starts now; "connect" follows.
    static std::shared_ptr<RawClient> connect(int port, std::string host = "127.0.0.1");
    explicit RawClient(boost::asio::ip::tcp::socket socket);

    void on(const std::string& event, Handler handler);
    void once(const std::string& event, Handler handler);
    void write(std::string data);
    void pause();
    void resume();
    void destroy();
    bool destroyed() const { return destroyed_; }

  private:
    struct Entry {
      Handler handler;
      bool once = false;
    };
    using Listeners = std::map<std::string, std::vector<Entry>>;
    void emit(const std::string& event, const std::string& data);
    void startConnect(boost::asio::ip::tcp::endpoint endpoint);
    void read();
    void flush();
    void closeWith(const std::string& error);

    boost::asio::ip::tcp::socket socket_;
    Listeners listeners_;
    std::weak_ptr<Listeners> closingListeners_;
    std::vector<char> buffer_ = std::vector<char>(65536);
    std::deque<std::string> output_;
    bool connected_ = false;
    bool reading_ = false;
    bool writing_ = false;
    bool paused_ = false;
    bool destroyed_ = false;
    bool closeEmitted_ = false;
  };
}
