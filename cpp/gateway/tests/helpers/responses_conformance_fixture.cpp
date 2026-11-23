#include "tests/helpers/responses_conformance_fixture.hpp"
#include "tests/helpers/loopback_http.hpp"
#include "uos/api_keys.hpp"
#include "uos/cache/prompt_analytics_queue.hpp"
#include "uos/config.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/handler/admission.hpp"
#include "uos/handler/index.hpp"
#include "uos/handler/serve_handler.hpp"
#include "uos/kv.hpp"
#include "uos/paid_fallback/index.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/serve.hpp"
#include "uos/storage/kv.hpp"
#include "uos/utils.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/text_codec.hpp"
#include <algorithm>
#include <cmath>
namespace uos::tests::helpers::responses_conformance_fixture {
  namespace wire=runtime::http;
  namespace node=wire::node_http;
  namespace {
    const std::string TOKEN="u_"+std::string(64,'d');
    const std::string KEY_ID="responses-conformance-key";
    void require(bool condition,const std::string& message) { if(!condition)throw rt::JsError(message,"AssertionError"); }
    int boundedMockDispatches(const std::optional<double>& value) { if(!value)return 4;if(!std::isfinite(*value)||std::floor(*value)!=*value||*value<4||*value>4096)throw rt::JsError("Mock maxDispatches must be an integer from 4 through 4096");return static_cast<int>(*value); }
    std::string chatFrame(const json::object& delta,const std::optional<std::string>& finishReason={}) {
      return "data: "+rt::jsonStringify(json::object{{"id","chatcmpl-fixture-1"},{"object","chat.completion.chunk"},{"created",1'780'000'001},{"model","deepseek-flash"},{"choices",json::array{json::object{{"index",0},{"delta",delta},{"finish_reason",finishReason?json::value(*finishReason):json::value(nullptr)}}}}})+"\n\n";
    }
    std::string firstFrame(int turn,const std::string& toolName) {
      if(turn!=1)return chatFrame(json::object{{"role","assistant"},{"content","fixture-"}});
      return chatFrame(json::object{{"role","assistant"},{"tool_calls",json::array{json::object{{"index",0},{"id",client::CONFORMANCE_CALL_ID},{"type","function"},{"function",json::object{{"name",toolName},{"arguments",R"({"text":"fixture-)"}}}}}}});
    }
    std::string remainingFrames(int turn) {
      std::string frame;
      if(turn==1) {
        const json::object arguments{{"arguments","argument-ok\"}"}};
        const json::object call{{"index",0},{"function",arguments}};
        const json::object delta{{"tool_calls",json::array{call}}};
        frame=chatFrame(delta,std::string("tool_calls"));
      } else {
        frame=chatFrame(json::object{{"content","final-ok"}},std::string("stop"));
      }
      const json::object usage=turn==1?json::object{{"prompt_tokens",11},{"completion_tokens",5},{"total_tokens",16},{"prompt_cache_hit_tokens",0}}:json::object{{"prompt_tokens",23},{"completion_tokens",7},{"total_tokens",30}};
      return frame+"data: "+rt::jsonStringify(json::object{{"id","chatcmpl-fixture-1"},{"object","chat.completion.chunk"},{"created",1'780'000'001},{"model","deepseek-flash"},{"choices",json::array{}},{"usage",usage}})+"\n\ndata: [DONE]\n\n";
    }
    rt::Promise<std::string> readRequest(const std::shared_ptr<node::IncomingMessage>& request) {
      const auto pending=rt::withResolvers<std::string>();const auto body=std::make_shared<rt::Bytes>();
      request->on("data",[body,pending,weak=std::weak_ptr<node::IncomingMessage>(request)](const node::IncomingEvent& event) { if(const auto* chunk=std::get_if<rt::ByteChunk>(&event)){body->insert(body->end(),chunk->begin(),chunk->end());if(body->size()>client::CONFORMANCE_LIMITS.bytes){pending.reject(rt::makeReason(rt::JsError("Mock upstream request exceeded its bound")));if(const auto current=weak.lock())current->pause();}} });
      request->once("end",[body,pending](const auto&){pending.resolve(rt::decodeUtf8(*body));});request->once("error",[pending](const auto& event){if(const auto* error=std::get_if<rt::Reason>(&event))pending.reject(*error);});request->resume();return pending.promise;
    }
    json::value recorded(const RecordedRequest& request) {
      json::object result{{"path",request.path},{"body",request.body},{"headers",request.headers}};if(request.response){json::array chunks;for(const auto& frame:request.response->chunksBase64)chunks.push_back(json::value(frame));result["response"]=json::object{{"status",request.response->status},{"headers",request.response->headers},{"chunksBase64",chunks}};}return result;
    }
    json::array recorded(const std::vector<RecordedRequest>& requests) { json::array result;for(const auto& request:requests)result.push_back(recorded(request));return result; }
    class QuotaAtomic final : public rt::ForwardingAtomicOperation {
    public:
      QuotaAtomic(std::unique_ptr<rt::AtomicOperation> inner_,std::shared_ptr<client::ConformanceJournal> journal_) : rt::ForwardingAtomicOperation(std::move(inner_)),journal(std::move(journal_)) {}
      rt::AtomicOperation& set(rt::KvKey key,rt::KvValue value,rt::KvSetOptions options={}) override {
        if(key.size()>=3&&key[0].isString()&&key[0].string()=="uos_ai"&&key[1].isString()&&key[1].string()=="api_key_usage"&&key[2].isString()&&key[2].string()=="v3")writes.push_back(json::object{{"key",rt::storedJsonView(rt::kvValueFromKey(key))},{"value",rt::storedJsonView(value)}});
        inner->set(std::move(key),std::move(value),options);return *this;
      }
      rt::Task<rt::KvCommitResult> commit() override { rt::Promise<rt::KvCommitResult> pending(inner->commit());auto result=co_await pending.wait();if(result.ok&&!writes.empty())journal->add("quota_commit",json::object{{"writes",writes}});co_return result; }
    private:
      std::shared_ptr<client::ConformanceJournal> journal;json::array writes;
    };
    class ObservedKv final : public rt::ForwardingKv {
    public:
      ObservedKv(std::shared_ptr<rt::Kv> database,std::shared_ptr<client::ConformanceJournal> journal_) : rt::ForwardingKv(*database),owner(std::move(database)),journal(std::move(journal_)) {}
      std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<QuotaAtomic>(inner.atomic(),journal); }
    private:
      std::shared_ptr<rt::Kv> owner;std::shared_ptr<client::ConformanceJournal> journal;
    };
    rt::Task<json::object> readLedger(std::shared_ptr<rt::Kv> database,api_key_policy::ApiKeyPolicy policy,std::string requestId) {
      const auto requestKey=api_key_policy::apiKeyUsageV3RequestKey(policy,requestId);const auto windowKey=api_key_policy::apiKeyUsageV3WindowKey(policy);const rt::KvGetOptions options{.consistency=rt::KvConsistencyLevel::strong};
      auto reading=database->get(requestKey,options);const auto request=co_await std::move(reading);auto readingWindow=database->get(windowKey,options);const auto window=co_await std::move(readingWindow);
      co_return json::object{{"request",rt::storedJsonView(request.value)},{"requestVersionstamp",request.versionstamp?json::value(*request.versionstamp):json::value(nullptr)},{"window",rt::storedJsonView(window.value)},{"windowVersionstamp",window.versionstamp?json::value(*window.versionstamp):json::value(nullptr)},{"paidFallback","not_applicable"}};
    }
  }
  struct ConformanceMock::State {
    FixtureOptions options;
    int maxDispatches=4;
    std::shared_ptr<node::Server> server;
    std::map<int,rt::Resolve<std::string>> pending;
    std::vector<rt::Promise<void>> tasks;
  };
  namespace {
    rt::Task<void> serveMock(std::shared_ptr<ConformanceMock> mock,std::shared_ptr<node::IncomingMessage> request,std::shared_ptr<node::ServerResponse> response) {
      auto reading=readRequest(request);const auto body=co_await reading.wait();auto state=mock->state;
      if(mock->calls.size()>=static_cast<std::size_t>(state->maxDispatches))throw rt::JsError("Fixture mock dispatch bound exceeded");
      const int turn=static_cast<int>(mock->calls.size()+1);const json::object responseHeaders{{"content-type","text/event-stream"},{"cache-control","no-cache"},{"x-request-id","fixture-provider-"+std::to_string(turn)}};
      rt::Headers headers;for(std::size_t i=0;i+1<request->rawHeaders.size();i+=2)headers.append(request->rawHeaders[i],request->rawHeaders[i+1]);
      const auto index=mock->calls.size();mock->calls.push_back({request->url,body,client::conformanceHeaders(headers),RecordedRequest::Response{200,responseHeaders,{}}});mock->journal->add("provider_dispatch",json::object{{"turn",turn},{"body",rt::jsonParse(body)}});
      const auto gate=rt::withResolvers<std::string>();state->pending.insert_or_assign(turn,gate.resolve);
      response->once("close",[mock,turn,response,gate](const auto&) { if(!response->writableFinished())mock->journal->add("provider_cancelled",json::object{{"turn",turn}});gate.resolve(std::string("cancel")); });
      for(const auto& item:responseHeaders){response->setHeader(item.key(),item.value().as_string());}
      response->flushHeaders();const auto initial=firstFrame(turn,state->options.toolName.value_or("fixture_echo"));response->write(initial);mock->calls[index].response->chunksBase64.push_back(rt::toBase64(rt::encodeUtf8(initial)));mock->journal->add("provider_first_delta",json::object{{"turn",turn}});
      if(turn==1&&state->options.holdToolDelta==false){gate.resolve(std::string("receipt"));}
      const auto deadline=rt::setTimeout([response]{response->destroy();},client::CONFORMANCE_LIMITS.deadlineMs);
      const auto reason=co_await gate.promise.wait();rt::clearTimeout(deadline);state->pending.erase(turn);if(reason=="cancel"||response->destroyed())co_return;
      mock->journal->add("provider_resumed",json::object{{"turn",turn}});const auto remaining=remainingFrames(turn);response->end(remaining);mock->calls[index].response->chunksBase64.push_back(rt::toBase64(rt::encodeUtf8(remaining)));
    }
  }
  rt::Task<std::shared_ptr<ConformanceMock>> startConformanceMock(FixtureOptions options,std::shared_ptr<client::ConformanceJournal> journal) {
    const auto mock=std::make_shared<ConformanceMock>();mock->journal=journal?std::move(journal):client::createConformanceJournal();mock->state=std::make_shared<ConformanceMock::State>();auto state=mock->state;state->options=options;state->maxDispatches=boundedMockDispatches(options.maxDispatches);
    state->server=node::createServer([mock](auto request,auto response){rt::Promise<void> task(serveMock(mock,request,response));auto handled=task.catch_([mock,response](rt::Reason error){mock->journal->add("mock_error",json::object{{"message",rt::errorMessage(error).value_or("")}});response->destroy();});mock->state->tasks.push_back(handled);});
    const auto ready=rt::withResolvers<void>();state->server->once("error",[ready](const auto& event){ready.reject(event.error);});state->server->listen(0,"127.0.0.1",ready.resolve);co_await ready.promise.wait();const auto address=state->server->address();require(address.has_value(),"Expected mock loopback address");mock->origin="http://127.0.0.1:"+rt::numberToString(address->port);
    mock->receipt=[weak=std::weak_ptr<ConformanceMock>(mock)](int turn,const client::ConformanceEvent& event){const auto current=weak.lock();if(!current)throw rt::JsError("Mock owner was released");const auto type=rt::getString(event,"type");require(type=="response.function_call_arguments.delta"||type=="response.output_text.delta","Expected progressive client receipt");const auto delta=rt::getString(event,"delta");require(delta&&!delta->empty(),"Expected nonempty client delta");const auto found=current->state->pending.find(turn);require(found!=current->state->pending.end(),"A receipt must refer to the still-paused producer");current->journal->add("client_received_delta",json::object{{"turn",turn},{"event",event}});found->second(std::string("receipt"));};
    mock->stop=[state]() -> rt::Task<void> { const auto closed=rt::withResolvers<void>();state->server->once("close",[closed](const auto&){closed.resolve();});state->server->close();state->server->closeAllConnections();co_await closed.promise.wait();auto pending=rt::allSettled(state->tasks);co_await pending.wait(); };
    co_return mock;
  }
  struct ResponsesConformanceFixture::State {
    FixtureOptions options;std::string savedAuth;bool savedDeploy=false;std::unique_ptr<rt::env::ScopedEnvironment> clean;
    std::unique_ptr<loopback_http::ScopedLoopbackFetch> network;std::unique_ptr<rt::GlobalFetchOverride> fetch;std::unique_ptr<rt::console::Capture> logs;
    std::shared_ptr<rt::Kv> database;api_key_policy::ApiKeyPolicy policy;std::string hash;json::object hashRecord;
    std::optional<wire::HttpServer> gateway;bool stopped=false,kvClosed=false;json::value savedCaptures;json::array accounting;
  };
  ResponsesConformanceFixture::ResponsesConformanceFixture(std::shared_ptr<ConformanceMock> upstream_) : calls(upstream_->calls),upstream(std::move(upstream_)) {}
  rt::Task<json::object> ResponsesConformanceFixture::ledger(std::string requestId) { auto reading=readLedger(state->database,state->policy,requestId);auto result=co_await std::move(reading);co_return result; }
  namespace {
    rt::Task<json::array> accountingFor(std::shared_ptr<ResponsesConformanceFixture> fixture,std::shared_ptr<rt::Kv> database) {
      json::array result;for(const auto& terminal:fixture->terminals){const auto requestId=std::string(rt::getString(terminal,"request_id").value_or(""));auto reading=readLedger(database,fixture->state->policy,requestId);auto rows=co_await std::move(reading);json::object row{{"requestId",requestId}};rt::objectAssign(row,rows);result.push_back(row);}co_return result;
    }
    void persist(const std::shared_ptr<ResponsesConformanceFixture>& fixture) {
      if(!fixture->state->options.captureDirectory)return;
      const auto directory=*fixture->state->options.captureDirectory;rt::fs::mkdirSync(directory,{.recursive=true,.mode=0700});json::array chronology,terminals;
      for(const auto& entry:fixture->journal->entries){chronology.push_back(json::object{{"kind",entry.kind},{"atMs",entry.atMs},{"detail",entry.detail}});}
      for(const auto& entry:fixture->terminals)terminals.push_back(entry);
      const json::object receipt{{"oracleSourceSha",CONFORMANCE_SOURCE_SHA},{"providerOwnership",fixture->state->options.providerOrigin?"external":"fixture"},{"storage",fixture->storage},{"inbound",recorded(fixture->inbound)},{"upstream",recorded(fixture->calls)},{"chronology",chronology},{"terminals",terminals},{"accounting",fixture->state->accounting},{"captures",fixture->state->savedCaptures}};
      rt::fs::writeFileSync(directory+"/raw.json",rt::jsonStringify(receipt,2)+"\n",{.mode=0600});rt::fs::writeFileSync(directory+"/normalized.json",rt::jsonStringify(client::normalizeConformanceValue(receipt),2)+"\n",{.mode=0600});
    }
  }
  rt::Task<void> ResponsesConformanceFixture::save(json::value captures) { state->savedCaptures=std::move(captures);if(!state->kvClosed){auto reading=accountingFor(shared_from_this(),state->database);state->accounting=co_await std::move(reading);}persist(shared_from_this()); }
  rt::Task<void> ResponsesConformanceFixture::save(const std::vector<client::ConformanceWireCapture>& captures) { json::array values;for(const auto& capture:captures)values.push_back(client::toJson(capture));return save(values); }
  rt::Task<void> ResponsesConformanceFixture::save(const client::ConformanceWireCapture& capture) { return save(client::toJson(capture)); }
  client::ConformanceEvent ResponsesConformanceFixture::verifyReopenedLedger() const { const auto* reopen=rt::get(storage,"reopen");require(state->stopped&&reopen&&rt::getString(*reopen,"outcome")=="verified","Stop and drain the fixture before durable readback");return client::conformanceRecord(*reopen); }
  void ResponsesConformanceFixture::acknowledge(int turn,const client::ConformanceEvent& event) { upstream->receipt(turn,event); }
  rt::Task<void> ResponsesConformanceFixture::revoke() { auto value=state->hashRecord;value["revoked_at_ms"]=rt::dateNow();const rt::KvValue stored(value);auto writing=state->database->set(rt::KvKey{"ubq_ai","api_keys","hash",state->hash},stored);co_await std::move(writing);api_key_policy::resetApiKeyPolicyCacheForTest(); }
  rt::Task<void> ResponsesConformanceFixture::stop() {
    if(state->stopped)co_return;
    state->stopped=true;const auto self=shared_from_this();rt::Reason failure;
    try {
      rt::Promise<void> stopUpstream(upstream->stop());const auto stopGateway=state->gateway->shutdown();std::vector<rt::Promise<void>> pending{stopUpstream,stopGateway};auto settled=rt::allSettled(pending);const auto results=co_await settled.wait();auto telemetry=::uos::serve::shutdownOptionalTelemetry();co_await std::move(telemetry);
      for(const auto& result:results)if(result.status=="rejected")std::rethrow_exception(result.reason);
      state->gateway.reset();
      journal->add("transport_drained");auto reading=accountingFor(self,state->database);state->accounting=co_await std::move(reading);state->database->close();state->kvClosed=true;journal->add("kv_closed");
      const auto before=state->accounting;const auto dispatchesBefore=calls.size();const auto quotaCommits=[this]{return std::count_if(journal->entries.begin(),journal->entries.end(),[](const auto& entry){return entry.kind=="quota_commit";});};const auto quotaCommitsBefore=quotaCommits();
      auto opening=storage::kv::openKv(std::string(*rt::getString(storage,"databasePath")));auto reopened=co_await std::move(opening);const auto close=rt::scopeExit([reopened]{reopened->close();});auto observing=std::make_shared<ObservedKv>(reopened,journal);auto rereading=accountingFor(self,observing);state->accounting=co_await std::move(rereading);
      require(rt::deepEqual(state->accounting,before),"Ledger rows and revisions must survive closing and reopening the real Bun database");require(calls.size()==dispatchesBefore,"Reopening state cannot dispatch another generation");require(quotaCommits()==quotaCommitsBefore,"Reopening state cannot settle a ledger twice");
      storage["reopen"]=json::object{{"outcome","verified"},{"before",before},{"after",state->accounting},{"dispatchesBefore",dispatchesBefore},{"dispatchesAfter",calls.size()},{"quotaCommitsBefore",quotaCommitsBefore},{"quotaCommitsAfter",quotaCommits()}};journal->add("kv_reopened_verified");persist(self);
    } catch(...) { failure=std::current_exception();storage["reopen"]=json::object{{"outcome","failed"},{"message",rt::errorMessage(failure).value_or("")}};persist(self); }
    state->logs.reset();state->fetch.reset();state->network.reset();handler::admission::setInferenceAdmissionControllerForTest(std::nullopt);::uos::kv::setKvForTest(nullptr);api_key_policy::resetApiKeyPolicyCacheForTest();provider::capacity_events::setProviderCapacitySampleTriggerForTest({});paid_fallback::index::setPaidFallbackTerminalSweepForTest(nullptr);if(!state->kvClosed)state->database->close();config::config.isDeploy=state->savedDeploy;config::config.codexAuthJsonB64=state->savedAuth;state->clean.reset();cache::prompt_analytics_queue::resetOptionalPromptCacheAnalyticsForTest();if(failure)std::rethrow_exception(failure);
  }
  rt::Task<std::shared_ptr<ResponsesConformanceFixture>> startResponsesConformanceFixture(FixtureOptions options) {
    client::stageGatewayAssets();(void)boundedMockDispatches(options.maxDispatches);const auto journal=client::createConformanceJournal();std::shared_ptr<ConformanceMock> upstream;
    if(options.providerOrigin){upstream=std::make_shared<ConformanceMock>();upstream->origin=client::conformanceEndpoint(*options.providerOrigin,"/").origin();upstream->journal=journal;upstream->receipt=[](int,const auto&){throw rt::JsError("The external mock owner must acknowledge its own client receipt");};upstream->stop=[]{return rt::fulfilledTask();};}else{auto starting=startConformanceMock(options,journal);upstream=co_await std::move(starting);}
    const auto fixture=std::make_shared<ResponsesConformanceFixture>(upstream);fixture->state=std::make_shared<ResponsesConformanceFixture::State>();auto state=fixture->state;state->options=options;state->savedAuth=config::config.codexAuthJsonB64;state->savedDeploy=config::config.isDeploy;
    const auto scratch=rt::env::get("TMPDIR");require(scratch.has_value(),"Conformance fixture requires task TMPDIR");std::vector<std::pair<std::string,std::string>> environment{{"TMPDIR",*scratch},{"HOME",*scratch+"/conformance-home"}};rt::fs::mkdirSync(environment.back().second,{.recursive=true,.mode=0700});state->clean=std::make_unique<rt::env::ScopedEnvironment>(environment);config::config.codexAuthJsonB64.clear();state->network=std::make_unique<loopback_http::ScopedLoopbackFetch>();
    fixture->token=TOKEN;fixture->journal=journal;fixture->expected=json::object{{"callId",client::CONFORMANCE_CALL_ID},{"arguments",client::CONFORMANCE_ARGUMENTS},{"final",client::CONFORMANCE_FINAL}};
    const auto directory=options.captureDirectory.value_or(*scratch);rt::fs::mkdirSync(directory,{.recursive=true,.mode=0700});const auto privateRoot=rt::fs::mkdtempSync(directory+"/uos-state-");const auto databasePath=privateRoot+"/fixture-state.sqlite3";fixture->storage=json::object{{"mode","file"},{"databasePath",databasePath},{"codec",std::string(storage::kv::BUN_KV_CODEC)},{"reopen",nullptr}};
    auto opening=storage::kv::openKv(databasePath);auto database=co_await std::move(opening);state->database=std::make_shared<ObservedKv>(database,journal);auto hashing=utils::sha256Base64Url(TOKEN);state->hash=co_await std::move(hashing);const double now=rt::dateNow();
    json::object common{{"expires_at_ms",-1},{"revoked_at_ms",nullptr},{"usage_limit_requests",options.usageLimit.value_or(50)},{"usage_requests",0},{"usage_reset_at_ms",now+3'600'000},{"window_ms",3'600'000},{"usage_quota_version",3},{"paid_fallback_enabled",false},{"paid_fallback_limit_microcredits",api_keys::PAID_FALLBACK_NO_LIMIT},{"paid_fallback_spent_microcredits",0},{"paid_fallback_reserved_microcredits",0},{"paid_fallback_reservation_request_id",nullptr}};
    state->hashRecord=common;state->hashRecord["id"]=KEY_ID;auto key=common;key["id"]=KEY_ID;key["name"]="Synthetic Responses conformance key";key["prefix"]=TOKEN.substr(0,10);key["hash"]=state->hash;key["created_at_ms"]=now;key["paid_fallback_model_ids"]=json::array{};key["paid_fallback_quota_per_credit"]=0;key["paid_fallback_max_exposure_microcredits"]=json::object{};key["paid_fallback_pricing_checked_at_ms"]=now;
    const rt::KvValue keyValue(key),hashValue(state->hashRecord);auto writing=state->database->set(rt::KvKey{"ubq_ai","api_keys","id",KEY_ID},keyValue);co_await std::move(writing);auto writingHash=state->database->set(rt::KvKey{"ubq_ai","api_keys","hash",state->hash},hashValue);co_await std::move(writingHash);const auto policy=api_key_policy::apiKeyPolicyFromHashRecord(state->hash,json::value(state->hashRecord),now);require(policy.has_value(),"Synthetic key must resolve through the actual API-key policy");state->policy=*policy;
    state->fetch=std::make_unique<rt::GlobalFetchOverride>(rt::Fetch([origin=upstream->origin](rt::RequestInfo info,rt::RequestInit init){const auto url=std::visit([](const auto& value)->std::string{using T=std::decay_t<decltype(value)>;if constexpr(std::is_same_v<T,std::string>)return value;else if constexpr(std::is_same_v<T,rt::URL>)return value.href();else return value.url();},info);if(url!=deepseek::index::DEEPSEEK_CHAT_COMPLETIONS_URL)throw rt::JsError("Fixture refused an unmapped provider destination");return rt::defaultFetch()(rt::RequestInfo(origin+"/chat/completions"),std::move(init));}));
    config::config.isDeploy=true;rt::env::set("DEEPSEEK_API_KEY","fixture-provider-secret");::uos::kv::setKvForTest(state->database);api_key_policy::resetApiKeyPolicyCacheForTest();cache::prompt_analytics_queue::resetOptionalPromptCacheAnalyticsForTest();provider::capacity_events::setProviderCapacitySampleTriggerForTest([](const auto&){});paid_fallback::index::setPaidFallbackTerminalSweepForTest([]{});
    const inference_admission::PartialInferenceAdmissionLimits limits{.maxActive=2,.maxWaiting=1,.maxQueueWaitMs=200};fixture->controller=inference_admission::createInferenceAdmissionController(limits);handler::admission::setInferenceAdmissionControllerForTest(fixture->controller);
    state->logs=std::make_unique<rt::console::Capture>(std::vector<rt::console::Method>{rt::console::Method::info});
    // The source console.info replacement needs the approved event-observer API from rt.
    state->logs->setObserver([weak=std::weak_ptr<ResponsesConformanceFixture>(fixture)](const rt::console::Call& call){if(auto current=weak.lock()){if(call.args.size()<2||!(call.args[0]=="[ai.ubq.fi] request_terminal"))return;auto terminal=client::conformanceRecord(rt::jsonParse(call.args[1].string()));current->terminals.push_back(terminal);current->journal->add("request_terminal",terminal);}});
    wire::ServeHttpOptions serveOptions;serveOptions.hostname="127.0.0.1";serveOptions.port=0;serveOptions.onListen=[](const auto&){};
    handler::serve_handler::DeliveryAwareHandler handler=[fixture](rt::Request request,handler::http::RequestDeliveryInfo delivery)->rt::Task<rt::Response>{auto clone=request.clone();const auto body=co_await clone.text();if(rt::utf8ByteLength(body)>client::CONFORMANCE_LIMITS.bytes)throw rt::JsError("Fixture inbound request exceeded its bound");json::object headers;for(const auto& [name,value]:request.headers())headers[name]=name=="authorization"?"[synthetic]":value;fixture->inbound.push_back({rt::URL(request.url()).pathname(),body,headers,{}});auto handling=::uos::handler::index::handler(request,delivery);auto response=co_await std::move(handling);co_return response;};
    state->gateway=wire::serveHttp(serveOptions,handler::serve_handler::createServeHandler(handler));const auto address=co_await state->gateway->ready.wait();fixture->endpoint="http://127.0.0.1:"+rt::numberToString(address.port);co_return fixture;
  }
}
