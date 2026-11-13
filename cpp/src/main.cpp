#include "pilot/network.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/signal_set.hpp>
#include <csignal>
#include <iostream>
int main(int argc,char** argv) {
  std::signal(SIGPIPE,SIG_IGN);
  try {
    if(argc!=3||std::string_view(argv[1])!="--config")throw std::runtime_error("usage: uos_cpp_pilot --config <private-fixture-config.json>");
    auto config=pilot::read_config(argv[2]);
    boost::asio::io_context io(1);
    pilot::Runtime runtime(io,std::move(config));
    boost::asio::signal_set signals(io,SIGINT,SIGTERM);
    runtime.on_drained = [&]{ boost::system::error_code error; signals.cancel(error); };
    signals.async_wait([&](boost::system::error_code ec,int) {
      if(!ec)runtime.stop();
    });
    boost::asio::co_spawn(io,runtime.accept_loop(),[&](std::exception_ptr e) {
      if(e) {
        std::cerr<<"Listener failed\n";runtime.stop();
      }
    });
    if(runtime.control->enabled()) {
      auto owner=runtime.control;
      boost::asio::co_spawn(io,owner->run(),[owner](std::exception_ptr error){if(error)owner->stop();});
    }
    std::cout<<boost::json::serialize(boost::json::object {
      {
        "event","ready"
      }, {
        "endpoint","http://127.0.0.1:"+std::to_string(runtime.acceptor.local_endpoint().port())
      }, {
        "fixture_authority",true
      }, {
        "provider","deepseek"
      }, {
        "model","deepseek-flash"
      }
    })<<std::endl;
    io.run();
    return 0;
  }
  catch(std::exception const& e) {
    std::cerr<<"Pilot startup failed: "<<e.what()<<"\n";
    return 1;
  }
}
