#include "pilot/network.hpp"
#include "pilot/transport.hpp"
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <array>
#include <limits>
namespace pilot {
namespace http=boost::beast::http;
struct Upstream::Impl {
  Config const& config;
  Transport transport;
  http::response_parser<http::buffer_body> parser;
  std::string pending,ready;
  bool eof=false;
  Impl(asio::io_context& io,Config const& c,IoCounters& counters):config(c),transport(io,c,counters){parser.header_limit(16384);parser.body_limit(std::numeric_limits<std::uint64_t>::max());parser.eager(true);}
  asio::awaitable<void> pump(bool headers_only=false){
    std::array<char,8192> network{},body{};
    for(;;){
      if(!pending.empty()){
        parser.get().body().data=body.data();parser.get().body().size=body.size();
        boost::system::error_code ec;
        auto used=parser.put(asio::buffer(pending),ec);pending.erase(0,used);
        auto produced=body.size()-parser.get().body().size;
        if(produced)ready.append(body.data(),produced);
        if(ec&&ec!=http::error::need_more&&ec!=http::error::need_buffer)throw boost::system::system_error(ec);
        if(produced||parser.is_done()||(headers_only&&parser.is_header_done()))co_return;
      }
      if(eof){boost::system::error_code ec;parser.put_eof(ec);if(ec)throw boost::system::system_error(ec);co_return;}
      auto n=co_await transport.read(asio::buffer(network));
      eof=n==0;pending.append(network.data(),n);
      if(pending.size()>32768)throw Failure(502,"Upstream header buffer exceeded bound");
    }
  }
};
Upstream::Upstream(asio::io_context& io,Config const& c,IoCounters& counters):impl_(std::make_shared<Impl>(io,c,counters)){}
Upstream::~Upstream(){impl_->transport.cancel();}
asio::awaitable<void> Upstream::open(std::string body){
  auto p=impl_;co_await p->transport.connect();
  std::string wire="POST "+p->config.upstream_target+" HTTP/1.1\r\nHost: "+p->config.upstream_host+"\r\nAuthorization: Bearer "+p->config.upstream_key+"\r\nContent-Type: application/json\r\nAccept: application/json\r\nConnection: close\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
  std::string{}.swap(body);co_await p->transport.write(std::move(wire));
  p->transport.deadline(std::chrono::steady_clock::now()+p->config.limits.headers);
  while(!p->parser.is_header_done()&&!p->eof)co_await p->pump(true);
  if(!p->parser.is_header_done())throw Failure(502,"Upstream closed before headers","deepseek_upstream_unreachable");
  reset_frame_deadline();
}
asio::awaitable<std::string> Upstream::read(){
  auto p=impl_;
  if(!p->ready.empty()){std::string result;result.swap(p->ready);co_return result;}
  while(!p->parser.is_done()){co_await p->pump();if(!p->ready.empty()){std::string result;result.swap(p->ready);co_return result;}if(p->eof)break;}
  co_return std::string{};
}
int Upstream::status()const{return static_cast<int>(impl_->parser.get().result_int());}
std::string Upstream::header(std::string_view key)const{auto const& headers=impl_->parser.get().base();auto found=headers.find(boost::beast::string_view(key.data(),key.size()));return found==headers.end()?"":std::string(found->value());}
void Upstream::reset_frame_deadline(){impl_->transport.deadline(std::chrono::steady_clock::now()+impl_->config.limits.frame);}
void Upstream::cancel(){impl_->transport.cancel();}
}
