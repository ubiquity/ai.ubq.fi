#include "pilot/transport.hpp"
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <openssl/ssl.h>
namespace pilot {
namespace asio=boost::asio;
using tcp=asio::ip::tcp;
struct Transport::Impl:std::enable_shared_from_this<Impl>{
  Config const& config;
  IoCounters& counters;
  tcp::resolver resolver;
  tcp::socket plain;
  asio::ssl::context context{asio::ssl::context::tls_client};
  std::unique_ptr<asio::ssl::stream<tcp::socket>> tls;
  asio::steady_timer timer;
  Impl(asio::io_context& io,Config const& c,IoCounters& counts):config(c),counters(counts),resolver(io),plain(io),timer(io){
    if(c.tls){
      context.set_verify_mode(asio::ssl::verify_peer);
      context.load_verify_file(c.ca_file);
      tls=std::make_unique<asio::ssl::stream<tcp::socket>>(io,context);
      tls->set_verify_callback(asio::ssl::host_name_verification(c.upstream_host));
      if(SSL_set_tlsext_host_name(tls->native_handle(),c.upstream_host.c_str())!=1)throw std::runtime_error("TLS SNI setup failed");
    }
  }
  tcp::socket& socket(){return tls?tls->next_layer():plain;}
  void cancel(){resolver.cancel();timer.cancel();boost::system::error_code ec;socket().cancel(ec);socket().close(ec);}
  void deadline(std::chrono::steady_clock::time_point at){
    timer.expires_at(at);
    std::weak_ptr<Impl> weak=shared_from_this();
    timer.async_wait([weak](boost::system::error_code ec){if(!ec)if(auto owner=weak.lock())owner->cancel();});
  }
};
Transport::Transport(asio::io_context& io,Config const& c,IoCounters& counters):impl_(std::make_shared<Impl>(io,c,counters)){}
Transport::~Transport(){impl_->cancel();}
asio::awaitable<void> Transport::connect(){
  auto p=impl_;
  p->deadline(std::chrono::steady_clock::now()+p->config.limits.connect);
  auto endpoints=co_await p->resolver.async_resolve(p->config.upstream_host,p->config.upstream_port,asio::use_awaitable);
  p->deadline(std::chrono::steady_clock::now()+p->config.limits.connect);
  co_await asio::async_connect(p->socket(),endpoints,asio::use_awaitable);
  if(p->tls){p->deadline(std::chrono::steady_clock::now()+p->config.limits.connect);co_await p->tls->async_handshake(asio::ssl::stream_base::client,asio::use_awaitable);}
  p->timer.cancel();
}
asio::awaitable<void> Transport::write(std::string value){
  auto p=impl_;
  p->deadline(std::chrono::steady_clock::now()+p->config.limits.write);
  if(p->tls)co_await asio::async_write(*p->tls,asio::buffer(value),asio::use_awaitable);
  else co_await asio::async_write(p->plain,asio::buffer(value),asio::use_awaitable);
  p->timer.cancel();
}
asio::awaitable<std::size_t> Transport::read(asio::mutable_buffer buffer){
  auto p=impl_;auto& io=p->counters;++io.upstream_read_starts;++io.pending_upstream_reads;
  struct ReadCompletion { IoCounters& io; ~ReadCompletion(){++io.upstream_read_completions;--io.pending_upstream_reads;} } completion{io};
  boost::system::error_code ec;std::size_t n;
  if(p->tls)n=co_await p->tls->async_read_some(buffer,asio::redirect_error(asio::use_awaitable,ec));
  else n=co_await p->plain.async_read_some(buffer,asio::redirect_error(asio::use_awaitable,ec));
  if(ec==asio::error::eof||ec==asio::ssl::error::stream_truncated)co_return 0;
  if(ec)throw boost::system::system_error(ec);
  co_return n;
}
void Transport::deadline(std::chrono::steady_clock::time_point time){impl_->deadline(time);}
void Transport::cancel(){impl_->cancel();}
}
