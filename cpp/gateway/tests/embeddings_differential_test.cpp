// 32 bounded Bun-recorded edge cases: parsing/UTF-16/order, chunk limits, exact AES-GCM,
// Retry-After coercion, arbitrary abort reasons, and raw stored job-field preservation.
// Links: tests/embeddings_differential_test.cpp
// src/embeddings/voyage.cpp tests/helpers/embeddings_harness.cpp src/embeddings/ledger.cpp src/utils.cpp src/kv.cpp
// src/embeddings/jobs.cpp src/openai_telemetry.cpp src/responses_failover_stream.cpp src/openai.cpp src/codex/responses_error.cpp
// src/request.cpp src/runtime_config.cpp src/embeddings/handlers.cpp src/responses_stream.cpp src/http.cpp
// src/defaults.cpp src/config.cpp src/provider/removed_provider_telemetry.cpp src/request_policy.cpp src/models/codex_models.cpp
// src/inference_deadline.cpp src/openai_schema.cpp src/provider/surplus.cpp src/auth/relay.cpp src/upstream_wire.cpp
// src/storage/kv.cpp src/provider/cerebras.cpp src/deepseek/index.cpp src/provider/surplus_claude_request.cpp src/storage/sqlite_readonly.cpp
// src/provider/cerebras_rate_limits.cpp src/storage/native_kv.cpp src/provider/claude_code_models.cpp src/provider/claude_code_accounts.cpp src/provider/anthropic_chat_responses.cpp
// src/release.cpp src/models/codex_models_fetch.cpp src/provider/anthropic_response_wire.cpp src/types.cpp src/codex/dispatch.cpp
// src/provider/metered.cpp src/provider/claude_code_credentials.cpp src/provider/health.cpp src/provider/selection.cpp src/bounded_response_body.cpp
// src/provider/anthropic_chat_stream.cpp src/authority/native_codex_contracts.cpp src/provider/anthropic_replay.cpp src/provider/surplus_claude_chat_request.cpp
// src/provider/lithos.cpp src/codex/auth.cpp src/models/codex_models_availability.cpp src/codex/routing_mutations.cpp src/authority/native_codex_capacity.cpp
// src/provider/capacity_events.cpp src/authority/native_codex_mutations.cpp src/codex/capacity_routing.cpp src/codex/rate_limit_429.cpp src/codex/auth_refresh.cpp
// src/codex/routing_state.cpp src/codex/native_auth.cpp src/codex/external_auth.cpp src/provider/claude_code.cpp src/provider/claude_code_quota.cpp
// src/authority/native_codex_evaluation.cpp src/provider/lithos_rate_limits.cpp src/authority/native_codex_429.cpp src/codex/banked_reset.cpp src/provider/capacity.cpp
// src/provider/capacity_history.cpp src/provider/capacity_contract.cpp src/provider/claude_capacity_rollups.cpp src/provider/anthropic_responses.cpp src/provider/claude_capacity_history.cpp
// src/provider/capacity_parse.cpp src/provider/claude_code_usage_schema.cpp src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_read.cpp src/paid_fallback/ledger_stats.cpp
// src/codex/native_auth_transport.cpp src/provider/capacity_rollups.cpp src/provider/capacity_analytics.cpp src/codex/experiment.cpp src/codex/reset_settings.cpp
// src/codex/banked_reset_provider.cpp src/metered_quota.cpp src/runtime/websocket.cpp src/codex/account_routing.cpp src/codex/routing_serial.cpp
// src/codex/routing_evaluation.cpp src/authority/native_codex.cpp src/codex/overage_settings.cpp
#include "tests/helpers/embeddings_harness.hpp"
#include "uos/embeddings/voyage.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"
#include <limits>
#define AWAIT(name, expr) auto name##Task=(expr);auto name=co_await std::move(name##Task)
namespace {
  namespace rt=uos::rt;namespace json=uos::json;namespace h=uos::tests::helpers::embeddings_harness;namespace v=uos::embeddings::voyage;namespace l=uos::embeddings::ledger;
#include "embeddings_differential_vectors.inc"
  const json::array& vectors(){static const auto value=rt::jsonParse(kEmbeddingsDifferentialVectors);return value.get_array();}
  std::size_t selected=0;
  v::FetchVoyageEmbeddingsParams params(){return {"fixture","voyage-4-large",{"hello"},"document",256,"float",true,1700000010000};}
  rt::Request jobRequest(){const json::object body{{"model","voyage-4-large"},{"input","hello"},{"input_type","query"},{"dimensions",256}};return rt::Request("https://ai.ubq.fi/uos/embedding-jobs",rt::RequestInit{.method="POST",.body=rt::jsonStringify(body)});}
  rt::Task<void> runSelected() {
    h::HarnessScope fixture;rt::globalDateNow()=[]{return 1700000000000.0;};const auto& item=vectors()[selected];const std::string kind(rt::getString(item,"kind").value());json::value actual;
    if(kind=="parse"){
      const auto body=rt::jsonParse(*rt::getString(item,"body"));const auto result=v::parseUosEmbeddingsRequest(body.get_object());
      if(const auto success=std::get_if<l::EmbeddingsParseResultOk>(&result)){json::array inputs;for(const auto& text:success->value.inputs)inputs.push_back(json::object{{"chars",rt::makeNumber(static_cast<double>(rt::length(text)))},{"tokens",rt::makeNumber(v::estimateTokens(text))}});actual=json::object{{"ok",true},{"model",success->value.model},{"inputs",inputs},{"total_chars",rt::makeNumber(success->value.total_chars)},{"profile",json::value_from(success->value.profile)}};}
      else {const auto response=std::get<l::EmbeddingsParseResultNotOk>(result).response;AWAIT(error,response.json());actual=json::object{{"ok",false},{"status",rt::makeNumber(static_cast<double>(response.status()))},{"body",error}};}
    }else if(kind=="chunk"){
      const std::vector<v::EmbeddingsChunkItem> items{{"a",""},{"b","😀"},{"c",std::string(9,'x')},{"d","z"}};const auto chunks=v::chunkByTokenBudget(items,rt::stringToNumber(*rt::getString(item,"maxItems")),rt::stringToNumber(*rt::getString(item,"maxTokens")));json::array out;for(const auto& chunk:chunks){json::array hashes;for(const auto& value:chunk)hashes.push_back(json::value(value.hash));out.push_back(hashes);}actual=out;
    }else if(kind=="float32"){
      const double nan=std::numeric_limits<double>::quiet_NaN(),infinity=std::numeric_limits<double>::infinity();const std::vector<double> values{0,-0.0,nan,-nan,infinity,-infinity,1e-50,3.5e38,1.0000000596046448};actual=v::floatEmbeddingToBase64(values);
    }else if(kind=="crypto"){
      rt::crypto::ScopedRandomSource random([](std::span<std::uint8_t> bytes){for(std::size_t i=0;i<bytes.size();++i)bytes[i]=static_cast<std::uint8_t>(i);});
      const std::string text="Unicode 😀 "+rt::fromCharCode(0xd800)+"\n";AWAIT(record,v::encryptEmbeddingsJobInput("source-seed",text));AWAIT(plain,v::decryptEmbeddingsJobInput("source-seed",record));AWAIT(wrong,v::decryptEmbeddingsJobInput("other-seed",record));
      const json::object expected{{"record",*rt::get(item,"record")},{"plain",*rt::get(item,"plain")},{"wrong",*rt::get(item,"wrong")}};actual=json::object{{"record",json::value_from(record)},{"plain",plain?json::value(*plain):json::value(nullptr)},{"wrong",wrong?json::value(*wrong):json::value(nullptr)}};EXPECT(rt::deepEqual(actual,expected));co_return;
    }else if(kind=="retry"){
      const std::string retry(rt::getString(item,"input").value());rt::GlobalFetchOverride fetch(rt::Fetch([retry](rt::RequestInfo,rt::RequestInit){return rt::Response("private upstream body",rt::ResponseInit{.status=429,.headers=rt::Headers{{"Retry-After",retry}}});}));auto input=params();rt::Reason error;try{AWAIT(ignored,v::fetchVoyageEmbeddings(input));(void)ignored;}catch(...){error=std::current_exception();}ASSERT(error);try{std::rethrow_exception(error);}catch(const v::VoyageEmbeddingsError&e){json::object out{{"name",e.name},{"message",e.message},{"status",e.status}};rt::objectDefine(out,"retry_after_ms",rt::orUndefined(e.retry_after_ms));actual=out;}
    }else if(kind=="abort"){
      rt::AbortController controller;controller.abort(rt::thrown(*rt::get(item,"reason")));bool cancelled=false,started=false;auto input=params();input.downstreamSignal=controller.signal();input.beforeProviderDispatch=[&](std::string){uos::api_key_policy::ApiKeyProviderDispatch dispatch{[&]{started=true;},[&]{cancelled=true;return rt::fulfilledTask();}};return rt::fulfilledTask(std::optional<uos::api_key_policy::ApiKeyProviderDispatch>(dispatch));};rt::Reason error;try{AWAIT(ignored,v::fetchVoyageEmbeddings(input));(void)ignored;}catch(...){error=std::current_exception();}ASSERT(error);json::value value;if(rt::isError(error))value=json::object{{"name",rt::errorName(error).value()},{"message",rt::errorMessage(error).value()}};else value=rt::thrownValue(error).value_or(nullptr);actual=json::object{{"error",value},{"cancelled",cancelled},{"started",started}};
    }else if(kind=="job-opaque"){
      h::kvStore.clear();h::kvStore.set(h::keyToString(l::VOYAGE_API_KEY_KV_KEY),rt::KvValue("fixture"));h::kvStore.set(h::keyToString(l::VOYAGE_RATE_LIMIT_KEY),rt::KvValue(json::object{{"window_start_ms",1700000000000LL},{"requests",3},{"tokens",0}}));
      AWAIT(created,h::handleEmbeddingsJobCreate(jobRequest(),"fixture-token"));EXPECT_EQ(created.status(),202);AWAIT(body,created.json());const std::string id(rt::getString(body,"id").value());AWAIT(tokenHash,h::sha256Hex("fixture-token"));const auto lookup=*h::kvStore.get(h::keyToString(h::embeddingsJobLookupKey(tokenHash,id)));const auto key=h::embeddingsJobKey(tokenHash,lookup["cache_profile_key"].asString(),id);auto raw=*h::kvStore.get(h::keyToString(key));raw.asObject().set("opaque",rt::KvValue(rt::KvBytes{7,9}));raw.asObject().set("future",rt::KvValue(json::object{{"enabled",true}}));h::kvStore.set(h::keyToString(key),raw);h::resetVoyageRateLimit();rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo,rt::RequestInit){return h::voyageOkResponse(1,256);}));
      AWAIT(finished,h::handleEmbeddingsJobGet(rt::Request("https://ai.ubq.fi/uos/embedding-jobs/"+id),"fixture-token",id));AWAIT(done,finished.json());const auto stored=*h::kvStore.get(h::keyToString(key));json::array opaque;for(auto byte:stored["opaque"].as<rt::KvBytes>())opaque.push_back(rt::makeNumber(static_cast<double>(byte)));json::array keys;for(const auto&[name,value]:stored.asObject()){(void)value;keys.push_back(json::value(name));}double remaining=0;for(const auto&[encoded,value]:h::kvStore){(void)value;const auto parsed=rt::jsonParse(encoded);const auto first=rt::get(parsed,0),second=rt::get(parsed,1),fourth=rt::get(parsed,3);if(first&&second&&fourth&&rt::toString(*first)=="embeddings"&&rt::toString(*second)=="jobs"&&rt::toString(*fourth)=="input")++remaining;}
      actual=json::object{{"status",rt::makeNumber(static_cast<double>(finished.status()))},{"jobStatus",*rt::get(done,"status")},{"usage",*rt::get(*rt::get(done,"result"),"usage")},{"opaque",opaque},{"future",rt::storedJsonView(stored["future"])},{"keys",keys},{"inputsRemaining",rt::makeNumber(remaining)}};
    }else throw rt::JsError("unknown differential kind");
    const auto expected=rt::get(item,"result");ASSERT(expected);EXPECT_EQ(rt::jsonStringify(actual),rt::jsonStringify(*expected));
  }
  const bool registered=[] {for(std::size_t i=0;i<vectors().size();++i){const std::string name="embeddings differential "+std::string(rt::getString(vectors()[i],"kind").value())+" "+rt::numberToString(static_cast<double>(i));rt::test::registry().push_back({name,[i]{selected=i;rt::test::runAsync(&runSelected);}});}return true;}();
}
