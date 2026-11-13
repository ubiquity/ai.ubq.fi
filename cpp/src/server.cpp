#include "pilot/network.hpp"
#include <algorithm>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <iostream>
namespace pilot {
  Runtime::Runtime(asio::io_context& context,Config c):io(context),config(std::move(c)),state(config),acceptor(io),control(std::make_shared<Control>(io,config,state,counters,[this] {
    stop(true);
  })),drain_timer(io) {
    tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"),config.port);
    acceptor.open(endpoint.protocol());
    acceptor.set_option(tcp::acceptor::reuse_address(true));
    acceptor.bind(endpoint);
    acceptor.listen(8);
  } asio::awaitable<void> Runtime::accept_loop() {
    while(!stopping) {
      boost::system::error_code ec;
      auto socket=co_await acceptor.async_accept(asio::redirect_error(asio::use_awaitable,ec));
      if(ec) {
        if(stopping)co_return;
        throw boost::system::system_error(ec);
      }
      if(sockets>=8) {
        socket.close(ec);
        continue;
      }
      ++sockets;
      auto session=std::make_shared<Session>(*this,std::move(socket));
      sessions.erase(std::remove_if(sessions.begin(),sessions.end(),[](auto const& p) {
        return p.expired();
      }),sessions.end());
      sessions.push_back(session);
      asio::co_spawn(io,session->run(),[session](std::exception_ptr error){
        if(error)session->cancel();
      });
    }
  } void Runtime::drained(){
    if(stopping&&sockets==0){drain_timer.cancel();if(on_drained)on_drained();}
  }
  void Runtime::stop(bool force){
    if(stopping){if(force)for(auto const& p:sessions)if(auto session=p.lock())session->cancel();return;}
    stopping=true;
    boost::system::error_code ec;
    acceptor.close(ec);
    control->stop();
    if(force)for(auto const& p:sessions)if(auto session=p.lock())session->cancel();
    drain_timer.expires_after(std::chrono::seconds(15));
    drain_timer.async_wait([this](boost::system::error_code error){if(!error)for(auto const& p:sessions)if(auto session=p.lock())session->cancel();});
    drained();
  }
}
