#pragma once
#include "pilot/state.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <functional>
namespace pilot {
  namespace asio=boost::asio;
  class Control:public std::enable_shared_from_this<Control> {
    State& state_;
    IoCounters& counters_;
    asio::posix::stream_descriptor input_,output_;
    asio::steady_timer fence_timer_,send_gate_,write_deadline_;
    std::function<void()> stop_;
    bool enabled_=false,armed_=false,held_=false,stopped_=false,sending_=false,send_waiter_=false;
    std::uint64_t last_id_=0,fence_id_=0,ordinal_=0;
    std::size_t commands_=0;
    std::string phase_,request_id_,key_id_;
    asio::awaitable<void> send(json::object message);
    asio::awaitable<json::object> read_frame();
    public: Control(asio::io_context& io,Config const& config,State& state,IoCounters& counters,std::function<void()> stop);
    asio::awaitable<void> run();
    asio::awaitable<void> fence(std::uint64_t ordinal,std::string_view phase,std::string_view request_id="");
    void stop();
    bool enabled() const {
      return enabled_;
    }
  };
}
