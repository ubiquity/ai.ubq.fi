#include "pilot/network.hpp"
#include "pilot/translator.hpp"
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_static_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <sys/socket.h>
#include <cerrno>
#include <array>
#include <iostream>
#include <sstream>
namespace pilot {
  namespace http=boost::beast::http;
  struct Session::Impl {
    Runtime& runtime;
    boost::beast::tcp_stream downstream;
    boost::beast::flat_static_buffer<32768> buffer;
    asio::steady_timer watch_join;
    std::unique_ptr<Upstream> upstream;
    bool cancelled=false,watch_pending=false,permit=false,headers_sent=false,transport=false,delivered=false,dispatch_intent=false,native_headers=false;
    std::string id=random_id("cpp_"), response_id;
    std::uint64_t ordinal=0,sequence=0;
    std::size_t event_count=0;
    std::string semantic="cancelled";
    json::value usage;
    std::shared_ptr<AllocationGroup> budget=std::make_shared<AllocationGroup>();
    BudgetResource input_resource {
      budget,8*1024*1024
    },projection_resource {
      budget,8*1024*1024
    },output_resource {
      budget,4*1024*1024
    },event_resource {
      budget,4*1024*1024
    };
    std::size_t external_reserved=0;
    void reserve_external(std::size_t bytes){if(bytes>budget->limit-budget->current+external_reserved)throw std::bad_alloc();budget->current=budget->current-external_reserved+bytes;external_reserved=bytes;budget->peak=std::max(budget->peak,budget->current);}
    explicit Impl(Runtime& r,tcp::socket socket):runtime(r),downstream(std::move(socket)),watch_join(r.io) {
      reserve_external(262144);
      asio::socket_base::send_buffer_size send_buffer(16384);
      downstream.socket().set_option(send_buffer);
      int actual_send_buffer=0;
      socklen_t option_size=sizeof(actual_send_buffer);
      if(::getsockopt(downstream.socket().native_handle(),SOL_SOCKET,SO_SNDBUF,&actual_send_buffer,&option_size)!=0)throw std::runtime_error("Kernel send buffer readback failed");
      runtime.counters.actual_downstream_send_buffer_bytes=std::max(runtime.counters.actual_downstream_send_buffer_bytes,static_cast<std::uint64_t>(actual_send_buffer));
    } ~Impl(){budget->current-=external_reserved;}
    bool peer_closed() {
      char c;
      auto n=::recv(downstream.socket().native_handle(),&c,1,MSG_PEEK|MSG_DONTWAIT);
      return n==0||(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR);
    } void cancel() {
      if(cancelled)return;
      cancelled=true;
      if(upstream)upstream->cancel();
      boost::system::error_code ec;
      downstream.socket().cancel(ec);
      downstream.socket().close(ec);
    } asio::awaitable<void> write(std::string wire) {
      if(cancelled)throw Failure(499,"Request was cancelled.","request_cancelled");
      auto& io=runtime.counters;++io.downstream_write_starts;++io.pending_downstream_writes;
      struct WriteCompletion { IoCounters& io; ~WriteCompletion(){++io.downstream_write_completions;--io.pending_downstream_writes;} } completion{io};
      downstream.expires_after(runtime.config.limits.write);
      co_await asio::async_write(downstream,asio::buffer(wire),asio::use_awaitable);
      ++io.downstream_write_successes;
    } asio::awaitable<void> reply(int status,json::object payload,std::string extra="") {
      auto body=json::serialize(payload);
      auto title=status==200?"OK":status==401?"Unauthorized":status==429?"Too Many Requests":status==503?"Service Unavailable":"Bad Request";
      std::string wire="HTTP/1.1 "+std::to_string(status)+" "+title+"\r\nContent-Type: application/json\r\nConnection: close\r\nx-uos-request-id: "+id+"\r\nx-uos-git-sha: "+std::string(UOS_GIT_SHA)+"\r\nx-uos-deployment-id: "+std::string(UOS_DEPLOYMENT_ID)+"\r\n"+extra+"Content-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
      headers_sent=true;
      co_await write(std::move(wire));
      delivered=true;
    } asio::awaitable<void> stream_headers(std::string const& provider,std::string const& warning) {
      std::string wire="HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: "+std::string(native_headers?"no-store":"no-cache")+"\r\nConnection: close\r\nTransfer-Encoding: chunked\r\nx-uos-request-id: "+id+"\r\nx-uos-git-sha: "+std::string(UOS_GIT_SHA)+"\r\nx-uos-deployment-id: "+std::string(UOS_DEPLOYMENT_ID)+"\r\n";
      if(!native_headers)wire+="x-uos-upstream: deepseek\r\n";
      if(!native_headers&&!provider.empty())wire+="x-uos-provider-request-id: "+provider+"\r\n";
      if(!warning.empty())wire+="x-uos-warning: "+warning+"\r\n";
      wire+="\r\n";
      headers_sent=true;
      co_await write(std::move(wire));
    } asio::awaitable<void> emit(json::array events) {
      for(auto& event:events) {
        if(++event_count>runtime.config.limits.events)throw Failure(502,"Response event count exceeded");
        auto& o=event.as_object();
        o["sequence_number"]=sequence++;
        auto frame="event: "+field(o,"type")+"\ndata: "+json::serialize(o)+"\n\n";
        if(frame.size()>runtime.config.limits.terminal_bytes)throw Failure(502,"Encoded response exceeds bound");
        std::ostringstream size;
        size<<std::hex<<frame.size();
        co_await write(size.str()+"\r\n"+frame+"\r\n");
      }
    }
  };
  Session::Session(Runtime& r,tcp::socket socket):impl_(std::make_unique<Impl>(r,std::move(socket))) {
  } Session::~Session()=default;
  void Session::cancel() {
    impl_->cancel();
  } asio::awaitable<void> Session::run() {
    auto self=shared_from_this();
    auto& p=*impl_;
    auto& runtime=p.runtime;
    auto const& limits=runtime.config.limits;
    std::optional<Failure> failure;
    bool admitted=false;
    try {
      http::request_parser<http::buffer_body> parser;
      parser.header_limit(16384);
      parser.body_limit(boost::none);
      p.downstream.expires_after(limits.header);
      co_await http::async_read_header(p.downstream,p.buffer,parser,asio::use_awaitable);
      auto const& request=parser.get();
      if(request.method()==http::verb::get&&request.target()=="/health") {
        auto health = json::object {
          {
            "status","ready"
          }, {
            "fixture_authority",true
          }, {
            "production_standby",false
          }, {"git_sha",UOS_GIT_SHA}, {"deployment_id",UOS_DEPLOYMENT_ID}, {"build_dirty",static_cast<bool>(UOS_BUILD_DIRTY)
          }, {
            "active",runtime.active
          }
        };
        co_await p.reply(200,std::move(health));
      }
      else {
        if(request.method()!=http::verb::post||request.target()!="/v1/responses")throw Failure(404,"Not found","not_found");
        auto auth=request[http::field::authorization];
        std::string bearer(auth);
        if(!bearer.starts_with("Bearer ")||!runtime.state.authenticate(bearer.substr(7)))throw Failure(401,"Unauthorized","invalid_api_key");
        if(p.peer_closed())throw Failure(499,"Request was cancelled.","request_cancelled");
        if(runtime.active>=2)throw Failure(503,"The gateway is at its concurrent inference limit and its waiting queue is full.","local_inference_overload");
        ++runtime.active;runtime.counters.active_requests=runtime.active;
        p.permit=true;
        p.ordinal=++runtime.ordinal;
        auto length=parser.content_length();
        if(length&&*length>limits.request_bytes)throw Failure(413,"Request exceeds the pilot byte bound");
        parser.body_limit(limits.request_bytes);
        co_await runtime.control->fence(p.ordinal,"before_reserve_write",p.id);
        admitted=runtime.state.reserve(p.id);
        co_await runtime.control->fence(p.ordinal,"after_reserve_fsync",p.id);
        std::string body;
        if(length){p.reserve_external(4*static_cast<std::size_t>(*length)+262144);body.reserve(static_cast<std::size_t>(*length));}
        std::array<char,8192> part {
        };
        p.downstream.expires_after(limits.body);
        while(!parser.is_done()) {
          parser.get().body().data=part.data();
          parser.get().body().size=part.size();
          boost::system::error_code ec;
          co_await http::async_read_some(p.downstream,p.buffer,parser,asio::redirect_error(asio::use_awaitable,ec));
          auto n=part.size()-parser.get().body().size;
          if(body.size()+n>limits.request_bytes)throw Failure(413,"Request exceeds the pilot byte bound");
          p.reserve_external(4*(body.size()+n)+262144);
          if(body.capacity()<body.size()+n)body.reserve(std::min(limits.request_bytes,std::max(body.size()+n,body.capacity()*2)));
          body.append(part.data(),n);
          if(ec&&ec!=http::error::need_buffer)throw boost::system::system_error(ec);
        } if(p.peer_closed())throw Failure(499,"Request was cancelled.","request_cancelled");
        p.watch_pending=true;
        p.watch_join.expires_at(std::chrono::steady_clock::time_point::max());
        p.downstream.socket().async_wait(tcp::socket::wait_read,[self](boost::system::error_code ec) {
          auto& state=*self->impl_;state.watch_pending=false;if(!ec) {
            char value;auto n=::recv(state.downstream.socket().native_handle(),&value,1,MSG_PEEK|MSG_DONTWAIT);if(n==0||(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR))state.cancel();else if(n>0)state.cancel();
          }
          state.watch_join.cancel();
        });
        JsonScan scan;
        scan.feed(body,limits);
        json::parse_options options;
        options.max_depth=64;
        boost::system::error_code parse_error;
        auto raw=json::parse(body,parse_error,json::storage_ptr(&p.input_resource),options);
        std::string {
        }
        .swap(body);
        if(parse_error||!raw.is_object())throw Failure(400,"Invalid JSON body");
        auto prepared=project(raw.as_object(),limits,json::storage_ptr(&p.projection_resource));
        if(prepared.stream){auto metadata=request["x-codex-turn-metadata"];try{auto identity=json::parse(std::string(metadata));if(identity.is_object()){p.native_headers=true;for(auto key:{"session_id","thread_id","turn_id"})if(!member(identity.as_object(),key).is_string()||!nonblank_utf16(field(identity.as_object(),key)))p.native_headers=false;}}catch(std::exception const&){p.native_headers=false;}}
        raw=nullptr;
        if(!admitted)admitted=runtime.state.reserve(p.id);
        if(!admitted)throw Failure(429,"Usage limit exceeded","rate_limit_exceeded");
        co_await runtime.control->fence(p.ordinal,"before_dispatch_intent_write",p.id);
        runtime.state.dispatch(p.id);p.dispatch_intent=true;
        co_await runtime.control->fence(p.ordinal,"after_dispatch_intent_fsync",p.id);
        if(p.cancelled||p.peer_closed()) {
          runtime.state.release(p.id,true);
          throw Failure(499,"Request was cancelled.","request_cancelled");
        }
        p.transport=true;
        co_await runtime.control->fence(p.ordinal,"after_transport_started",p.id);
        p.upstream=std::make_unique<Upstream>(runtime.io,runtime.config,runtime.counters);
        co_await p.upstream->open(json::serialize(prepared.body));
        prepared.body.clear();
        std::size_t name_bytes=0;for(auto const& name:prepared.names)name_bytes+=name.name.size()+name.chat.size();p.reserve_external(8*1024*1024+name_bytes+262144);
        if(p.upstream->status()<200||p.upstream->status()>=300) {
          std::string error;
          while(error.size()<65536) {
            auto chunk=co_await p.upstream->read();
            if(chunk.empty())break;
            error+=chunk;
          }
          auto code="deepseek_upstream_http_error";
          std::string message="DeepSeek upstream request failed.";
          try {
            auto v=json::parse(error);
            if(v.is_object()&&member(v.as_object(),"error").is_object()) {
              auto const& e=member(v.as_object(),"error").as_object();
              message=field(e,"message",message);
            }
          }
          catch(std::exception const&) {
          }
          throw Failure(p.upstream->status(),message,code);
        } auto provider=p.upstream->header("x-request-id");
        if(provider.size()>256||provider.find_first_not_of("!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~")!=std::string::npos)provider.clear();
        auto response_id=provider.empty()?random_id("resp_"):"resp_";
        if(!provider.empty())for(char c:provider)if(std::isalnum(static_cast<unsigned char>(c)))response_id+=c;
        p.response_id = response_id;
    Translator translator(prepared,limits,response_id,json::storage_ptr(&p.output_resource));
        SseReader frames(limits.frame_bytes,limits.events);
        bool done=false;
        if(prepared.stream) {
          co_await p.stream_headers(provider,prepared.warning);

        } while(!done) {
          auto frame=frames.next();
          if(frame) {
            p.upstream->reset_frame_deadline();
            if(*frame=="[DONE]") {
              done=true;
              break;
            }
            if(frame->empty())continue;
            Limits frame_limits=limits;
            frame_limits.request_bytes=limits.frame_bytes;
            frame_limits.nodes=16384;
            JsonScan frame_scan;
            frame_scan.feed(*frame,frame_limits);
            auto v=json::parse(repair_utf8(*frame),json::storage_ptr(&p.event_resource));
            if(!v.is_object())throw Failure(502,"Invalid upstream Chat chunk","deepseek_upstream_stream_error");
            auto events=translator.push(v.as_object());
            p.usage=translator.usage();
            if(prepared.stream)co_await p.emit(std::move(events));
            continue;
          }
          auto chunk=co_await p.upstream->read();
          if(chunk.empty())throw Failure(502,"Upstream Chat Completions stream ended before [DONE].","deepseek_upstream_stream_error");
          frames.feed(chunk);
        } auto events=translator.finish();
        p.usage=translator.usage();
        p.semantic=events.empty()?"response.failed":field(events.back().as_object(),"type");
        runtime.state.terminal(p.id,p.semantic,p.usage);
        co_await runtime.control->fence(p.ordinal,"after_terminal_intent_fsync",p.id);
        if(prepared.stream) {
          co_await p.emit(std::move(events));
          co_await p.write("0\r\n\r\n");
          p.delivered=true;
        }
        else {
          if(events.empty()||!member(events.back().as_object(),"response").is_object())throw Failure(502,"Missing terminal response");
          co_await p.reply(200,member(events.back().as_object(),"response").as_object());
        }
        co_await runtime.control->fence(p.ordinal,"after_terminal_write",p.id);
      }
    }
    catch(Failure const& e) {
      failure=e;
    }
    catch(std::bad_alloc const&) {
      failure=Failure(p.transport?502:503,"Pilot allocation budget exhausted",p.transport?"deepseek_upstream_stream_error":"local_inference_overload");
    }
    catch(std::exception const&) {
      failure=Failure(p.cancelled?499:502,p.cancelled?"Request was cancelled.":"Upstream request could not be completed.",p.cancelled?"request_cancelled":p.headers_sent?"deepseek_upstream_stream_error":"deepseek_upstream_unreachable");
    } if(failure) {
      if(p.upstream)p.upstream->cancel();
      if(!p.transport){try{runtime.state.release(p.id,p.dispatch_intent);}catch(std::exception const&){runtime.stop();p.cancel();}}
      if(p.semantic=="cancelled")p.semantic=p.cancelled||failure->status==499?"cancelled":"response.failed";
      try {
        runtime.state.terminal(p.id,p.semantic,p.usage);
      }
      catch(std::exception const&) {
      }
      if(!p.cancelled&&failure->status!=499) {
        try {
          if(!p.headers_sent)co_await p.reply(failure->status,error_payload(*failure),failure->status==503?"Retry-After: 1\r\n":failure->status==429?"Retry-After: 60\r\n":"");
          else {
            auto failure_events = json::array {
              json::object {
                {
                  "type","response.failed"
                }, {
                  "response",json::object {
                    {
                      "id",p.response_id.empty()?p.id:p.response_id
                    }, {
                      "object","response"
                    }, {
                      "status","failed"
                    }, {
                      "error",json::object {
                        {
                          "code",failure->code
                        }, {
                          "message","Upstream Chat Completions stream failed."
                        }
                      }
                    }
                  }
                }
              }
            };
            co_await p.emit(std::move(failure_events));
            co_await p.write("0\r\n\r\n");
            p.delivered=true;
          }
        }
        catch(std::exception const&) {
          p.delivered=false;
        }
      }
    } if(p.upstream)p.upstream->cancel();
    boost::system::error_code ec;
    p.downstream.socket().cancel(ec);
    if(p.watch_pending) {
      co_await p.watch_join.async_wait(asio::redirect_error(asio::use_awaitable,ec));
    }
    p.downstream.socket().close(ec);
    try {
      runtime.state.delivered(p.id,p.delivered);
      runtime.state.settle(p.id);
      co_await runtime.control->fence(p.ordinal,"after_settlement_fsync",p.id);
      if(runtime.state.needs_snapshot()) {
        runtime.state.snapshot_publish();
        co_await runtime.control->fence(p.ordinal,"after_snapshot_rename",p.id);
        runtime.state.snapshot_sync_directory();
        co_await runtime.control->fence(p.ordinal,"after_directory_fsync",p.id);
        runtime.state.snapshot_reset_journal();
        co_await runtime.control->fence(p.ordinal,"after_journal_reset",p.id);
      }
    }
    catch(std::exception const&) {
      runtime.stop();
    } std::cerr<<json::serialize(json::object {
      {
        "event","request_terminal"
      }, {
        "request_id",p.id
      }, {
        "semantic_terminal",p.semantic
      }, {
        "delivery_outcome",p.delivered?"delivered":"interrupted"
      }, {
        "dispatch_started",p.transport
      }, {
        "allocation_budget_peak",p.budget->peak
      }, {
        "authority","private_fixture"
      }
    })<<"\n";
    if(p.permit) {
      --runtime.active;runtime.counters.active_requests=runtime.active;
      p.permit=false;
    }
    --runtime.sockets;
    runtime.drained();
  }
}
