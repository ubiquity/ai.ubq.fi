#pragma once
#include "pilot/core.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/buffer.hpp>
namespace pilot {
class Transport {
  struct Impl;
  std::shared_ptr<Impl> impl_;
public:
  Transport(boost::asio::io_context& io,Config const& config,IoCounters& counters);
  ~Transport();
  boost::asio::awaitable<void> connect();
  boost::asio::awaitable<void> write(std::string value);
  boost::asio::awaitable<std::size_t> read(boost::asio::mutable_buffer buffer);
  void deadline(std::chrono::steady_clock::time_point time);
  void cancel();
};
}
