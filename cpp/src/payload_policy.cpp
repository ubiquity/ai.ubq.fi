#include "pilot/core.hpp"
#include <cmath>
namespace pilot {
  namespace {
    std::u16string units(std::string_view input) {
      std::u16string result;
      for(std::size_t i=0;i<input.size();) {
        auto first=static_cast<unsigned char>(input[i++]);
        std::uint32_t value=first; unsigned count=0;
        if(first>=0xc2&&first<=0xdf){value=first&31;count=1;}
        else if(first>=0xe0&&first<=0xef){value=first&15;count=2;}
        else if(first>=0xf0&&first<=0xf4){value=first&7;count=3;}
        else if(first>=0x80)throw std::runtime_error("Invalid decoded UTF8");
        if(i+count>input.size())throw std::runtime_error("Truncated decoded UTF8");
        while(count--){auto c=static_cast<unsigned char>(input[i++]);if((c&0xc0)!=0x80)throw std::runtime_error("Invalid decoded UTF8");value=(value<<6)|(c&63);}
        if(value<=0xffff)result.push_back(static_cast<char16_t>(value));
        else{value-=0x10000;result.push_back(static_cast<char16_t>(0xd800+(value>>10)));result.push_back(static_cast<char16_t>(0xdc00+(value&1023)));}
      }
      return result;
    }
    std::string bytes(std::u16string const& input) {
      std::string result;
      for(std::size_t i=0;i<input.size();++i){std::uint32_t value=input[i];
        if(value>=0xd800&&value<=0xdbff&&i+1<input.size()&&input[i+1]>=0xdc00&&input[i+1]<=0xdfff){value=0x10000+((value-0xd800)<<10)+(input[++i]-0xdc00);}
        else if(value>=0xd800&&value<=0xdfff)value=0xfffd;
        if(value<0x80)result+=static_cast<char>(value);
        else if(value<0x800){result+=static_cast<char>(0xc0|(value>>6));result+=static_cast<char>(0x80|(value&63));}
        else if(value<0x10000){result+=static_cast<char>(0xe0|(value>>12));result+=static_cast<char>(0x80|((value>>6)&63));result+=static_cast<char>(0x80|(value&63));}
        else{result+=static_cast<char>(0xf0|(value>>18));result+=static_cast<char>(0x80|((value>>12)&63));result+=static_cast<char>(0x80|((value>>6)&63));result+=static_cast<char>(0x80|(value&63));}
      }
      return result;
    }
    std::u16string head(std::u16string const& input,std::size_t length) {
      auto r=input.substr(0,length);
      if(r.size()<input.size()&&!r.empty()&&r.back()>=0xd800&&r.back()<=0xdbff)r.pop_back();
      return r;
    } bool space(char16_t c) {
      return c==9||c==10||c==11||c==12||c==13||c==32||c==0xa0||c==0xfeff||c==0x1680||(c>=0x2000&&c<=0x200a)||c==0x2028||c==0x2029||c==0x202f||c==0x205f||c==0x3000;
    }
  } std::string bounded_payload(std::string value,std::string const& path,bool reject,bool arguments) {
    constexpr std::size_t limit=65536;
    if(value.size()<=limit)return value;
    if(reject)throw Failure(400,"input item "+path+" carries "+std::to_string(value.size())+" bytes; this route forwards at most 65536 bytes per message under deepseek-forwarded-payload/v2, and the request set truncation 'disabled', so the gateway will not reduce it. Reduce the payload or send truncation 'auto' to allow the declared reduction.","context_length_exceeded",path);
    if(arguments) {
      auto count=static_cast<std::int64_t>(value.size());
      return json::serialize(json::object {
        {
          "gateway_elision",json::object {
            {
              "policy","deepseek-forwarded-payload/v2"
            }, {
              "kind","tool_arguments"
            }, {
              "originalBytes",count
            }, {
              "omittedBytes",count
            }, {
              "message","[gateway: deepseek-forwarded-payload/v2 elided "+std::to_string(count)+" bytes omitted; the model did not receive these tool-call arguments; this route forwards at most 65536 bytes per message]"
            }
          }
        }
      });
    } auto original=value.size();
    auto all=units(value);
    auto prefix=head(all,limit);
    while(!prefix.empty()&&bytes(prefix).size()>limit)prefix=head(prefix,static_cast<std::size_t>(static_cast<double>(prefix.size())*0.9));
    for(;;) {
      auto encoded=bytes(prefix);
      auto marker="\n[gateway: deepseek-forwarded-payload/v2 elided "+std::to_string(original-encoded.size())+" bytes omitted; the model did not receive the elided bytes; this route forwards at most 65536 bytes per message because the provider counts forwarded payloads as text tokens]";
      if(prefix.empty()||encoded.size()+marker.size()<=limit)return encoded+marker;
      prefix=head(prefix,static_cast<std::size_t>(static_cast<double>(prefix.size())*0.9));
    }
  } std::size_t estimate_tokens(std::string_view input) {
    auto s=units(input);
    double tokens=0;
    for(std::size_t i=0;i<s.size();) {
      auto c=s[i];
      if(space(c)) {
        ++i;
        continue;
      }
      bool digit=c>=u'0'&&c<=u'9';
      bool alpha=(c>=u'A'&&c<=u'Z')||(c>=u'a'&&c<=u'z');
      if(digit||alpha) {
        auto begin=i++;
        while(i<s.size()&&(digit?(s[i]>=u'0'&&s[i]<=u'9'):((s[i]>=u'A'&&s[i]<=u'Z')||(s[i]>=u'a'&&s[i]<=u'z'))))++i;
        auto n=i-begin;
        tokens+=digit?static_cast<double>(n)/2.0:1.0+static_cast<double>((n-1)/6);
      }
      else {
        tokens+=0.9;
        ++i;
      }
    } return static_cast<std::size_t>(std::ceil(tokens));
  }
bool nonblank_utf16(std::string_view input){auto value=units(input);for(auto unit:value)if(!space(unit))return true;return false;}
}
