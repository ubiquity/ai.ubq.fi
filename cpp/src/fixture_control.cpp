#include "pilot/control.hpp"
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <array>
#include <fcntl.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
namespace pilot {
  Control::Control(asio::io_context& io,Config const& c,State& s,IoCounters& counters,std::function<void()> stop):state_(s),counters_(counters),input_(io),output_(io),fence_timer_(io),send_gate_(io),write_deadline_(io),stop_(std::move(stop)),key_id_(c.key_id) {
    if(c.control_in<0&&c.control_out<0)return;
    if(!c.fixture||c.control_in<0||c.control_out<0||c.control_in==c.control_out)throw std::runtime_error("Invalid fixture descriptors");
    for(auto pair: {
      std::pair {
        c.control_in,O_RDONLY
      },std::pair {
        c.control_out,O_WRONLY
      }
    }) {
      struct stat st {
      };
      int mode=::fcntl(pair.first,F_GETFL);
      if(::fstat(pair.first,&st)||!S_ISFIFO(st.st_mode)||st.st_uid!=::getuid()||mode<0||(mode&O_ACCMODE)!=pair.second)throw std::runtime_error("Fixture descriptors must be owned anonymous pipes with exact directions");
      std::array<char,256> target{};
      auto count=::readlink(("/proc/self/fd/"+std::to_string(pair.first)).c_str(),target.data(),target.size());
      if(count<6||!std::string_view(target.data(),static_cast<std::size_t>(count)).starts_with("pipe:["))throw std::runtime_error("Fixture control requires anonymous pipes");
      auto cap=::fcntl(pair.first,F_GETPIPE_SZ);
      if(cap<0||cap>65536)throw std::runtime_error("Fixture pipe capacity exceeds bound");
    } input_.assign(c.control_in);
    output_.assign(c.control_out);
    enabled_=true;
  } asio::awaitable<void> Control::send(json::object message) {
    if(stopped_)co_return;
    if(sending_) {
      if(send_waiter_)throw std::runtime_error("Fixture reply queue exceeded");
      send_waiter_=true;
      send_gate_.expires_at(std::chrono::steady_clock::time_point::max());
      boost::system::error_code ec;
      co_await send_gate_.async_wait(asio::redirect_error(asio::use_awaitable,ec));
      send_waiter_=false;
    }
    if(stopped_)co_return;
    sending_=true;
    auto value=json::serialize(message);
    if(value.size()>4092)throw std::runtime_error("Fixture reply exceeds bound");
    std::string wire(4,'\0');
    for(unsigned i=0;i<4;++i)wire[3-i]=static_cast<char>((value.size()>>(i*8))&255);
    wire+=value;
    boost::system::error_code ec;
    write_deadline_.expires_after(std::chrono::seconds(5));
    write_deadline_.async_wait([this](boost::system::error_code error){if(!error){boost::system::error_code ignored;output_.cancel(ignored);}});
    co_await asio::async_write(output_,asio::buffer(wire),asio::redirect_error(asio::use_awaitable,ec));
    write_deadline_.cancel();
    sending_=false;
    send_gate_.cancel();
    if(ec)throw boost::system::system_error(ec);
  } asio::awaitable<json::object> Control::read_frame() {
    auto owner=shared_from_this();
        std::array<unsigned char,4> prefix {
        };
        co_await asio::async_read(input_,asio::buffer(prefix),asio::use_awaitable);
        std::size_t n=0;
        for(auto c:prefix)n=(n<<8)|c;
        if(!n||n>4092||commands_>=1024)throw std::runtime_error("Fixture control frame bound exceeded");
        std::string payload(n,'\0');
        co_await asio::async_read(input_,asio::buffer(payload),asio::use_awaitable);
        Limits limits;
        limits.request_bytes=4092;
        limits.depth=8;
        limits.nodes=64;
        JsonScan scan;
        scan.feed(payload,limits);
        auto v=json::parse(payload);
        if(!v.is_object()||v.as_object().size()>16)throw std::runtime_error("Invalid fixture control object");
        co_return std::move(v.as_object());

  } asio::awaitable<void> Control::run() {
    if(!enabled_)co_return;
    auto owner=shared_from_this();
    try {
      while(!stopped_) {
        auto o=co_await read_frame();
        ++commands_;
        static std::set<std::string>const keys= {
          "version","id","op","key_id","request_id","request_ordinal","phase","fence_id"
        };
        for(auto const& x:o)if(!keys.contains(std::string(x.key())))throw std::runtime_error("Unknown fixture control key");
        if(integer(o,"id")<=0)throw std::runtime_error("Control IDs must be positive");
          auto id=static_cast<std::uint64_t>(integer(o,"id"));
        if(integer(o,"version")!=1||id<=last_id_)throw std::runtime_error("Invalid fixture control version/sequence");
        last_id_=id;
        auto op=field(o,"op");
        json::object reply {
          {
            "version",1
          }, {
            "id",id
          }
        };
        if(op=="READ_STATE") {
          reply["kind"]="STATE";
          auto snapshot=state_.inspect(field(o,"request_id"));
          reply["io_counter_epoch"]=counters_.epoch;
          snapshot["active_requests"]=counters_.active_requests;snapshot["queued_requests"]=0;
          snapshot["upstream_read_starts"]=counters_.upstream_read_starts;snapshot["upstream_read_completions"]=counters_.upstream_read_completions;snapshot["pending_upstream_reads"]=counters_.pending_upstream_reads;
          snapshot["downstream_write_starts"]=counters_.downstream_write_starts;snapshot["downstream_write_completions"]=counters_.downstream_write_completions;snapshot["pending_downstream_writes"]=counters_.pending_downstream_writes;
          snapshot["downstream_write_successes"]=counters_.downstream_write_successes;snapshot["requested_downstream_send_buffer_bytes"]=16384;snapshot["actual_downstream_send_buffer_bytes"]=counters_.actual_downstream_send_buffer_bytes;
          snapshot["upstream_paused_for_downstream_write"]=counters_.pending_downstream_writes>0&&counters_.pending_upstream_reads==0;
          reply["state"]=std::move(snapshot);
        } else if(op=="REVOKE_KEY") {
          if(field(o,"key_id")!=key_id_)throw std::runtime_error("Unknown synthetic key identity");

          state_.revoke();
          reply["kind"]="REVOKED";
          reply["journal_sequence"]=state_.sequence();
        } else if(op=="ARM_FENCE") {
          static std::set<std::string>const phases= {
            "before_reserve_write","after_reserve_fsync","before_dispatch_intent_write","after_dispatch_intent_fsync","after_transport_started","after_terminal_intent_fsync","after_terminal_write","after_settlement_fsync","after_snapshot_rename","after_directory_fsync","after_journal_reset"
          };
          if(armed_||!phases.contains(field(o,"phase")))throw std::runtime_error("Invalid or overlapping fixture fence");
          armed_=true;
          held_=false;
          phase_=field(o,"phase");
          request_id_=field(o,"request_id");
          if(!request_id_.empty()&&o.contains("request_ordinal"))throw std::runtime_error("A fence has exactly one request selector");
          ordinal_=request_id_.empty()?static_cast<std::uint64_t>(integer(o,"request_ordinal",1)):0;
          if(request_id_.size()>64||(!ordinal_&&request_id_.empty()))throw std::runtime_error("Invalid fence request selector");
          fence_id_=id;
          fence_timer_.expires_after(std::chrono::seconds(10));
          auto expected=id;
          fence_timer_.async_wait([this,expected](boost::system::error_code error){if(!error&&armed_&&!held_&&expected==fence_id_)armed_=false;});
          reply["kind"]="FENCE_ARMED";
          reply["fence_id"]=id;
        } else if(op=="RELEASE_FENCE") {
          if(!held_||static_cast<std::uint64_t>(integer(o,"fence_id"))!=fence_id_)throw std::runtime_error("Mismatched fixture fence release");
          armed_=false;
          held_=false;
          fence_timer_.cancel();
          reply["kind"]="FENCE_RELEASED";
          reply["fence_id"]=fence_id_;
        } else throw std::runtime_error("Unknown fixture control opcode");
        co_await send(std::move(reply));
      }
    } catch(std::exception const&) {
      if(!stopped_) {
        stop();
        stop_();
      }
    }
  } asio::awaitable<void> Control::fence(std::uint64_t ordinal,std::string_view phase,std::string_view request) {
    if(!enabled_||!armed_||phase!=phase_||(request_id_.empty()?ordinal!=ordinal_:request!=request_id_))co_return;
    if(fence_timer_.expiry()<=std::chrono::steady_clock::now()) {
      armed_=false;
      throw Failure(499,"Fixture fence expired","request_cancelled");
    }
    held_=true;
    fence_timer_.expires_after(std::chrono::seconds(5));
    fence_timer_.async_wait([this](boost::system::error_code error){if(!error&&held_){stop();stop_();}});
    json::object event {
      {
        "version",1
      }, {
        "kind","FENCE_REACHED"
      }, {
        "fence_id",fence_id_
      }, {
        "phase",phase_
      }, {
        "request_id",std::string(request)
      }, {
        "journal_sequence",state_.sequence()
      }
    };
    co_await send(std::move(event));
    boost::system::error_code ec;
    co_await fence_timer_.async_wait(asio::redirect_error(asio::use_awaitable,ec));
    if(held_||stopped_) {
      held_=false;
      armed_=false;
      throw Failure(499,"Fixture fence owner/deadline ended","request_cancelled");
    }
  } void Control::stop() {
    if(stopped_)return;
    stopped_=true;
    boost::system::error_code ec;
    input_.close(ec);
    output_.close(ec);
    fence_timer_.cancel();
    send_gate_.cancel();
    write_deadline_.cancel();
  }
}
