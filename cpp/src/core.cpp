#include "pilot/core.hpp"
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <fstream>
#include <limits>
#include <new>
namespace pilot {
  Failure::Failure(int s, std::string message, std::string c, std::string p) : std::runtime_error(std::move(message)), status(s), code(std::move(c)), param(std::move(p)) {
  } std::string text(json::value const& v) {
    return v.is_string() ? std::string(v.as_string()) : "";
  } json::value const& member(json::object const& v, std::string_view k) {
    static json::value const empty;
    auto p = v.if_contains(k);
    return p ? *p : empty;
  } std::string field(json::object const& v, std::string_view k, std::string fallback) {
    auto const& x = member(v,k);
    return x.is_string() ? text(x) : std::move(fallback);
  } bool flag(json::object const& v, std::string_view k, bool fallback) {
    auto const& x = member(v,k);
    return x.is_bool() ? x.as_bool() : fallback;
  } std::int64_t integer(json::object const& v, std::string_view k, std::int64_t fallback) {
    auto const& x = member(v,k);
    if(x.is_int64()) return x.as_int64();
    if(x.is_uint64() && x.as_uint64() <= static_cast<std::uint64_t>(INT64_MAX)) return static_cast<std::int64_t>(x.as_uint64());
    return fallback;
  } std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  } std::string sha256(std::string_view v) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<unsigned char const*>(v.data()),v.size(),digest);
    std::string r;
    r.reserve(64);
    constexpr char hex[]="0123456789abcdef";
    for(auto c:digest) {
      r+=hex[c>>4];
      r+=hex[c&15];
    } return r;
  } std::string random_id(std::string_view prefix) {
    unsigned char bytes[16];
    if(RAND_bytes(bytes,16)!=1) throw std::runtime_error("Random identity unavailable");
    std::string r(prefix);
    constexpr char hex[]="0123456789abcdef";
    for(auto c:bytes) {
      r+=hex[c>>4];
      r+=hex[c&15];
    } return r;
  } json::object error_payload(Failure const& f) {
    json::object e {
      {
        "message",f.what()
      }, {
        "type",f.status>=500?"server_error":f.status==429?"rate_limit_error":"invalid_request_error"
      }, {
        "code",f.code
      }
    };
    if(!f.param.empty()) e["param"]=f.param;
    return {
      {
        "error",std::move(e)
      }
    };
  } void* BudgetResource::do_allocate(std::size_t n, std::size_t a) {
    if(n>limit_-current_ || n>group_->limit-group_->current) throw std::bad_alloc();
    void* p=::operator new(n,std::align_val_t(a));
    current_+=n;
    group_->current+=n;
    peak_=std::max(peak_,current_);
    group_->peak=std::max(group_->peak,group_->current);
    return p;
  } void BudgetResource::do_deallocate(void* p,std::size_t n,std::size_t a) {
    current_-=n;
    group_->current-=n;
    ::operator delete(p,std::align_val_t(a));
  } bool BudgetResource::do_is_equal(json::memory_resource const& other) const noexcept {
    return this==&other;
  } void JsonScan::feed(std::string_view input, Limits const& l) {
    bytes+=input.size();
    if(bytes>l.request_bytes) throw Failure(413,"Request exceeds the pilot byte bound");
    for(char c:input) {
      if(quoted) {
        if(escaped)escaped=false;
        else if(c=='\\')escaped=true;
        else if(c=='"')quoted=false;
        continue;
      } if(c=='"') {
        quoted=true;
        atom=false;
        ++nodes;
      }
      else if(c=='{'||c=='[') {
        ++nodes;
        ++depth;
        atom=false;
      }
      else if(c=='}'||c==']') {
        if(depth)--depth;
        atom=false;
      }
      else if(c==','||c==':'||c==' '||c=='\n'||c=='\r'||c=='\t')atom=false;
      else if(!atom) {
        ++nodes;
        atom=true;
      }
      if(depth>l.depth||nodes>l.nodes)throw Failure(400,"JSON exceeds the pilot depth or node bound");
    }
  } Config read_config(std::filesystem::path const& path) {
    if(std::filesystem::is_symlink(path)||!std::filesystem::is_regular_file(path)) throw std::runtime_error("Configuration must be a regular private file");
    std::ifstream file(path);
    if(!file)throw std::runtime_error("Configuration file unavailable");
    file.seekg(0,std::ios::end);
    if(file.tellg()>16384)throw std::runtime_error("Configuration exceeds its bound");
    file.seekg(0);
    std::string data((std::istreambuf_iterator<char>(file)), {
    });
    if(data.size()>16384)throw std::runtime_error("Configuration exceeds its bound");
    auto v=json::parse(data);
    if(!v.is_object())throw std::runtime_error("Configuration must be an object");
    auto const& o=v.as_object();
    Config c;
    c.fixture=flag(o,"fixture_mode");
    c.port=static_cast<unsigned short>(integer(o,"port"));
    c.state_directory=field(o,"state_directory");
    c.quota_limit=integer(o,"usage_limit",50);
    c.key_id=field(o,"key_id",c.key_id);
    c.ca_file=field(o,"ca_file","/etc/ssl/certs/ca-certificates.crt");
    c.upstream_key=field(o,"upstream_key");
    auto token=field(o,"token");
    if(token.empty()||token.size()>256||c.state_directory.empty()||c.quota_limit<0)throw std::runtime_error("Synthetic token/state/quota configuration required");
    c.token_hash=sha256(token);
    auto origin=field(o,"upstream_origin","https://api.deepseek.com");
    auto start=origin.find("://");
    if(start==std::string::npos)throw std::runtime_error("Invalid upstream origin");
    auto scheme=origin.substr(0,start);
    auto authority=origin.substr(start+3);
    if(authority.find_first_of("/@?#")!=std::string::npos)throw std::runtime_error("Upstream must be a root origin");
    auto colon=authority.find(':');
    c.upstream_host=authority.substr(0,colon);
    c.tls=scheme=="https";
    c.upstream_port=colon==std::string::npos?(c.tls?"443":"80"):authority.substr(colon+1);
    if(!c.tls && !(c.fixture&&scheme=="http"&&c.upstream_host=="127.0.0.1"))throw std::runtime_error("Plain HTTP requires an isolated loopback fixture");
    if(c.tls&&c.upstream_host!="api.deepseek.com"&&!c.fixture)throw std::runtime_error("Provider origin is fixed");
    c.control_in=static_cast<int>(integer(o,"control_read_fd",-1));
    c.control_out=static_cast<int>(integer(o,"control_write_fd",-1));
    if(!c.fixture&&(c.control_in>=0||c.control_out>=0))throw std::runtime_error("Control descriptors require fixture mode");
    if(c.fixture&&flag(o,"short_deadlines")) {
      c.limits.body=std::chrono::milliseconds(5000);
      c.limits.headers=c.limits.frame=c.limits.write=c.limits.body;
    } return c;
  }
std::string repair_utf8(std::string_view input) {
  std::string result;
  for (std::size_t i = 0; i < input.size();) {
    auto first = static_cast<unsigned char>(input[i]);
    if (first < 0x80) { result += input[i++]; continue; }
    unsigned length = first >= 0xc2 && first <= 0xdf ? 2 : first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
    if (!length) { result += "\xef\xbf\xbd"; ++i; continue; }
    unsigned consumed = 1;
    while (consumed < length && i + consumed < input.size()) {
      auto c = static_cast<unsigned char>(input[i + consumed]);
      bool valid = c >= 0x80 && c <= 0xbf;
      if (consumed == 1) valid = valid && !(first == 0xe0 && c < 0xa0) && !(first == 0xed && c > 0x9f) && !(first == 0xf0 && c < 0x90) && !(first == 0xf4 && c > 0x8f);
      if (!valid) break;
      ++consumed;
    }
    if (consumed == length) result.append(input.substr(i, length));
    else result += "\xef\xbf\xbd";
    i += consumed;
  }
  return result;
}
}
