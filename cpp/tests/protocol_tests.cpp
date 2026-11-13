#include "pilot/core.hpp"
#include "pilot/translator.hpp"
#include <iostream>
namespace {
void require(bool condition,char const* message){if(!condition)throw std::runtime_error(message);}
std::string repeated(std::string_view value,std::size_t count){std::string result;result.reserve(value.size()*count);while(count--)result+=value;return result;}
void unicode_source_vectors(){
  std::string emoji="\xf0\x9f\x98\x80",accent="\xc3\xa9";
  struct Vector{std::string value,prefix;};
  std::string byte_prefix=repeated(accent,10000)+std::string(43082,'a');
  std::string marker_prefix=repeated(accent,6552)+std::string(46530,'a');
  std::vector<Vector> vectors={
    {std::string(65535,'a')+emoji+std::string(1023,'a'),std::string(58981,'a')},
    {byte_prefix+emoji+std::string(13476,'a'),byte_prefix},
    {marker_prefix+emoji+std::string(13476,'a'),marker_prefix},
    {std::string(66560,'a'),std::string(58982,'a')},
    {repeated(accent,66560),repeated(accent,31343)}
  };
  for(auto const& vector:vectors){
    auto result=pilot::bounded_payload(vector.value,"input[1].output",false);
    require(result.size()<=65536,"UTF8 forwarding cap");
    auto marker=result.find("\n[gateway:");
    require(marker!=std::string::npos&&result.substr(0,marker)==vector.prefix,"Exact frozen source UTF16 prefix");
    require(result.find("the model did not receive the elided bytes")!=std::string::npos,"Visible source elision notice");
    bool refused=false;
    try{pilot::bounded_payload(vector.value,"input[1].output",true);}catch(pilot::Failure const& f){refused=f.status==400&&f.code=="context_length_exceeded";}
    require(refused,"Disabled truncation must fail closed");
  }
  auto repaired=pilot::repair_utf8(std::string("before")+"\xc3"+"(after");
  require(repaired=="before\xef\xbf\xbd(after","Selected-route nonfatal UTF8 decoder");
}
void budget_vectors(){
  auto group=std::make_shared<pilot::AllocationGroup>();group->limit=256;
  pilot::BudgetResource first(group,192),second(group,192);
  auto a=first.allocate(128,8);auto b=second.allocate(128,8);
  bool refused=false;
  try{second.allocate(1,8);}catch(std::bad_alloc const&){refused=true;}
  require(refused&&group->peak==256,"Aggregate allocation must refuse before crossing bound");
  first.deallocate(a,128,8);second.deallocate(b,128,8);
  require(group->current==0,"All allocation owners released");
  pilot::Limits limits;limits.depth=2;pilot::JsonScan scan;
  refused=false;try{scan.feed("[[[]]]",limits);}catch(pilot::Failure const&){refused=true;}
  require(refused,"Depth bound");
  limits.depth=64;limits.nodes=4;pilot::JsonScan nodes;
  refused=false;try{nodes.feed("{\"x\":[1,2,3]}",limits);}catch(pilot::Failure const&){refused=true;}
  require(refused,"Count nodes before JSON key collapse");
}
}
int main(){
  try{
    unicode_source_vectors();budget_vectors();
    auto group=std::make_shared<pilot::AllocationGroup>();
    pilot::BudgetResource resource(group,8*1024*1024);pilot::Limits limits;
    auto input=pilot::json::parse(R"({"model":"deepseek-flash","stream":true,"input":"hello","reasoning":{"effort":"none"}})");
    auto request=pilot::project(input.as_object(),limits,pilot::json::storage_ptr(&resource));
    require(pilot::field(request.body,"model")=="deepseek-flash","Canonical source model");
    pilot::Translator translator(request,limits,"resp_test",pilot::json::storage_ptr(&resource));
    auto chunk=pilot::json::parse(R"({"choices":[{"index":0,"delta":{"content":"hello"},"finish_reason":"stop"}],"usage":{"prompt_tokens":1,"completion_tokens":1,"total_tokens":2}})");
    translator.push(chunk.as_object());auto end=translator.finish();
    require(pilot::field(end.back().as_object(),"type")=="response.completed"&&translator.finish().empty(),"Exactly one accumulated terminal");
    pilot::SseReader reader(1024,20);reader.feed("data: [DONE]\r\n\r");require(!reader.next(),"Split CRLF delimiter is incomplete");reader.feed("\n");require(reader.next()==std::optional<std::string>("[DONE]"),"Completed CRLF delimiter");
    std::cout<<"protocol, source Unicode and hard allocation vectors passed\n";return 0;
  }catch(std::exception const& error){std::cerr<<error.what()<<"\n";return 1;}
}
