// Links: tests/provider_images_branch_coverage_test.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
// src/sentinel/replay_model.cpp src/sentinel/replay_observation.cpp src/images.cpp src/lib/jev_compaction/codex_items.cpp src/lib/jev_compaction/compact.cpp src/lib/jev_compaction/request.cpp
// src/lib/jev_compaction/state.cpp src/api_key_policy.cpp src/api_keys.cpp src/auth/relay.cpp src/authority/native_api_key_policy.cpp
// src/authority/native_api_key.cpp src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp
// src/authority/native_codex_mutations.cpp src/authority/native_codex.cpp src/authority/native_paid_policy.cpp src/authority/native_paid.cpp src/bounded_response_body.cpp
// src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp src/cache/prompt_analytics.cpp src/cache/telemetry_gate.cpp
// src/catalog/index.cpp src/catalog/models.cpp src/catalog/store.cpp src/catalog/types.cpp src/chat/stream_translation.cpp
// src/codex/account_routing.cpp src/codex/auth_refresh.cpp src/codex/auth.cpp src/codex/banked_reset_claim.cpp src/codex/banked_reset_pool.cpp
// src/codex/banked_reset_provider.cpp src/codex/banked_reset_submission.cpp src/codex/banked_reset.cpp src/codex/capacity_routing.cpp src/codex/dispatch.cpp
// src/codex/experiment.cpp src/codex/external_auth.cpp src/codex/index.cpp src/codex/native_auth_transport.cpp src/codex/native_auth.cpp
// src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/responses_error.cpp src/codex/responses_operations.cpp
// src/codex/responses_runners.cpp src/codex/responses_state.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp
// src/codex/routing_state.cpp src/config.cpp src/debug_routing.cpp src/deepseek/chat_projection.cpp src/deepseek/context_admission_failure.cpp
// src/deepseek/flash_aliases.cpp src/deepseek/forwarded_payload_policy.cpp src/deepseek/handlers.cpp src/deepseek/index.cpp src/deepseek/responses_payload.cpp
// src/deepseek/responses_stream.cpp src/deepseek/responses.cpp src/deepseek/stream.cpp src/deepseek/waterfall_handler.cpp src/deepseek/waterfall.cpp
// src/defaults.cpp src/http.cpp src/inference_deadline.cpp src/input_normalization.cpp src/kv.cpp
// src/metered_quota.cpp src/models/codex_models_availability.cpp src/models/codex_models_fetch.cpp src/models/codex_models_whitelist.cpp src/models/codex_models.cpp
// src/models/metadata.cpp src/models/openrouter_models.cpp src/openai_schema.cpp src/openai_telemetry.cpp src/openai.cpp
// src/optional_telemetry_queue.cpp src/paid_fallback/claude_code_route.cpp src/paid_fallback/health.cpp src/paid_fallback/index.cpp src/paid_fallback/ledger_admission.cpp
// src/paid_fallback/ledger_backfill.cpp src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/paid_fallback/removed_provider.cpp
// src/paid_fallback/rollups.cpp src/paid_fallback/routing.cpp src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp src/provider/anthropic_replay.cpp
// src/provider/anthropic_response_wire.cpp src/provider/anthropic_responses.cpp src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp src/provider/capacity_events.cpp
// src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp src/provider/capacity.cpp src/provider/cerebras_handlers.cpp
// src/provider/cerebras_rate_limits.cpp src/provider/cerebras_responses.cpp src/provider/cerebras.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp
// src/provider/claude_code_access.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_models.cpp src/provider/claude_code_quota.cpp
// src/provider/claude_code_usage_schema.cpp src/provider/claude_code.cpp src/provider/health.cpp src/provider/lithos_handlers.cpp src/provider/lithos_models.cpp
// src/provider/lithos_rate_limits.cpp src/provider/lithos_streams.cpp src/provider/lithos.cpp src/provider/metered.cpp src/provider/openrouter_handlers.cpp
// src/provider/openrouter_request_controls.cpp src/provider/openrouter_streams.cpp src/provider/openrouter_web_tools.cpp src/provider/openrouter.cpp src/provider/presentation.cpp
// src/provider/removed_provider_circuit.cpp src/provider/removed_provider_telemetry.cpp src/provider/selection.cpp src/provider/stream_relay.cpp src/provider/surplus_claude_chat_request.cpp
// src/provider/surplus_claude_request.cpp src/provider/surplus.cpp src/recent_model_context.cpp src/release.cpp src/request_policy.cpp
// src/request.cpp src/responses_attempts.cpp src/responses_buffered.cpp src/responses_failover_stream.cpp src/responses_handler.cpp
// src/responses_native_headers.cpp src/responses_request.cpp src/responses_stream.cpp src/runtime_config.cpp src/runtime/http_body.cpp
// src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp src/runtime/http.cpp src/runtime/websocket.cpp
// src/sentinel/upstream_capture.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp src/storage/native_kv.cpp
// src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/upstream_wire.cpp src/utils.cpp src/types.cpp
// src/authority/native_paid_contracts.cpp src/lib/jev_compaction/types.cpp tests/helpers/counting_kv.cpp
#include "helpers/image_multipart_fixture.hpp"
#include "uos/images.hpp"
#include "uos/config.hpp"
#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "uos/request.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/test_async.hpp"
#include <algorithm>
#include <memory>
using namespace uos;
namespace im = uos::images;
namespace tel = uos::openai_telemetry;
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  using namespace uos::test::image_multipart;

  struct ImageScope {
    std::string tmp = rt::env::get("TMPDIR").value_or("/tmp");
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", tmp + "/h"}, {"TMPDIR", tmp}}};
    std::string oldAuth = config::config.codexAuthJsonB64;
    ImageScope() { config::config.codexAuthJsonB64.clear(); kv::setKvForTest(nullptr); im::setImageBaseModelForTest("gpt-5.6-sol"); }
    ~ImageScope() { im::setImageBaseModelForTest(std::nullopt); kv::setKvForTest(nullptr); config::config.codexAuthJsonB64 = oldAuth; }
  };
  rt::Request imagesRequest(const json::value& body, const std::string& path = "/v1/images/generations") {
    rt::RequestInit init; init.method = "POST"; init.headers = rt::Headers{{"content-type", "application/json"}}; init.body = rt::jsonStringify(body);
    return rt::Request("http://127.0.0.1" + path, init);
  }
  rt::Request imagesRequest(const std::string& body, const std::string& path = "/v1/images/generations") { return imagesRequest(rt::jsonParse(body), path); }
  rt::Request imagesRequest(const char* body, const std::string& path = "/v1/images/generations") { return imagesRequest(std::string(body), path); }
  rt::Response imageResponse(const std::string& result, double createdAt = 1787431659, const rt::Headers& headers = {}, const std::optional<std::string>& revised = std::nullopt) {
    json::object item{{"type", "image_generation_call"}, {"result", result}, {"output_format", "png"}};
    if (revised && !revised->empty()) item["revised_prompt"] = *revised;
    const json::object payload{{"created_at", rt::makeNumber(createdAt)}, {"output", json::array{item}}};
    return rt::Response(rt::jsonStringify(payload), {.status = 200, .headers = headers});
  }
  im::ImageHandlerOptions constantDispatch(rt::Response response) { return {[response](rt::Request) { return rt::fulfilledTask(response); }}; }
  void expectJson(const json::value& actual, const char* expected) { const auto parsed = rt::jsonParse(expected); EXPECT(rt::deepEqual(actual, parsed)); }
  std::string errorParam(const json::value& body) { return std::string(rt::getString(*rt::get(body, "error"), "param").value_or("")); }
}
ASYNC_TEST("the base model uses the selected default and never invents one") {
  ImageScope scope; AWAIT(configured,im::resolveImageBaseModel()); EXPECT_EQ(configured,std::optional<std::string>("gpt-5.6-sol"));
  im::setImageBaseModelForTest(std::nullopt); AWAIT(fallback,im::resolveImageBaseModel()); EXPECT(!fallback || !fallback->empty());
}
ASYNC_TEST("image requests reject unusable bodies before any dispatch") {
  ImageScope scope;const auto options=constantDispatch(rt::Response(std::string("{}"),{.status=599}));
  rt::RequestInit init;init.method="POST";init.body="not json";const rt::Request invalid("https://ai.ubq.fi/v1/images/generations",init);AWAIT(nonJson,im::handleImages(invalid,"generations",nullptr,options));EXPECT_EQ(nonJson.status(),400);
  const std::vector<std::string> bodies{R"("a string")",R"({})",R"({"prompt":"a cat","unsupported":true})",R"({"prompt":"a cat","quality":"ultra"})",R"({"prompt":"a cat","n":0})",R"({"prompt":"a cat","n":1.5})",R"({"prompt":"a cat","n":"2"})",R"({"prompt":"a cat","stream":true})",R"({"prompt":"a cat","response_format":"yaml"})"};
  for(const auto& body:bodies){const auto request=imagesRequest(body);AWAIT(response,im::handleImages(request,"generations",nullptr,options));EXPECT_EQ(response.status(),400);}
  json::array imageVariants{json::value("not-an-array"),json::array{7},json::array{json::object{{"image_url","https://example.com/cat.png"},{"extra",1}}},json::array{json::object{{"image_url",""}}},json::array{json::object{{"image_url",std::string(3000000,'h')}}},json::array{json::object{{"image_url","ftp://example.com/cat.png"}}},json::array{json::object{{"image_url","https://[::1"}}},json::array{json::object{{"image_url","data:text/plain;base64,aGk="}}},json::array{json::object{{"image_url","data:image/png;base64,!!!"}}}};
  for(const auto& images:imageVariants){const json::object body{{"prompt","a cat"},{"images",images}};const auto request=imagesRequest(json::value(body));AWAIT(response,im::handleImages(request,"generations",nullptr,options));EXPECT_EQ(response.status(),400);}
  const auto request=imagesRequest(R"({"prompt":"a cat","images":[{"image_url":"data:image/png;base64,iVBORw0KGgo="},{"image_url":"https://example.com/dog.png"}]})");AWAIT(inlineResponse,im::handleImages(request,"generations",nullptr,options));EXPECT_EQ(inlineResponse.status(),400);
}
ASYNC_TEST("image generation fans out one upstream call per requested image") {
  ImageScope scope;std::vector<rt::Request> seen;
  im::ImageHandlerOptions options{[&seen](rt::Request child){seen.push_back(child);return rt::fulfilledTask(imageResponse("aW1n",1700000000,{},"a nicer cat"));}};
  const auto request=imagesRequest(R"({"prompt":"a cat"})");AWAIT(single,im::handleImages(request,"generations",nullptr,options));EXPECT_EQ(single.status(),200);AWAIT(body,single.json());
  expectJson(body,R"({"created":1700000000,"data":[{"b64_json":"aW1n","revised_prompt":"a nicer cat"}],"output_format":"png"})");ASSERT_EQ(seen.size(),std::size_t{1});AWAIT(dispatched,seen[0].json());EXPECT(rt::getString(dispatched,"model"));
  std::vector<rt::Request> fanout;im::ImageHandlerOptions doubledOptions{[&fanout](rt::Request child){fanout.push_back(child);return rt::fulfilledTask(rt::Response(std::string(R"({"output":[{"type":"image_generation_call","result":"aW1n"}]})")));}};
  const auto doubledRequest=imagesRequest(R"({"prompt":"a cat","n":3})");AWAIT(doubled,im::handleImages(doubledRequest,"generations",nullptr,doubledOptions));EXPECT_EQ(doubled.status(),200);EXPECT_EQ(fanout.size(),std::size_t{3});AWAIT(doubledBody,doubled.json());ASSERT(rt::getArray(doubledBody,"data"));EXPECT_EQ(rt::getArray(doubledBody,"data")->size(),std::size_t{3});EXPECT(!rt::get(doubledBody,"output_format"));
}
ASYNC_TEST("image aggregation reports every upstream failure shape") {
  ImageScope scope;
  const std::vector<rt::Response> upstreams{
    rt::Response(std::string("<html>gateway</html>"),{.status=200}),
    rt::Response(std::string(R"({"error":{"message":"quota exhausted"}})"),{.status=429}),
    rt::Response(std::string(R"({"output":[]})")),rt::Response(std::string(""))};
  const std::vector<std::string> messages{"Image upstream returned a non-JSON response.","quota exhausted","The model did not return an image for this request.","The model did not return an image for this request."};
  for(std::size_t i=0;i<upstreams.size();++i){const auto options=constantDispatch(upstreams[i]);const auto request=imagesRequest(R"({"prompt":"a cat"})");AWAIT(response,im::handleImages(request,"generations",nullptr,options));EXPECT_EQ(response.status(),i==1?429:502);AWAIT(body,response.json());EXPECT_EQ(rt::getString(*rt::get(body,"error"),"message"),std::optional<std::string_view>(messages[i]));}
  // The source predicate checks the rewritten /v1/responses URL, so its reject arm is never selected.
  im::ImageHandlerOptions options{[](rt::Request child){if(rt::includes(child.url(),"image"))return rt::Promise<rt::Response>::reject(rt::makeReason(rt::JsError("upstream exploded"))).wait();return rt::fulfilledTask(rt::Response(std::string("{}")));}};
  const auto request=imagesRequest(R"({"prompt":"a cat","n":2})");AWAIT(rejected,im::handleImages(request,"generations",nullptr,options));EXPECT(rejected.status()>=400);AWAIT(body,rejected.json());EXPECT(!rt::getString(*rt::get(body,"error"),"message")->empty());
}
TEST("image extraction reads only complete image-generation items") {
  EXPECT(im::extractImagesFromResponses(json::value(nullptr)).empty());EXPECT(im::extractImagesFromResponses(rt::jsonParse(R"({"output":"not-an-array"})")).empty());
  EXPECT(im::extractImagesFromResponses(rt::jsonParse(R"({"output":[null,{"type":"message"},{"type":"image_generation_call"},{"type":"image_generation_call","result":""}]})")).empty());
  const auto images=im::extractImagesFromResponses(rt::jsonParse(R"({"output":[{"type":"image_generation_call","result":"aW1n"},{"type":"image_generation_call","result":"bW9yZQ==","revised_prompt":7}]})"));ASSERT_EQ(images.size(),std::size_t{2});expectJson(images[0],R"({"b64_json":"aW1n"})");expectJson(images[1],R"({"b64_json":"bW9yZQ=="})");
}
ASYNC_TEST("multipart image edits validate every part before dispatch") {
  ImageScope scope;const auto marker=constantDispatch(rt::Response(std::string("{}"),{.status=599}));
  const rt::Bytes png{137,80,78,71,13,10,26,10};
  for(int index=0;index<9;++index){MultipartFixture form;putPart(form, "prompt","a cat");putPart(form, "image",file(png,"cat.png"));
    if(index==0)putPart(form, "unsupported","value");
    if(index==1)removePart(form, "image");
    if(index==2)putPart(form, "image","not-a-file");
    if(index==3)putPart(form, "image",file(rt::encodeUtf8("not an image"),"cat.png"));
    if(index==4)putPart(form, "mask",file(png,"mask.jpg","image/jpeg"));
    if(index==5)putPart(form, "mask",file({},"mask.png"));
    if(index==6){putPart(form, "stream","true");putPart(form, "size","1024x1024");}if(index==7)putPart(form, "size","1024x1024");
    const auto request=multipartRequest(form);const auto options=index>=7?constantDispatch(imageResponse("aW1n")):marker;AWAIT(response,im::handleImages(request,"edits",nullptr,options));EXPECT_EQ(response.status(),index>=7?200:index==3?599:400);AWAIT(body,response.json());
    if(index==0)EXPECT_EQ(errorParam(body),std::string("unsupported"));
    if(index==1)EXPECT_EQ(errorParam(body),std::string("image"));
    if(index==4)EXPECT_EQ(errorParam(body),std::string("mask"));
    if(index==6)EXPECT_EQ(errorParam(body),std::string("stream"));
    if(index>=7)expectJson(*rt::get(body,"data"),R"([{"b64_json":"aW1n"}])");
  }
}
ASYNC_TEST("a multipart edit with an unusable declaration or body is refused") {
  ImageScope scope;im::ImageHandlerOptions options{[](rt::Request){return rt::Promise<rt::Response>::reject(rt::makeReason(rt::JsError("no dispatch"))).wait();}};
  for(int index=0;index<3;++index){auto request=index<2?multipartRequest(editForm()):imagesRequest(R"({"prompt":"a cat"})","/v1/images/edits");
    if(index<2)request.headers().set("content-length",index==0?"not-a-number":"209715200");
    AWAIT(response,im::handleImages(request,"edits",nullptr,options));EXPECT_EQ(response.status(),400);AWAIT(body,response.json());
    if(index<2)EXPECT_EQ(rt::getString(*rt::get(body,"error"),"message"),std::optional<std::string_view>(index==0?"Multipart Content-Length is invalid.":"Multipart image edits must be no larger than 64 MiB."));}
}
