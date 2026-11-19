// Links: tests/lithos_wiring_test.cpp src/catalog/index.cpp src/catalog/models.cpp src/catalog/store.cpp src/catalog/types.cpp
// src/responses_failover_stream.cpp src/provider/stream_relay.cpp src/chat/stream_translation.cpp src/openai_telemetry.cpp src/upstream_wire.cpp
// src/deepseek/index.cpp src/provider/lithos.cpp src/provider/cerebras_rate_limits.cpp src/utils.cpp src/provider/removed_provider_telemetry.cpp
// src/codex/responses_error.cpp src/deepseek/stream.cpp src/deepseek/responses.cpp src/responses_stream.cpp src/config.cpp
// src/bounded_response_body.cpp src/deepseek/responses_stream.cpp src/provider/lithos_rate_limits.cpp src/deepseek/responses_payload.cpp src/http.cpp
// src/provider/cerebras.cpp src/release.cpp src/auth/relay.cpp src/request_policy.cpp src/provider/claude_code_models.cpp
// src/provider/surplus.cpp src/models/codex_models.cpp src/provider/surplus_claude_request.cpp src/provider/anthropic_chat_responses.cpp src/kv.cpp
// src/provider/claude_code_accounts.cpp src/provider/selection.cpp src/provider/claude_code_credentials.cpp src/defaults.cpp src/models/codex_models_fetch.cpp
// src/provider/claude_code.cpp src/provider/anthropic_response_wire.cpp src/types.cpp src/models/codex_models_availability.cpp src/storage/kv.cpp
// src/storage/sqlite_readonly.cpp src/codex/dispatch.cpp src/provider/claude_code_quota.cpp src/provider/anthropic_responses.cpp src/codex/auth.cpp
// src/provider/capacity_events.cpp src/provider/claude_code_usage_schema.cpp src/codex/external_auth.cpp src/provider/anthropic_chat_stream.cpp src/provider/metered.cpp
// src/provider/surplus_claude_chat_request.cpp src/provider/anthropic_replay.cpp src/codex/native_auth.cpp src/provider/capacity_contract.cpp src/codex/routing_mutations.cpp
// src/codex/banked_reset.cpp src/authority/native_codex_mutations.cpp src/codex/capacity_routing.cpp src/authority/native_codex_capacity.cpp src/codex/auth_refresh.cpp
// src/codex/reset_settings.cpp src/runtime_config.cpp src/authority/native_codex_contracts.cpp src/codex/banked_reset_provider.cpp
// src/storage/native_kv.cpp src/provider/capacity.cpp src/cache/prompt_analytics_read.cpp src/provider/health.cpp src/cache/prompt_analytics_core.cpp
// src/codex/native_auth_transport.cpp src/provider/capacity_history.cpp src/provider/claude_capacity_history.cpp src/codex/experiment.cpp src/provider/capacity_parse.cpp
// src/provider/claude_capacity_rollups.cpp src/paid_fallback/ledger_stats.cpp src/provider/capacity_analytics.cpp src/provider/capacity_rollups.cpp src/codex/account_routing.cpp
// src/codex/routing_serial.cpp src/codex/routing_evaluation.cpp src/authority/native_codex_evaluation.cpp src/codex/routing_state.cpp src/authority/native_codex.cpp
// src/codex/overage_settings.cpp src/metered_quota.cpp src/codex/rate_limit_429.cpp src/runtime/websocket.cpp src/authority/native_codex_429.cpp
// src/lib/jev_compaction/codex_items.cpp src/lib/jev_compaction/compact.cpp src/lib/jev_compaction/request.cpp src/lib/jev_compaction/state.cpp src/api_key_policy.cpp
// src/api_keys.cpp src/authority/native_api_key_policy.cpp src/authority/native_api_key.cpp src/authority/native_paid_policy.cpp src/authority/native_paid.cpp
// src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics.cpp src/cache/telemetry_gate.cpp src/codex/banked_reset_claim.cpp src/codex/banked_reset_pool.cpp
// src/codex/banked_reset_submission.cpp src/codex/index.cpp src/codex/responses_operations.cpp src/codex/responses_runners.cpp src/codex/responses_state.cpp
// src/debug_routing.cpp src/deepseek/chat_projection.cpp src/deepseek/context_admission_failure.cpp src/deepseek/flash_aliases.cpp src/deepseek/forwarded_payload_policy.cpp
// src/deepseek/handlers.cpp src/deepseek/waterfall_handler.cpp src/deepseek/waterfall.cpp src/inference_deadline.cpp src/input_normalization.cpp
// src/models/metadata.cpp src/models/openrouter_models.cpp src/openai_schema.cpp src/openai.cpp src/optional_telemetry_queue.cpp
// src/paid_fallback/claude_code_route.cpp src/paid_fallback/health.cpp src/paid_fallback/index.cpp src/paid_fallback/ledger_admission.cpp src/paid_fallback/ledger_backfill.cpp
// src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/removed_provider.cpp src/paid_fallback/rollups.cpp src/paid_fallback/routing.cpp
// src/provider/cerebras_handlers.cpp src/provider/cerebras_responses.cpp src/provider/claude_code_access.cpp src/provider/lithos_handlers.cpp src/provider/lithos_models.cpp
// src/provider/lithos_streams.cpp src/provider/openrouter_handlers.cpp src/provider/openrouter_request_controls.cpp src/provider/openrouter_streams.cpp src/provider/openrouter_web_tools.cpp
// src/provider/openrouter.cpp src/provider/presentation.cpp src/provider/removed_provider_circuit.cpp src/recent_model_context.cpp src/request.cpp
// src/responses_attempts.cpp src/responses_buffered.cpp src/responses_handler.cpp src/responses_native_headers.cpp src/responses_request.cpp
// src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp src/runtime/http.cpp
// src/sentinel/upstream_capture.cpp src/storage/key_compatibility.cpp src/storage/value_compatibility.cpp src/authority/native_paid_contracts.cpp src/lib/jev_compaction/types.cpp
// src/chat/envelope.cpp src/chat/completions.cpp tests/helpers/openai_compat_harness.cpp src/models/catalog.cpp src/models/codex_models_whitelist.cpp
// Port of tests/lithos-wiring.test.ts. Deadline fixtures spend the real inference budget in the mocked first fetch; FakeClock leaves zero or twenty milliseconds for the refused retry wait.
#include "tests/helpers/openai_compat_harness.hpp"
#include "uos/chat/envelope.hpp"
#include "uos/responses_handler.hpp"
#include "uos/models/catalog.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/provider/lithos_handlers.hpp"
#include "uos/provider/lithos_rate_limits.hpp"
#include "uos/deepseek/waterfall.hpp"
#include "uos/inference_deadline.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include <limits>
#include <iostream>
namespace rt=uos::rt;namespace json=uos::json;namespace h=uos::tests::helpers::openai_compat_harness;namespace lr=uos::provider::lithos_rate_limits;namespace lithos=uos::provider::lithos;
namespace {
  const std::string MODEL="deepseek-ai/DeepSeek-V4.1-Flash-ultra",BASE="deepseek-ai/DeepSeek-V4.1-Flash",SIBLING="deepseek-ai/DeepSeek-V4.1-Flash-fast",KIMI="moonshotai/Kimi-K3",MIXED="deepseek-ai/DeepSeek-V4.1-Flash-ULTRA";
  struct Fixture { h::Fixture harness;Fixture(){std::setvbuf(stdout,nullptr,_IONBF,0);rt::env::set("LITHOSAI_API_KEY","lith_sk_fixture");lr::clearLithosFailoverWindows();}~Fixture(){lr::clearLithosFailoverWindows();} };
  json::object completion(json::object message,const std::string& model=MODEL){return {{"id","chatcmpl-lithos-1"},{"object","chat.completion"},{"created",1'790'160'326},{"model",model},{"choices",json::array{json::object{{"index",0},{"message",message},{"finish_reason","stop"}}}},{"usage",json::object{{"prompt_tokens",91},{"completion_tokens",113},{"total_tokens",204},{"prompt_tokens_details",json::object{{"cached_tokens",64}}},{"completion_tokens_details",json::object{{"reasoning_tokens",99}}}}},{"system_fingerprint","fp_lithos_provider_only"}};}
  rt::Response success(const std::string& model=MODEL,const std::string& content="OK"){return rt::Response::json(completion(json::object{{"role","assistant"},{"content",content}},model));}
  rt::Response refusal(const std::string& wait="60",bool prohibited=false){rt::Headers headers{{"retry-after-ms",wait}};if(prohibited)headers.set("x-should-retry","false");return rt::Response::json(json::object{{"error",json::object{{"message","Rate limit exceeded for input_tokens."},{"type","input_tokens"},{"code","rate_limit_exceeded"}}}},rt::ResponseInit{.status=429,.headers=headers});}
  std::string frame(const json::object& value){return "data: "+rt::jsonStringify(value)+"\n\n";}
  rt::Response stream(const std::string& model=MODEL,const std::string& content="Paris",bool reasoning=false){
    const auto chunk=[&model](const json::object& delta,const json::value& finish){return json::object{{"id","chatcmpl-lithos-stream"},{"object","chat.completion.chunk"},{"created",1'790'160'329},{"model",model},{"choices",json::array{json::object{{"index",0},{"delta",delta},{"finish_reason",finish}}}}};};
    std::vector<std::string> frames{frame(chunk(json::object{{"role","assistant"},{"content",""}},nullptr))};if(reasoning)frames.push_back(frame(chunk(json::object{{"reasoning_content","Thinking"}},nullptr)));frames.push_back(frame(chunk(json::object{{"content",content}},nullptr)));frames.push_back(frame(chunk(json::object{},"stop")));frames.push_back(frame(json::object{{"id","chatcmpl-lithos-stream"},{"object","chat.completion.chunk"},{"created",1'790'160'330},{"model",model},{"choices",json::array{}},{"usage",json::object{{"prompt_tokens",11},{"completion_tokens",7},{"total_tokens",18},{"prompt_tokens_details",nullptr},{"completion_tokens_details",json::object{{"reasoning_tokens",5}}}}}}));frames.push_back("data: [DONE]\n\n");return h::sseResponse(frames);
  }
  struct Call { std::string url;json::object body;rt::Headers headers; };
  struct Exchange {rt::Response response;std::string text;std::vector<Call> calls;std::optional<uos::openai_telemetry::ResponseTelemetry> telemetry;json::value payload()const{return rt::jsonParse(text);} };
  using Responder=std::function<rt::Response(const Call&,std::size_t)>;
  rt::Task<Exchange> run(bool chat,bool streamed,std::string model,Responder responder,json::object extra={}){
    std::vector<Call> calls;rt::GlobalFetchOverride fetch(h::createFetchMock([&calls,responder](std::string url,std::optional<std::string> text,rt::RequestInit init){Call call{url,text?rt::jsonParse(*text).as_object():json::object{},init.headers.value_or(rt::Headers())};calls.push_back(call);return rt::fulfilledTask(responder(call,calls.size()));}));
    json::object body{{"model",model},{"stream",streamed}};if(chat)body["messages"]=json::array{json::object{{"role","user"},{"content","hi"}}};else body["input"]="hi";rt::objectAssign(body,extra);
    const auto req=chat?h::specialProviderChatRequest(body):h::specialProviderResponsesRequest(body);uos::openai_telemetry::UsageContext context{.keyId=std::nullopt,.kernelRepo=std::nullopt,.kernelOrg=std::nullopt,.requestId="lithos-wiring",.startedAtMs=rt::dateNow(),.startedAtMonotonicMs=rt::performanceNow()};
    auto task=chat?uos::chat::envelope::handleChatCompletions(req,&context):uos::responses_handler::handleResponses(req,&context);const auto response=co_await std::move(task);const auto text=co_await response.text();co_return Exchange{response,text,calls,uos::openai_telemetry::getResponseTelemetry(response)};
  }
  std::vector<std::string> models(const Exchange& value){std::vector<std::string> result;for(const auto& call:value.calls)result.emplace_back(rt::getString(call.body,"model").value_or(""));return result;}
  void ladder(const Exchange& value,std::size_t count){EXPECT_EQ(value.calls.size(),count);const std::vector<std::string> expected{MODEL,SIBLING,BASE};for(std::size_t i=0;i<count;++i)EXPECT_EQ(rt::getString(value.calls[i].body,"model"),expected[i%3]);}
  rt::Task<void> budget(bool chat,bool exhausted){Fixture fixture;Responder responder=[exhausted](const Call&,std::size_t count){return exhausted||count<=9?refusal():stream();};auto task=run(chat,true,MODEL,responder);const auto value=co_await std::move(task);ladder(value,exhausted?18:10);EXPECT_EQ(value.response.status(),200);ASSERT(value.telemetry);ASSERT(value.telemetry->rateLimitWaitMs);EXPECT(*value.telemetry->rateLimitWaitMs>=(exhausted?300:180));EXPECT_EQ(value.telemetry->streamTerminalType,std::optional<std::string>(exhausted?"response.failed":"response.completed"));EXPECT(rt::includes(value.text,exhausted?"rate_limit_exceeded":chat?"[DONE]":"event: response.completed"));if(exhausted)EXPECT(!rt::includes(value.text,chat?"[DONE]":"event: response.completed"));co_await h::settleHarness();}
}
ASYNC_TEST("lithos wiring: dispatches Chat Completions to the vendor with the bearer key and a verbatim reasoning_effort") {
  Fixture fixture;Responder responder=[](const Call&,std::size_t){return rt::Response::json(completion(json::object{{"role","assistant"},{"content","OK"},{"reasoning_content","Considered the request."},{"tool_calls",nullptr}}),rt::ResponseInit{.headers=rt::Headers{{"x-request-id","lithos-middlebox-injected"}}});};json::object extra{{"reasoning_effort","xhigh"},{"max_completion_tokens",512},{"messages",json::array{json::object{{"role","user"},{"content","Say OK."}}}}};auto task=run(true,false,MODEL,responder,extra);const auto value=co_await std::move(task);EXPECT_EQ(value.calls.size(),1U);EXPECT_EQ(value.calls[0].url,std::string(lithos::LITHOS_CHAT_COMPLETIONS_URL));EXPECT_EQ(value.calls[0].headers.get("authorization"),std::optional<std::string>("Bearer lith_sk_fixture"));EXPECT_EQ(value.calls[0].headers.get("content-type"),std::optional<std::string>("application/json"));const auto expected=rt::jsonParse(R"({"model":"deepseek-ai/DeepSeek-V4.1-Flash-ultra","messages":[{"role":"user","content":"Say OK."}],"reasoning_effort":"xhigh","max_tokens":512,"stream":false})");EXPECT(rt::deepEqual(value.calls[0].body,expected));EXPECT_EQ(value.response.status(),200);EXPECT_EQ(value.response.headers().get("x-uos-upstream"),std::optional<std::string>("lithos"));EXPECT(!value.response.headers().get("x-uos-provider-request-id"));EXPECT_EQ(rt::getString(value.payload(),"model"),MODEL);EXPECT(!rt::includes(value.text,"fp_lithos_provider_only"));EXPECT_EQ(value.telemetry->provider,std::optional<std::string>("lithos"));EXPECT_EQ(value.telemetry->reasoning,std::optional<std::string>("xhigh"));EXPECT_EQ(value.telemetry->attemptedProviders,std::vector<std::string>{"lithos"});EXPECT(value.telemetry->completed);co_await h::settleHarness();
}
ASYNC_TEST("lithos wiring: relays the vendor's Chat stream frames and its unconditional usage") {
  Fixture fixture;Responder responder=[](const Call&,std::size_t){return stream(KIMI,"Paris",true);};auto task=run(true,true,KIMI,responder);const auto value=co_await std::move(task);EXPECT_EQ(value.response.status(),200);EXPECT_EQ(value.response.headers().get("content-type"),std::optional<std::string>("text/event-stream"));EXPECT_EQ(value.calls[0].body.size(),4U);EXPECT_EQ(rt::getString(value.calls[0].body,"reasoning_effort"),"medium");EXPECT(!rt::hasOwn(value.calls[0].body,"stream_options"));EXPECT(rt::includes(value.text,"[DONE]"));const auto chunks=h::parseResponsesSseValues(value.text);EXPECT_EQ(chunks.size(),5U);EXPECT(rt::getArray(chunks[4],"choices")->empty());EXPECT(!rt::hasOwn(rt::get(chunks[4],"usage")->as_object(),"prompt_tokens_details"));EXPECT_EQ(value.telemetry->inputTokens,std::optional<double>(11));EXPECT_EQ(value.telemetry->outputTokens,std::optional<double>(7));EXPECT(value.telemetry->completed);co_await h::settleHarness();
}
ASYNC_TEST("lithos wiring: streams the translated Responses event sequence") {
  Fixture fixture;Responder responder=[](const Call&,std::size_t){return stream(MODEL,"Paris",true);};auto task=run(false,true,MODEL,responder);const auto value=co_await std::move(task);EXPECT_EQ(value.response.status(),200);EXPECT_EQ(rt::getString(value.calls[0].body,"reasoning_effort"),"medium");EXPECT(!rt::hasOwn(value.calls[0].body,"stream_options"));const auto events=h::parseResponsesSseEvents(value.text);EXPECT_EQ(rt::getString(events[0],"type"),"response.created");std::vector<std::string> types;for(std::size_t i=0;i<events.size();++i){types.emplace_back(rt::getString(events[i],"type").value());EXPECT_EQ(rt::getNumber(events[i],"sequence_number"),std::optional<double>(static_cast<double>(i)));}EXPECT(rt::includes(types,std::string("response.output_item.added")));EXPECT(rt::includes(types,std::string("response.reasoning_summary_text.delta")));EXPECT(rt::includes(types,std::string("response.output_text.delta")));EXPECT_EQ(types.back(),"response.completed");const auto* response=rt::get(events.back(),"response");EXPECT_EQ(rt::getString(*response,"model"),MODEL);const auto* output=rt::getArray(*response,"output");EXPECT_EQ(rt::getString((*output)[0],"type"),"reasoning");EXPECT(rt::deepEqual(*rt::get((*output)[1],"content"),rt::jsonParse(R"([{"type":"output_text","text":"Paris","annotations":[]}])")));EXPECT(value.telemetry->completed);co_await h::settleHarness();
}
ASYNC_TEST("lithos wiring: serves /v1/responses through the shared profile translation") {
  Fixture fixture;Responder responder=[](const Call&,std::size_t){return rt::Response::json(completion(json::object{{"role","assistant"},{"content","Paris"},{"reasoning_content","Considered it."},{"tool_calls",nullptr}}));};json::object extra{{"input","Capital of France?"},{"reasoning",json::object{{"effort","high"}}},{"max_output_tokens",256}};auto task=run(false,false,MODEL,responder,extra);const auto value=co_await std::move(task);EXPECT_EQ(value.calls.size(),1U);const auto expected=rt::jsonParse(R"({"model":"deepseek-ai/DeepSeek-V4.1-Flash-ultra","messages":[{"role":"user","content":"Capital of France?"}],"stream":false,"max_tokens":256,"reasoning_effort":"high"})");EXPECT(rt::deepEqual(value.calls[0].body,expected));const auto payload=value.payload();EXPECT_EQ(rt::getString(payload,"object"),"response");EXPECT_EQ(rt::getString(payload,"status"),"completed");EXPECT_EQ(value.telemetry->outputTokenAllowance,std::optional<double>(256));EXPECT_EQ(value.telemetry->reasoning,std::optional<std::string>("high"));extra["reasoning"]=json::object{{"effort","ultra"}};auto rejectedTask=run(false,false,MODEL,responder,extra);const auto rejected=co_await std::move(rejectedTask);EXPECT(rejected.calls.empty());EXPECT_EQ(rejected.response.status(),400);EXPECT_EQ(rt::getString(*rt::get(rejected.payload(),"error"),"param"),"reasoning.effort");EXPECT(rt::includes(rejected.text,"not supported by LithosAI"));co_await h::settleHarness();
}
ASYNC_TEST("lithos wiring: maps the vendor's refusal semantics distinctly") {
  Fixture fixture;struct Case{double status;std::string body;std::string model;rt::Headers headers;std::string code;std::string type;std::string message;};
  const std::vector<Case> cases{{402,R"({"error":{"message":"Your organization has no credit remaining.","type":"insufficient_quota","code":"insufficient_quota"}})",MODEL,rt::Headers{{"x-should-retry","false"}},"insufficient_quota","insufficient_quota","Your organization has no credit remaining."},{429,R"({"error":{"message":"Rate limit exceeded for input_tokens.","type":"input_tokens","code":"rate_limit_exceeded"}})",BASE,rt::Headers{{"retry-after-ms","120000"},{"retry-after","17"},{"x-ratelimit-limit-requests","600"},{"x-ratelimit-remaining-requests","0"},{"x-ratelimit-reset-requests","12s"},{"x-ratelimit-limit-tokens","200000"},{"x-ratelimit-remaining-tokens","0"},{"x-ratelimit-reset-tokens","45s"},{"x-uos-provider-only","must-not-be-relayed"}},"rate_limit_exceeded","rate_limit_error","Rate limit exceeded for input_tokens."},{429,R"({"error":{"message":"The model is at capacity.","type":"provider_overloaded","code":"provider_overloaded"}})",MODEL,{},"provider_overloaded","rate_limit_error","The model is at capacity."},{404,R"({"error":{"message":"The model does not exist.","type":"invalid_request_error","code":"model_not_found"}})",MODEL,{},"model_not_found","invalid_request_error","The model does not exist."},{400,R"({"object":"error","message":"Invalid value for 'n': must be 1","type":"BadRequestError","code":400})",MODEL,{},"400","invalid_request_error","Invalid value for 'n': must be 1"},{500,"<html>provider-only-body</html>",MODEL,rt::Headers{{"content-type","text/html"}},"lithos_upstream_error","server_error","LithosAI upstream returned an error."}};
  for(const auto& item:cases){lr::clearLithosFailoverWindows();Responder responder=[item](const Call&,std::size_t){return rt::Response(item.body,rt::ResponseInit{.status=item.status,.headers=item.headers});};auto task=run(true,false,item.model,responder);const auto value=co_await std::move(task);EXPECT_EQ(value.response.status(),item.status);const auto payload=value.payload();const auto* error=rt::get(payload,"error");EXPECT_EQ(rt::getString(*error,"code"),item.code);EXPECT_EQ(rt::getString(*error,"message"),item.message);if(item.status!=429||item.code=="rate_limit_exceeded")EXPECT_EQ(rt::getString(*error,"type"),item.type);if(item.code=="rate_limit_exceeded"){EXPECT_EQ(value.calls.size(),1U);for(const auto header:lithos::LITHOS_RATE_LIMIT_HEADERS)EXPECT(value.response.headers().has(header));EXPECT_EQ(value.response.headers().get("retry-after"),std::optional<std::string>("17"));EXPECT(!value.response.headers().has("x-uos-provider-only"));}if(item.status==402||item.status==400||item.status==404)EXPECT_EQ(value.response.headers().get("x-should-retry"),std::optional<std::string>("false"));}co_await h::settleHarness();
}
ASYNC_TEST("lithos wiring: the catalog advertises eight addressable ids only while the key is configured") {
  Fixture fixture;rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo,rt::RequestInit){return rt::Response("{}",rt::ResponseInit{.status=503});}));
  for(const bool configured:std::vector<bool>{true,false}){if(!configured)rt::env::remove("LITHOSAI_API_KEY");auto task=uos::models::catalog::buildModelCatalogSnapshot();const auto snapshot=co_await std::move(task);std::vector<std::string> ids;for(const auto& row:snapshot.models){bool included=false;for(const auto& provider:row.providers)if(provider.id=="lithos")included=true;if(!included||row.id==uos::deepseek::waterfall::DEEPSEEK_WATERFALL_MODEL_ID)continue;ids.push_back(row.id);EXPECT_EQ(row.context_window_tokens,std::optional<double>(1'048'576));EXPECT_EQ(row.max_context_window_tokens,std::optional<double>(1'048'576));EXPECT_EQ(row.effective_context_window_percent,std::optional<double>(100));EXPECT_EQ(row.default_reasoning_effort,std::optional<std::string>("medium"));const std::vector<std::string> tiers{"none","minimal","low","medium","high","xhigh","max"};EXPECT_EQ(row.supported_reasoning_levels,std::optional(tiers));}EXPECT_EQ(ids.size(),configured?8U:0U);auto listedTask=uos::models::catalog::handleModels();const auto listed=co_await std::move(listedTask);const auto list=co_await listed.json();const auto* data=rt::getArray(list,"data");for(const auto id:lithos::LITHOS_MODEL_IDS){bool found=false;for(const auto& model:*data)if(rt::getString(model,"id")==id)found=true;EXPECT_EQ(found,configured);}if(configured){auto capTask=uos::models::catalog::handleModelCapabilities();const auto capabilities=co_await std::move(capTask);const auto payload=co_await capabilities.json();std::size_t count=0;for(const auto& model:*rt::getArray(payload,"data"))if(rt::getString(model,"upstream_provider")=="lithos"){++count;EXPECT(rt::getObject(model,"reasoning_effort_wire_map")->empty());EXPECT_EQ(rt::getString(model,"default_reasoning_effort"),"medium");EXPECT_EQ(rt::getNumber(model,"context_window_tokens"),std::optional<double>(1'048'576));}EXPECT_EQ(count,8U);}}co_await h::settleHarness();
}
TEST("lithos rate-limit waits follow the vendor's own header precedence") {
  using W=lr::LithosRateLimitWait;EXPECT_EQ(lr::lithosRateLimitWait(rt::Headers{{"retry-after-ms","250"},{"Retry-After","9"},{"x-ratelimit-reset-tokens","30s"}},1000),std::optional(W{250,"retry-after-ms"}));EXPECT_EQ(lr::lithosRateLimitWait(rt::Headers{{"Retry-After","9"}},1000),std::optional(W{9000,"retry-after"}));EXPECT_EQ(lr::lithosRateLimitWait(rt::Headers{{"Retry-After","Thu, 01 Jan 1970 00:00:05 GMT"}},1000),std::optional(W{4000,"retry-after"}));EXPECT_EQ(lr::lithosRateLimitWait(rt::Headers{{"x-ratelimit-reset-tokens","3.23s"},{"x-ratelimit-reset-requests","12s"}},1000),std::optional(W{12000,"x-ratelimit-reset-requests"}));EXPECT_EQ(lr::lithosRateLimitWait(rt::Headers{{"x-ratelimit-reset-tokens","1m30s"}},1000),std::optional(W{90000,"x-ratelimit-reset-tokens"}));EXPECT_EQ(lr::lithosRateLimitWait(rt::Headers{{"x-ratelimit-reset-requests","750ms"}},1000),std::optional(W{750,"x-ratelimit-reset-requests"}));EXPECT(!lr::lithosRateLimitWait(rt::Headers{{"retry-after-ms","5"},{"x-should-retry","false"}},1000));EXPECT(!lr::lithosRateLimitWait(rt::Headers(),1000));
}
TEST("lithos rate-limit numeric headers reject nonfinite waits and retain finite precedence") {
  const std::string oversized(309,'9'),overflow(308,'9');using W=lr::LithosRateLimitWait;EXPECT(!lr::lithosRateLimitWait(rt::Headers{{"retry-after-ms",oversized}},1000));EXPECT(!lr::lithosRateLimitWait(rt::Headers{{"retry-after",oversized}},1000));EXPECT(!lr::lithosRateLimitWait(rt::Headers{{"retry-after",overflow}},1000));EXPECT(!lr::lithosRateLimitWait(rt::Headers{{"x-ratelimit-reset-tokens",oversized+"s"}},1000));EXPECT_EQ(lr::lithosRateLimitWait(rt::Headers{{"retry-after-ms",oversized},{"retry-after","9"},{"x-ratelimit-reset-tokens","30s"}},1000),std::optional(W{9000,"retry-after"}));EXPECT_EQ(lr::lithosRateLimitWait(rt::Headers{{"retry-after",overflow},{"x-ratelimit-reset-tokens","1.25s"},{"x-ratelimit-reset-requests","750ms"}},1000),std::optional(W{1250,"x-ratelimit-reset-tokens"}));EXPECT_EQ(lr::lithosRateLimitWait(rt::Headers{{"retry-after-ms",overflow}},1000),std::optional(W{rt::stringToNumber(overflow),"retry-after-ms"}));EXPECT_EQ(lr::lithosRateLimitWait(rt::Headers{{"retry-after-ms","90000"},{"retry-after","120"}},1000),std::optional(W{90000,"retry-after-ms"}));
}
TEST("lithos rate-limit numeric windows reject nonfinite deadlines and keep finite expiry") {
  lr::clearLithosFailoverWindows();for(const double value:std::vector<double>{std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){lr::lithosOpenFailoverWindow(MODEL,1000,value,SIBLING);EXPECT(!lr::lithosFailoverTargetAt(MODEL,1000));}lr::lithosOpenFailoverWindow(MODEL,rt::Number::MAX_VALUE,rt::Number::MAX_VALUE,SIBLING);EXPECT(!lr::lithosFailoverTargetAt(MODEL,rt::Number::MAX_VALUE));lr::lithosOpenFailoverWindow(MODEL,1000,90000,SIBLING);EXPECT_EQ(lr::lithosFailoverTargetAt(MODEL,1000),std::optional(SIBLING));EXPECT_EQ(lr::lithosFailoverTargetAt(MODEL,90999),std::optional(SIBLING));EXPECT(!lr::lithosFailoverTargetAt(MODEL,91000));lr::clearLithosFailoverWindows();
}
TEST("lithos refusal windows select the highest recovered rung at its own deadline") {
  lr::clearLithosFailoverWindows();lr::lithosOpenFailoverWindow(MODEL,1000,60000,SIBLING);lr::lithosOpenFailoverWindow(MODEL,2000,1000,BASE);EXPECT_EQ(lr::lithosFailoverTargetAt(MODEL,3001),std::optional(SIBLING));EXPECT_EQ(lr::lithosFailoverTargetAt(MODEL,60999),std::optional(SIBLING));EXPECT(!lr::lithosFailoverTargetAt(MODEL,61000));lr::lithosOpenFailoverWindow(MODEL,61000,1000,SIBLING);EXPECT_EQ(lr::lithosFailoverTargetAt(MODEL,61999),std::optional(SIBLING));EXPECT(!lr::lithosFailoverTargetAt(MODEL,62000));lr::lithosOpenFailoverWindow(MODEL,100000,60000,SIBLING);lr::lithosOpenFailoverWindow(MODEL,101000,120000,BASE);EXPECT(!lr::lithosFailoverTargetAt(MODEL,160000));EXPECT(!lr::lithosFailoverTargetAt(MODEL,220999));EXPECT(!lr::lithosFailoverTargetAt(MODEL,221000));lr::clearLithosFailoverWindows();
}
ASYNC_TEST("lithos refusal windows preserve each rung's own reset in handler routing") {
  Fixture fixture;for(const double wait:std::vector<double>{1000,120000}){lr::clearLithosFailoverWindows();const double initial=rt::dateNow();double now=initial;rt::globalDateNow()=[&now]{return now;};Responder firstResponder=[&now,initial,wait](const Call&,std::size_t count){if(count==1)return refusal("60000");if(count==2){now=initial+1000;return refusal(rt::numberToString(wait));}return success(BASE,"base-served");};auto firstTask=run(true,false,MODEL,firstResponder);const auto first=co_await std::move(firstTask);ladder(first,3);EXPECT_EQ(first.response.status(),200);now=initial+2001;Responder secondResponder=[](const Call& call,std::size_t){return success(std::string(rt::getString(call.body,"model").value()),"served");};auto secondTask=run(true,false,MODEL,secondResponder);const auto second=co_await std::move(secondTask);EXPECT_EQ(models(second),std::vector<std::string>{wait==1000?SIBLING:BASE});EXPECT_EQ(lr::lithosFailoverTargetAt(MODEL,initial+59999),std::optional(models(second)[0]));now=initial+60000;Responder thirdResponder=[](const Call&,std::size_t){return success();};auto thirdTask=run(true,false,MODEL,thirdResponder);const auto third=co_await std::move(thirdTask);EXPECT_EQ(models(third),std::vector<std::string>{MODEL});}co_await h::settleHarness();
}
TEST("lithos rate-limit snapshots capture the vendor's own budgets, bounded") {
  const auto value=lr::lithosRateLimitSnapshot(rt::Headers{{"x-ratelimit-limit-tokens","4000000"},{"x-ratelimit-remaining-tokens","607980"},{"x-ratelimit-reset-tokens","50.88s"},{"retry-after","51"},{"x-ratelimit-remaining-requests","299"},{"x-should-retry","false"}});EXPECT_EQ(rt::getNumber(value,"limit_tokens"),std::optional<double>(4000000));EXPECT_EQ(rt::getNumber(value,"remaining_tokens"),std::optional<double>(607980));EXPECT_EQ(rt::getString(value,"reset_tokens"),"50.88s");EXPECT_EQ(rt::getString(value,"retry_after"),"51");EXPECT_EQ(rt::getNumber(value,"remaining_requests"),std::optional<double>(299));EXPECT_EQ(rt::getString(value,"should_retry"),"false");EXPECT(rt::get(value,"window_ms")->is_null());EXPECT(rt::get(value,"window_source")->is_null());const auto bare=lr::lithosRateLimitSnapshot(rt::Headers());EXPECT(rt::get(bare,"window_ms")->is_null());EXPECT(rt::get(bare,"limit_tokens")->is_null());EXPECT(rt::get(bare,"retry_after")->is_null());
}
ASYNC_TEST("lithos wiring: an Ultra refusal fails over once to the sibling tier's own bucket") {
  Fixture fixture;Responder responder=[](const Call&,std::size_t count){return count==1?refusal("5000"):success(SIBLING,"sibling-served");};auto task=run(true,false,MODEL,responder);const auto value=co_await std::move(task);ladder(value,2);EXPECT_EQ(value.response.status(),200);EXPECT(rt::includes(value.text,"sibling-served"));EXPECT_EQ(value.telemetry->rateLimitFailoverModel,std::optional(SIBLING));EXPECT(!value.telemetry->rateLimitWaitMs);co_await h::settleHarness();
}
ASYNC_TEST("lithos wiring: a refusal on both tiers is relayed without waiting") {
  Fixture fixture;Responder responder=[](const Call&,std::size_t){return refusal("5000",true);};auto task=run(true,false,MODEL,responder);const auto value=co_await std::move(task);ladder(value,3);EXPECT_EQ(value.response.status(),429);EXPECT_EQ(value.telemetry->rateLimitFailoverModel,std::optional(BASE));EXPECT(!value.telemetry->rateLimitWaitMs);EXPECT_EQ(value.telemetry->streamTerminalType,std::optional<std::string>("response.failed"));co_await h::settleHarness();
}
ASYNC_TEST("lithos wiring: a refusal on both tiers is absorbed by the vendor's own window, then the requested tier answers") {
  Fixture fixture;Responder responder=[](const Call&,std::size_t count){return count<=3?refusal("120"):success(MODEL,"after-window");};auto task=run(true,false,MODEL,responder);const auto value=co_await std::move(task);ladder(value,4);EXPECT_EQ(value.response.status(),200);EXPECT(value.telemetry->rateLimitWaitMs);EXPECT(*value.telemetry->rateLimitWaitMs>=120);EXPECT(!value.telemetry->rateLimitFailoverModel);co_await h::settleHarness();
}
ASYNC_TEST("lithos wiring: retry waits preserve deadline and caller cancellation without dispatching again") {
  Fixture fixture;
  for(const bool chat:std::vector<bool>{true,false})for(const bool already:std::vector<bool>{true,false})for(const bool deadline:std::vector<bool>{true,false}){
    lr::clearLithosFailoverWindows();rt::AbortController controller;const auto reason=rt::makeReason(rt::DOMException("Retry wait fixture","AbortError"));int calls=0;std::optional<rt::AbortSignal> observedSignal;
    rt::GlobalFetchOverride fetch(rt::Fetch([&calls,&observedSignal,controller,reason,already,deadline](rt::RequestInfo,rt::RequestInit init)->rt::Task<rt::Response>{
      ++calls;observedSignal=init.signal;
      if(deadline){const double remaining=already?0:20;co_await rt::sleep(uos::inference_deadline::BUFFERED_INFERENCE_DEADLINE_MS-remaining);}
      else if(already)controller.abort(reason);else rt::setTimeout([controller,reason]{controller.abort(reason);},20);
      co_return refusal("5000");
    }));
    json::object raw{{"model",BASE},{"stream",false}};if(chat)raw["messages"]=json::array{json::object{{"role","user"},{"content","hi"}}};else raw["input"]="hi";
    const std::optional<rt::AbortSignal> callerSignal=deadline?std::nullopt:std::optional(controller.signal());
    const auto req=chat?h::specialProviderChatRequest(raw,callerSignal):h::specialProviderResponsesRequest(raw,callerSignal);auto task=chat?uos::chat::envelope::handleChatCompletions(req):uos::responses_handler::handleResponses(req);const auto response=co_await std::move(task);const auto payload=co_await response.json();
    EXPECT_EQ(response.status(),deadline?504:499);EXPECT_EQ(rt::getString(*rt::get(payload,"error"),"code"),deadline?"gateway_timeout":"request_cancelled");EXPECT_EQ(calls,1);
    const auto telemetry=uos::openai_telemetry::getResponseTelemetry(response);EXPECT_EQ(telemetry->failureKind,std::optional<std::string>(deadline?"deadline":"cancellation"));EXPECT_EQ(telemetry->streamTerminalType,std::optional<std::string>(deadline?"deadline":"cancelled"));ASSERT(telemetry->rateLimitWaitMs);EXPECT(*telemetry->rateLimitWaitMs<1000);if(!already)EXPECT(*telemetry->rateLimitWaitMs>0);
    ASSERT(observedSignal);EXPECT(observedSignal->aborted());if(deadline)EXPECT(!req.signal().aborted());rt::Reason rejection;try{co_await lr::waitForLithosRetry(10000,*observedSignal).wait();}catch(...){rejection=std::current_exception();}EXPECT(rejection==observedSignal->reason());
  }
  co_await h::settleHarness();
}
ASYNC_TEST("lithos wiring: a window beyond the wait cap is relayed instead of absorbed") {
  Fixture fixture;Responder responder=[](const Call&,std::size_t){return refusal("120000");};auto task=run(true,false,MODEL,responder);const auto value=co_await std::move(task);ladder(value,3);EXPECT_EQ(value.response.status(),429);EXPECT(rt::includes(value.text,"rate_limit_exceeded"));co_await h::settleHarness();
}
ASYNC_TEST("lithos wiring: a streamed absorb spends the wider stream budget beyond two waits"){co_await budget(false,false);}
ASYNC_TEST("lithos wiring: a streamed absorb that outlives the stream budget reports the refusal in-band"){co_await budget(false,true);}
ASYNC_TEST("lithos wiring: a streamed Chat absorb spends the wider stream budget beyond two waits"){co_await budget(true,false);}
ASYNC_TEST("lithos wiring: a streamed Chat absorb that outlives the stream budget reports the refusal in-band"){co_await budget(true,true);}
ASYNC_TEST("lithos wiring: an Ultra refusal walks the whole ladder to the family's normal tier") {
  Fixture fixture;Responder responder=[](const Call&,std::size_t count){return count<=2?refusal():stream(BASE,"base-served");};auto task=run(false,true,MODEL,responder);const auto value=co_await std::move(task);ladder(value,3);EXPECT_EQ(value.response.status(),200);EXPECT(rt::includes(value.text,"event: response.completed"));EXPECT_EQ(value.telemetry->rateLimitFailoverModel,std::optional(BASE));co_await h::settleHarness();
}
ASYNC_TEST("lithos wiring: a fast refusal deepens the open window to the normal tier for later requests") {
  Fixture fixture;Responder first=[](const Call&,std::size_t count){return count<=2?refusal("60000"):success(BASE,"base-served");};auto firstTask=run(true,false,MODEL,first);const auto value=co_await std::move(firstTask);ladder(value,3);Responder second=[](const Call&,std::size_t){return success(BASE,"base-again");};auto secondTask=run(true,false,MODEL,second);const auto next=co_await std::move(secondTask);EXPECT_EQ(models(next),std::vector<std::string>{BASE});EXPECT_EQ(next.response.status(),200);co_await h::settleHarness();
}
ASYNC_TEST("lithos wiring: a streamed refusal reports in-band instead of an HTTP 429 status") {
  Fixture fixture;Responder responder=[](const Call&,std::size_t){return refusal("5000",true);};auto task=run(false,true,MODEL,responder);const auto value=co_await std::move(task);ladder(value,3);EXPECT_EQ(value.response.status(),200);EXPECT(rt::includes(value.text,"\"type\":\"response.failed\""));EXPECT(rt::includes(value.text,"rate_limit_exceeded"));EXPECT_EQ(value.telemetry->streamTerminalType,std::optional<std::string>("response.failed"));co_await h::settleHarness();
}
ASYNC_TEST("lithos wiring: a streamed refusal on both tiers is absorbed by the vendor's own window, then the requested tier streams") {
  Fixture fixture;Responder responder=[](const Call&,std::size_t count){return count<=3?refusal():stream(MODEL,"after-window");};auto task=run(false,true,MODEL,responder);const auto value=co_await std::move(task);ladder(value,4);EXPECT_EQ(value.response.status(),200);EXPECT(rt::includes(value.text,"event: response.completed"));EXPECT(value.telemetry->rateLimitWaitMs);EXPECT(*value.telemetry->rateLimitWaitMs>=60);EXPECT(!value.telemetry->rateLimitFailoverModel);co_await h::settleHarness();
}
ASYNC_TEST("lithos wiring: only the mapped sibling may answer an Ultra request with a different model") {
  Fixture fixture;Responder responder=[](const Call&,std::size_t){return success(SIBLING,"impostor");};auto task=run(true,false,MODEL,responder);const auto value=co_await std::move(task);ladder(value,1);EXPECT_EQ(value.response.status(),502);EXPECT(rt::includes(value.text,"lithos_upstream_invalid_response"));co_await h::settleHarness();
}
ASYNC_TEST("lithos wiring: a differently-cased refused Ultra id serves from the sibling until reset") {
  Fixture fixture;Responder first=[](const Call&,std::size_t count){return count==1?refusal("400"):success(SIBLING,"sibling-served");};auto firstTask=run(true,false,MIXED,first);const auto value=co_await std::move(firstTask);ladder(value,2);Responder second=[](const Call&,std::size_t){return success(SIBLING,"sibling-direct");};auto secondTask=run(true,false,MIXED,second);const auto next=co_await std::move(secondTask);EXPECT_EQ(models(next),std::vector<std::string>{SIBLING});EXPECT_EQ(next.telemetry->rateLimitFailoverModel,std::optional(SIBLING));EXPECT(!next.telemetry->rateLimitWaitMs);co_await rt::sleep(420);Responder third=[](const Call&,std::size_t){return success();};auto thirdTask=run(true,false,MIXED,third);const auto last=co_await std::move(thirdTask);EXPECT_EQ(models(last),std::vector<std::string>{MODEL});EXPECT(!last.telemetry->rateLimitFailoverModel);co_await h::settleHarness();
}
ASYNC_TEST("lithos wiring: a refusal on a tier with no configured sibling opens no window") {
  Fixture fixture;Responder first=[](const Call&,std::size_t){return refusal("120000");};auto firstTask=run(true,false,BASE,first);const auto value=co_await std::move(firstTask);EXPECT_EQ(value.calls.size(),1U);EXPECT_EQ(value.response.status(),429);Responder second=[](const Call&,std::size_t){return success();};auto secondTask=run(true,false,MODEL,second);const auto next=co_await std::move(secondTask);EXPECT_EQ(models(next),std::vector<std::string>{MODEL});co_await h::settleHarness();
}


ASYNC_TEST("lithos wiring: completed deferred dispatch releases its signals while the loop stays live") {
  const std::vector<bool> wires{false, true};
  for (const bool chat : wires) {
    std::weak_ptr<rt::AbortController> observed;
    bool sawController = false;
    int calls = 0;
    uos::provider::lithos_handlers::setLithosPendingCancellationObserverForTest([&observed, &sawController](auto weak) { observed = std::move(weak); sawController = true; });
    const auto clearObserver = rt::scopeExit([] { uos::provider::lithos_handlers::setLithosPendingCancellationObserverForTest({}); });
    {
      Fixture fixture;
      rt::GlobalFetchOverride fetch(h::createFetchMock([&calls](std::string, std::optional<std::string>, rt::RequestInit init) {
        ++calls;
        if (calls <= 3) return rt::fulfilledTask(refusal("1"));
        EXPECT(init.signal);
        return rt::fulfilledTask(stream());
      }));
      json::object body{{"model", MODEL}, {"stream", true}};
      if (chat) body["messages"] = json::array{json::object{{"role", "user"}, {"content", "hi"}}};
      else body["input"] = "hi";
      const auto request = chat ? h::specialProviderChatRequest(body) : h::specialProviderResponsesRequest(body);
      auto dispatch = chat ? uos::provider::lithos_handlers::handleLithosChatCompletions(request, body, MODEL)
                           : uos::provider::lithos_handlers::handleLithosResponses(request, body, MODEL);
      const auto response = co_await std::move(dispatch);
      EXPECT_EQ(response.status(), 200);
      auto reading = response.text();
      const auto output = co_await std::move(reading);
      EXPECT(rt::includes(output, "Paris"));
      EXPECT_EQ(calls, 4);
      co_await h::settleHarness();
    }
    co_await rt::microtask();
    EXPECT(sawController);
    EXPECT(observed.expired());
  }
}
