#include "pilot/state.hpp"
#include <openssl/crypto.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <fstream>
#include <iostream>
namespace pilot {
  namespace {
    void check(bool value,char const* message) {
      if(!value)throw std::runtime_error(message);
    } void write_all(int fd,void const* data,std::size_t size) {
      auto p=static_cast<char const*>(data);
      while(size) {
        auto n=::write(fd,p,size);
        if(n<0&&errno==EINTR)continue;
        check(n>0,"Fixture state write failed");
        p+=n;
        size-=static_cast<std::size_t>(n);
      }
    } void sync_fd(int fd) {
      auto begin=std::chrono::steady_clock::now();
      check(::fsync(fd)==0,"Fixture state fsync failed");
      auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count();
      std::cerr<<"{\"state_fsync_us\":"<<us<<"}\n";
    } std::string read_file(std::filesystem::path const& path,std::size_t cap) {
      OwnedFd fd;
      fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
      if(fd<0){if(errno==ENOENT)return {};throw std::runtime_error("Fixture state file unavailable");}
      struct stat metadata{};
      check(::fstat(fd,&metadata)==0&&S_ISREG(metadata.st_mode)&&metadata.st_uid==::getuid()&&metadata.st_nlink==1,"Fixture state must be a private regular file");
      check(metadata.st_size>=0&&static_cast<std::uint64_t>(metadata.st_size)<=cap,"Fixture state exceeds disk bound");
      std::string result;
      std::array<char,8192> buffer{};
      for(;;){auto n=::read(fd,buffer.data(),buffer.size());if(n<0&&errno==EINTR)continue;check(n>=0,"Fixture state read failed");if(n==0)break;result.append(buffer.data(),static_cast<std::size_t>(n));check(result.size()<=cap,"Fixture state exceeds disk bound");}
      return result;
    }
  } State::State(Config const& c):config_(c),root_(std::filesystem::canonical(c.state_directory)) {
    auto path=root_.string();
    check((path.find("/.data/")==std::string::npos||path.find("/.data/cpp-tmp/")!=std::string::npos||path.find("/.data/cpp-evidence/")!=std::string::npos)&&path.find("kv.sqlite")==std::string::npos&&path.find("kv-bun.sqlite")==std::string::npos,"Production state path is prohibited");
    check(std::filesystem::exists(root_)&&std::filesystem::is_directory(root_)&&!std::filesystem::is_symlink(root_),"Create a private fixture state directory before launch");
    struct stat meta {
    };
    check(::lstat(root_.c_str(),&meta)==0&&meta.st_uid==::getuid()&&(meta.st_mode&077)==0,"Fixture state directory must be owned and mode700");
    lock_=::open((root_/"owner.lock").c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    check(lock_>=0&&::flock(lock_,LOCK_EX|LOCK_NB)==0,"Fixture state has another writer");
    data_= {
      {
        "magic","uos-cpp-fixture-state-v1"
      }, {
        "seq",0
      }, {
        "revoked",false
      }, {
        "token_hash",c.token_hash
      }, {
        "key_id",c.key_id
      }, {
        "limit",c.quota_limit
      }, {
        "window_start_ms",now_ms()
      }, {
        "window_reset_ms",now_ms()+3600000
      }, {
        "committed",0
      }, {
        "reserved",0
      }, {
        "quota_transactions",0
      }, {
        "quota_mutations",0
      }, {
        "rows",json::object {
        }
      }
    };
    load();
    check(field(data_,"token_hash")==c.token_hash&&field(data_,"key_id")==c.key_id,"Fixture configuration identity does not match retained state");
    journal_=::open((root_/"journal.bin").c_str(),O_CREAT|O_RDWR|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);
    check(journal_>=0,"Fixture journal unavailable");
    if(!std::filesystem::exists(root_/"snapshot.json")) {
      snapshot_publish();
      snapshot_sync_directory();
      snapshot_reset_journal();
    } std::vector<std::string> settle_ids;
    for(auto const& e:data_["rows"].as_object())if(e.value().is_object()&&!member(e.value().as_object(),"terminal").is_null()&&!flag(e.value().as_object(),"settled"))settle_ids.emplace_back(e.key());
    for(auto const& id:settle_ids)settle(id);
  } OwnedFd::~OwnedFd() { if(value>=0) ::close(value); }
  State::~State()=default;
  void State::load() {
    auto snap=read_file(root_/"snapshot.json",8*1024*1024);
    if(!snap.empty()) {
      auto v=json::parse(snap,json::storage_ptr(&resource_));
      check(v.is_object()&&field(v.as_object(),"magic")=="uos-cpp-fixture-state-v1","Wrong fixture snapshot codec");
      data_=std::move(v.as_object());
      seq_=static_cast<std::uint64_t>(integer(data_,"seq"));
    } auto wire=read_file(root_/"journal.bin",8*1024*1024);
    std::size_t pos=0;
    while(pos<wire.size()) {
      check(wire.size()-pos>=4,"Incomplete final fixture frame retained; explicit recovery required");
      std::uint32_t n=0;
      for(unsigned k=0;k<4;++k)n=(n<<8)|static_cast<unsigned char>(wire[pos+k]);
      check(n>64&&n<=2048&&wire.size()-pos-4>=n,"Incomplete or oversized fixture frame");
      auto payload=std::string_view(wire).substr(pos+4,n-64);
      auto digest=std::string_view(wire).substr(pos+4+n-64,64);
      check(sha256(payload)==digest,"Fixture journal checksum mismatch");
      auto v=json::parse(payload);
      check(v.is_object(),"Invalid fixture journal object");
      auto const& o=v.as_object();
      check(field(o,"magic")=="uos-cpp-fixture-journal-v1"&&(field(o,"op")=="row"||field(o,"op")=="policy")&&member(o,"quota").is_bool()&&member(o,"value").is_object()&&integer(o,"seq")>0,"Invalid fixture journal schema");
      auto seq=static_cast<std::uint64_t>(integer(o,"seq"));
      if(seq>seq_) {
        check(seq==seq_+1,"Fixture journal sequence gap");
        apply(field(o,"op"),member(o,"value").as_object(),flag(o,"quota"));
        seq_=seq;
        data_["seq"]=seq_;
      }
      pos+=4+n;
      ++frames_;
    }
    journal_bytes_=wire.size();
  } void State::apply(std::string const& op,json::object const& v,bool quota) {
    if(op=="policy") {
      data_["revoked"]=flag(v,"revoked");
      return;
    }
    auto id=field(v,"request_id");
    check(!id.empty(),"Missing internal state identity");
    auto& rows=data_["rows"].as_object();
    auto old=rows.if_contains(id);
    auto old_state=old&&old->is_object()?field(old->as_object(),"state"):"";
    auto next=field(v,"state");
    auto reserved=integer(data_,"reserved"),committed=integer(data_,"committed");
    if(old_state=="reserved")--reserved;
    if(old_state=="dispatched")--committed;
    if(next=="reserved")++reserved;
    if(next=="dispatched")++committed;
    check(reserved>=0&&committed>=0,"Fixture aggregate underflow");
    data_["reserved"]=reserved;
    data_["committed"]=committed;
    rows[id]=v;
    if(quota) {
      data_["quota_transactions"]=integer(data_,"quota_transactions")+1;
      data_["quota_mutations"]=integer(data_,"quota_mutations")+2;
    }
  } void State::append(std::string const& op,json::object const& v,bool quota) {
    if(unavailable_) throw std::runtime_error("Fixture state is unavailable");
    struct FailureGuard { bool& value; bool complete=false; ~FailureGuard(){ if(!complete)value=true; } } guard{unavailable_};
    check(frames_<4096,"Fixture journal frame limit reached");
    check(op=="policy"||data_["rows"].as_object().size()<16384||data_["rows"].as_object().contains(field(v,"request_id")),"Fixture record limit reached");
    json::object entry {
      {
        "magic","uos-cpp-fixture-journal-v1"
      }, {
        "seq",seq_+1
      }, {
        "op",op
      }, {
        "quota",quota
      }, {
        "value",v
      }
    };
    auto payload=json::serialize(entry);
    auto n=payload.size()+64;
    check(n<=2048&&journal_bytes_+n+4<=8*1024*1024,"Fixture journal byte limit reached");
    std::array<unsigned char,4> prefix {
    };
    for(unsigned k=0;k<4;++k)prefix[3-k]=static_cast<unsigned char>((n>>(8*k))&255);
    write_all(journal_,prefix.data(),prefix.size());
    write_all(journal_,payload.data(),payload.size());
    auto hash=sha256(payload);
    write_all(journal_,hash.data(),hash.size());
    sync_fd(journal_);
    apply(op,v,quota);
    ++seq_;
    data_["seq"]=seq_;
    journal_bytes_+=n+4;
    ++frames_;
  guard.complete=true;
  } bool State::authenticate(std::string_view token) const {
    auto hash=sha256(token);
    auto expected=field(data_,"token_hash");
    return !flag(data_,"revoked")&&hash.size()==expected.size()&&CRYPTO_memcmp(hash.data(),expected.data(),hash.size())==0;
  } bool State::reserve(std::string const& id) {
    auto& rows=data_["rows"].as_object();
    if(rows.contains(id))return field(rows[id].as_object(),"state")=="reserved";
    std::vector<std::string> expired;
    for(auto const& e:rows)if(field(e.value().as_object(),"state")=="reserved"&&integer(e.value().as_object(),"lease_expires_at_ms")<=now_ms())expired.emplace_back(e.key());
    for(auto const& key:expired)release(key);
    if(flag(data_,"revoked"))throw Failure(401,"Unauthorized","invalid_api_key");
    if(integer(data_,"committed")+integer(data_,"reserved")>=integer(data_,"limit"))return false;
    json::object row {
      {
        "request_id",id
      }, {
        "state","reserved"
      }, {
        "reserved_at_ms",now_ms()
      }, {
        "lease_expires_at_ms",now_ms()+300000
      }, {
        "provider",nullptr
      }, {
        "terminal",nullptr
      }, {
        "usage",nullptr
      }, {
        "delivery","unobserved"
      }, {
        "settled",false
      }
    };
    append("row",row,true);
    return true;
  } void State::dispatch(std::string const& id) {
    auto const* p=data_["rows"].as_object().if_contains(id);
    if(!p||field(p->as_object(),"state")!="reserved")throw Failure(429,"Usage limit exceeded","rate_limit_exceeded");
    auto row=p->as_object();
    row["state"]="dispatched";
    row["provider"]="deepseek";
    row["dispatched_at_ms"]=now_ms();
    append("row",row,true);
  } void State::release(std::string const& id,bool compensate) {
    auto const* p=data_["rows"].as_object().if_contains(id);
    if(!p)return;
    auto row=p->as_object();
    auto s=field(row,"state");
    if(s!="reserved"&&!(compensate&&s=="dispatched"))return;
    row["state"]="released";
    row["release_reason"]=compensate?"cancelled_before_transport":"route_completed_without_provider_dispatch";
    append("row",row,true);
  } void State::terminal(std::string const& id,std::string const& kind,json::value const& usage) {
    auto p=data_["rows"].as_object().if_contains(id);
    if(!p||!member(p->as_object(),"terminal").is_null())return;
    auto row=p->as_object();
    row["terminal"]=kind;
    row["usage"]=usage;
    append("row",row,false);
  } void State::delivered(std::string const& id,bool success) {
    auto p=data_["rows"].as_object().if_contains(id);
    if(!p||field(p->as_object(),"delivery")!="unobserved")return;
    auto row=p->as_object();
    row["delivery"]=success?"delivered":"interrupted";
    append("row",row,false);
  } void State::settle(std::string const& id) {
    auto p=data_["rows"].as_object().if_contains(id);
    if(!p||flag(p->as_object(),"settled"))return;
    auto row=p->as_object();
    row["settled"]=true;
    append("row",row,false);
  } void State::revoke() {
    append("policy",json::object {
      {
        "revoked",true
      }
    },false);
  } json::object State::inspect(std::string const& id) const {
    json::object r {
      {
        "fixture_authority",true
      }, {
        "journal_sequence",seq_
      }, {
        "revoked",flag(data_,"revoked")
      }, {
        "committed_requests",integer(data_,"committed")
      }, {
        "reserved_requests",integer(data_,"reserved")
      }, {
        "quota_transactions",integer(data_,"quota_transactions")
      }, {
        "quota_mutations",integer(data_,"quota_mutations")
      }, {
        "record_count",data_.at("rows").as_object().size()
      }, {
        "paid_fallback","not_applicable"
      }, {
        "kernel_quota","not_applicable"
      }
    };
    std::size_t settled=0,unsettled=0;
    for(auto const& row:data_.at("rows").as_object()){if(flag(row.value().as_object(),"settled"))++settled;else ++unsettled;}
    r["logical_dispatch_count"]=integer(data_,"committed");r["settlement_count"]=settled;r["unsettled_count"]=unsettled;
    if(!id.empty()) {
      auto p=data_.at("rows").as_object().if_contains(id);
      r["request"]=p?*p:json::value(nullptr);
    }
    return r;
  } bool State::needs_snapshot() const {
    return frames_>=1024||journal_bytes_>=2*1024*1024;
  } void State::snapshot_publish() {
    auto path=root_/"snapshot.tmp";
    int fd=::open(path.c_str(),O_CREAT|O_TRUNC|O_WRONLY|O_CLOEXEC|O_NOFOLLOW,0600);
    check(fd>=0,"Snapshot temporary unavailable");
    try {
      json::serializer sr;
      sr.reset(&data_);
      std::array<char,65536> buffer {
      };
      std::size_t total=0;
      while(!sr.done()) {
        auto part=sr.read(buffer.data(),buffer.size());
        total+=part.size();
        check(total<=8*1024*1024,"Snapshot cap exceeded");
        write_all(fd,part.data(),part.size());
      }
      sync_fd(fd);
      ::close(fd);
      fd=-1;
      check(::rename(path.c_str(),(root_/"snapshot.json").c_str())==0,"Snapshot rename failed");
    }
    catch(...) {
      if(fd>=0)::close(fd);
      throw;
    }
  } void State::snapshot_sync_directory() {
    int fd=::open(root_.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    check(fd>=0,"Fixture directory unavailable");
    try {
      sync_fd(fd);
      ::close(fd);
    }
    catch(...) {
      ::close(fd);
      throw;
    }
  } void State::snapshot_reset_journal() {
    check(::ftruncate(journal_,0)==0,"Journal reset failed");
    sync_fd(journal_);
    frames_=0;
    journal_bytes_=0;
  }
}
