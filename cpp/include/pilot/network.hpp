#pragma once
#include "pilot/control.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <memory>
namespace pilot {
  using tcp=asio::ip::tcp;
  class Upstream {
    struct Impl;
    std::shared_ptr<Impl> impl_;
    public: Upstream(asio::io_context& io,Config const& config,IoCounters& counters);
    ~Upstream();
    asio::awaitable<void> open(std::string body);
    asio::awaitable<std::string> read();
    int status() const;
    std::string header(std::string_view key) const;
    void reset_frame_deadline();
    void cancel();
  };
  class Session;
  struct Runtime {
    asio::io_context& io;
    Config config;
    State state;
    tcp::acceptor acceptor;
    IoCounters counters;
    std::shared_ptr<Control> control;
    unsigned sockets=0,active=0;
    std::uint64_t ordinal=0;
    bool stopping=false;
  std::function<void()> on_drained;
  asio::steady_timer drain_timer;
    std::vector<std::weak_ptr<Session>> sessions;
    Runtime(asio::io_context& io,Config config);
    void stop(bool force=false);
    void drained();
    asio::awaitable<void> accept_loop();
  };
  class Session:public std::enable_shared_from_this<Session> {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    public: Session(Runtime& runtime,tcp::socket socket);
    ~Session();
    asio::awaitable<void> run();
    void cancel();
  };
}
