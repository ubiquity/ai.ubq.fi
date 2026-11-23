#include "tests/helpers/responses_conformance_client.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include <algorithm>
#include <map>
namespace uos::tests::helpers::responses_conformance_client {
  namespace {
    void require(bool condition,const std::string& message) { if(!condition) throw rt::JsError(message,"AssertionError"); }
    class SseDecoder {
    public:
      explicit SseDecoder(std::function<void(ConformanceEvent)> emit_) : emit(std::move(emit_)) {}
      void push(const rt::Bytes& chunk) { pending+=decoder.decode(chunk,{.stream=true});drain(false); }
      void finish() { pending+=decoder.decode();drain(true); }
    private:
      rt::TextDecoder decoder{rt::TextDecoderOptions{.fatal=true}};
      std::string pending,event;
      std::vector<std::string> data;
      std::function<void(ConformanceEvent)> emit;
      void line(const std::string& value) {
        if(value.empty()) {
          if(!data.empty()) { auto payload=conformanceRecord(rt::jsonParse(rt::join(data,"\n")));if(!event.empty())require(rt::getString(payload,"type")==event,"named SSE event must match its payload");emit(payload); }
          event.clear();data.clear();return;
        }
        if(value.front()==':')return;
        const auto colon=value.find(':');const auto field=value.substr(0,colon);auto content=colon==std::string::npos?std::string():value.substr(colon+1);
        if(!content.empty()&&content.front()==' ')content.erase(0,1);
        if(field=="event")event=content;
        if(field=="data")data.push_back(content);
      }
      void drain(bool final) {
        for(;;) { const auto index=pending.find_first_of("\r\n");if(index==std::string::npos||(!final&&pending[index]=='\r'&&index==pending.size()-1))break;const std::size_t width=pending[index]=='\r'&&index+1<pending.size()&&pending[index+1]=='\n'?2:1;line(pending.substr(0,index));pending.erase(0,index+width); }
        if(final&&(!pending.empty()||!data.empty()))throw rt::JsError("SSE ended inside an unterminated event");
      }
    };
  }
  ConformanceEvent conformanceRecord(const json::value& value) { if(!value.is_object())throw rt::JsError("Expected a fixture record");return value.as_object(); }
  std::shared_ptr<ConformanceJournal> createConformanceJournal() { return std::make_shared<ConformanceJournal>(); }
  void ConformanceJournal::add(std::string kind,ConformanceEvent detail) {
    if(entries.size()>=CONFORMANCE_LIMITS.events)throw rt::JsError("Fixture chronology exceeded its bound");
    const ConformanceMarker entry{std::move(kind),rt::performanceNow(),std::move(detail)};entries.push_back(entry);
    std::vector<rt::Resolve<ConformanceMarker>> matched;
    for(auto it=waiters.begin();it!=waiters.end();) { if(it->predicate(entry)){rt::clearTimeout(it->timer);matched.push_back(it->resolve);it=waiters.erase(it);}else ++it; }
    for(const auto& resolve:matched)resolve(entry);
  }
  rt::Promise<ConformanceMarker> ConformanceJournal::wait(std::function<bool(const ConformanceMarker&)> predicate,double timeoutMs) {
    for(const auto& entry:entries)if(predicate(entry))return rt::Promise<ConformanceMarker>::resolve(entry);
    const auto pending=rt::withResolvers<ConformanceMarker>();const auto id=++nextId;const auto weak=weak_from_this();
    auto timer=rt::setTimeout([weak,id,reject=pending.reject] { if(auto self=weak.lock()){std::erase_if(self->waiters,[id](const auto& item){return item.id==id;});reject(rt::makeReason(rt::JsError("Fixture event deadline exceeded")));} },timeoutMs);
    waiters.push_back({id,std::move(predicate),pending.resolve,timer});return pending.promise;
  }
  json::object conformanceHeaders(const rt::Headers& headers) {
    json::object result;for(const auto& [name,value]:headers.entries())if(name=="content-type"||name=="cache-control"||name=="retry-after"||name=="x-request-id"||rt::startsWith(name,"x-uos-"))result[name]=value;return result;
  }
  rt::URL conformanceEndpoint(const std::string& endpoint,const std::string& path) {
    rt::URL url(path,endpoint);const auto hostname=url.hostname();if(url.protocol()!="http:"||(hostname!="127.0.0.1"&&hostname!="localhost"&&hostname!="[::1]"))throw rt::JsError("Synthetic conformance targets must use loopback HTTP");return url;
  }
  json::value toJson(const ConformanceWireCapture& capture) {
    json::array chunks,events;for(const auto& item:capture.chunksBase64)chunks.push_back(json::value(item));for(const auto& item:capture.events)events.push_back(item);
    return json::object{{"request",json::object{{"path",capture.request.path},{"body",capture.request.body}}},{"status",capture.status},{"headers",capture.headers},{"chunksBase64",chunks},{"events",events},{"outcome",capture.outcome},{"error",capture.error?json::value(*capture.error):json::value(nullptr)}};
  }
  rt::Task<std::shared_ptr<ConformanceResponse>> openConformanceResponse(std::string endpoint,std::string token,ConformanceEvent body) {
    const auto url=conformanceEndpoint(endpoint);const auto serialized=rt::jsonStringify(body);
    if(rt::utf8ByteLength(serialized)>CONFORMANCE_LIMITS.bytes)throw rt::JsError("Fixture request exceeded its byte bound");
    const auto journal=createConformanceJournal();const auto finished=rt::withResolvers<ConformanceWireCapture>();
    const auto disconnected=std::make_shared<bool>(false);const rt::AbortController controller;
    rt::RequestInit init;init.method="POST";init.headers=rt::Headers{{"authorization","Bearer "+token},{"content-type","application/json"}};init.body=serialized;init.signal=controller.signal();
    const auto timer=rt::setTimeout([controller]{controller.abort(rt::makeReason(rt::JsError("Fixture HTTP deadline exceeded")));},CONFORMANCE_LIMITS.deadlineMs);
    rt::RequestInfo info=url.href();auto fetching=rt::defaultFetch()(info,init);auto response=co_await std::move(fetching);
    const auto capture=std::make_shared<ConformanceWireCapture>();capture->request={url.pathname(),serialized};capture->status=response.status();capture->headers=conformanceHeaders(response.headers());
    auto result=std::make_shared<ConformanceResponse>();result->capture=capture;result->finished=finished.promise;
    result->waitFor=[journal](std::string type) { std::function<bool(const ConformanceMarker&)> predicate=[type](const auto& entry){return entry.kind=="event"&&rt::getString(entry.detail,"type")==type;};return journal->wait(predicate).then([](const auto& entry){return entry.detail;}); };
    result->disconnect=[disconnected,controller] { *disconnected=true;controller.abort(); };
    auto read=[capture,journal,finished,disconnected,response,timer]() mutable -> rt::Task<void> {
      std::size_t bytes=0;const bool isSse=rt::includes(response.headers().get("content-type").value_or(""),"text/event-stream");
      SseDecoder decoder([capture,journal](ConformanceEvent event) { if(capture->events.size()>=CONFORMANCE_LIMITS.events)throw rt::JsError("Fixture response exceeded its event bound");capture->events.push_back(event);journal->add("event",event); });
      try {
        if(auto stream=response.body()) { auto reader=stream->getReader();for(;;){const auto chunk=co_await reader.read();if(chunk.done)break;bytes+=chunk.value->size();if(bytes>CONFORMANCE_LIMITS.bytes)throw rt::JsError("Fixture response exceeded its byte bound");capture->chunksBase64.push_back(rt::toBase64(*chunk.value));if(isSse)decoder.push(*chunk.value);} }
        if(*disconnected)capture->outcome="interrupted";else { if(isSse)decoder.finish();capture->outcome="delivered"; }
      } catch(...) { capture->error=rt::errorMessage(std::current_exception());capture->outcome=*disconnected?"interrupted":"failed"; }
      rt::clearTimeout(timer);journal->add("closed",json::object{{"outcome",capture->outcome}});finished.resolve(*capture);co_return;
    };
    rt::spawn(rt::async(std::move(read)),rt::ignoreRejection);co_return result;
  }
  ConformanceEvent conformanceRequest(ConformanceRequestOptions options) {
    json::array tools{json::object{{"type","function"},{"name","fixture_echo"},{"parameters",json::object{{"type","object"},{"properties",json::object{{"text",json::object{{"type","string"}}}}},{"required",json::array{"text"}}}}}};
    if(options.namespace_)tools.push_back(json::object{{"type","namespace"},{"name",*options.namespace_},{"tools",json::array{json::object{{"type","function"},{"name","fixture_echo"},{"parameters",json::object{{"type","object"}}}}}}});
    return { {"model",options.model.value_or("deepseek-flash")},{"stream",true},{"store",false},{"reasoning",json::object{{"effort","none"}}},{"instructions","Use the fixture tool and preserve its result."},{"input",json::array{json::object{{"role","developer"},{"content","Keep the exact fixture values."}},json::object{{"role","user"},{"content","Run the fixture tool."}}}},{"tools",tools} };
  }
  ConformanceEvent completedConformanceResponse(const ConformanceWireCapture& capture) {
    require(capture.status==200,"Expected status 200");require(capture.outcome=="delivered","Expected delivered response");std::vector<ConformanceEvent> terminals;
    for(const auto& event:capture.events){const auto type=rt::getString(event,"type");if(type=="response.completed"||type=="response.failed"||type=="response.incomplete"||type=="error")terminals.push_back(event);}
    require(terminals.size()==1,"each response has exactly one semantic terminal");require(rt::getString(terminals.front(),"type")=="response.completed","Expected response.completed");return conformanceRecord(terminals.front().at("response"));
  }
  ConformanceEvent conformanceContinuation(const ConformanceEvent& first,const ConformanceWireCapture& capture,const json::value& output) {
    const auto terminal=completedConformanceResponse(capture);auto result=first;json::array input;
    if(const auto* value=rt::get(first,"input");value&&value->is_array())input=value->as_array();
    if(const auto* value=rt::get(terminal,"output");value&&value->is_array())for(const auto& item:value->as_array())input.push_back(item);
    input.push_back(json::object{{"type","function_call_output"},{"call_id",CONFORMANCE_CALL_ID},{"output",output}});result["previous_response_id"]=terminal.at("id");result["input"]=input;return result;
  }
  rt::Task<std::vector<ConformanceWireCapture>> runConformanceRoundTrip(ConformanceRoundTripOptions options) {
    const auto body=options.request.value_or(conformanceRequest());auto opening=openConformanceResponse(options.endpoint,options.token,body);auto first=co_await std::move(opening);const auto closeFirst=rt::scopeExit(first->disconnect);
    auto firstWaiting=first->waitFor("response.function_call_arguments.delta");const auto delta=co_await firstWaiting.wait();options.acknowledge(1,delta);const auto tool=co_await first->finished.wait();
    const auto continuation=conformanceContinuation(body,tool);auto openingSecond=openConformanceResponse(options.endpoint,options.token,continuation);auto second=co_await std::move(openingSecond);const auto closeSecond=rt::scopeExit(second->disconnect);
    auto secondWaiting=second->waitFor("response.output_text.delta");const auto text=co_await secondWaiting.wait();options.acknowledge(2,text);const auto answer=co_await second->finished.wait();(void)completedConformanceResponse(answer);co_return std::vector<ConformanceWireCapture>{tool,answer};
  }
  json::value normalizeConformanceValue(const json::value& value) {
    std::map<std::string,std::string> ids;std::function<json::value(const json::value&,const std::string&)> visit;
    visit=[&](const json::value& entry,const std::string& key)->json::value { if(entry.is_array()){json::array result;for(const auto& item:entry.as_array())result.push_back(visit(item,""));return result;}if(entry.is_object()){json::object result;for(const auto& item:entry.as_object())result[item.key()]=visit(item.value(),std::string(item.key()));return result;}if((key=="created_at"||key=="created_at_ms")&&entry.is_number())return 0;if((key=="id"||key=="item_id"||key=="request_id")&&entry.is_string()){const auto id=std::string(entry.as_string());if(!ids.contains(id)){const auto prefix=rt::startsWith(id,"fc_")?"fc":rt::startsWith(id,"msg_")?"msg":"id";ids[id]=std::string(prefix)+"_"+std::to_string(ids.size()+1);}return json::value(ids.at(id));}return entry; };return visit(value,"");
  }
}
