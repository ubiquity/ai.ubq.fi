#pragma once
#include "tests/helpers/responses_conformance_client.hpp"
// Exactly two genuine gateway steps from tests/helpers/metered-routing-matrix-part1.ts:986,1122 (16 original assert sites).
// The other sequence steps are predecessor-module work and are not claimed by this partial parent registration.
#include "tests/helpers/openai_compat_harness.hpp"
#include "uos/config.hpp"
#include "uos/debug_routing.hpp"
#include "uos/paid_fallback/removed_provider.hpp"
#include "uos/rt/test_async.hpp"
namespace uos::tests::helpers::metered_routing_matrix_part1 {
  namespace h=openai_compat_harness;
  namespace removed=paid_fallback::removed_provider;
  inline rt::Request request(const std::string& input) { return rt::Request("http://localhost/v1/responses",rt::RequestInit{.method="POST",.headers=rt::Headers{{"Content-Type","application/json"}},.body=rt::jsonStringify(json::object{{"model",h::DEFAULT_TEST_MODEL},{"input",input}})}); }
  inline std::vector<json::value> terminals(const rt::console::Capture& logs) { std::vector<json::value> result;for(const auto& call:logs.calls())if(call.args.size()>1&&call.args[0]=="[ai.ubq.fi] request_terminal")result.push_back(rt::jsonParse(call.args[1].string()));return result; }
  inline void debugScenario(const std::string& scenario) { h::kvStore.set(h::keyToString(debug_routing::DEBUG_ROUTING_KEY),rt::KvValue(json::object{{"scenario",scenario},{"expires_at_ms",rt::dateNow()+60'000},{"updated_at_ms",rt::dateNow()}}));debug_routing::resetDebugRoutingCacheForTest(); }
  inline rt::Task<void> runMeteredPaidFallbackMatrixPart1() {
    responses_conformance_client::stageGatewayAssets();h::Fixture fixture;if(fixture.scratch)rt::env::set("HOME",*fixture.scratch+"/h");const bool deploy=config::config.isDeploy;config::config.isDeploy=false;rt::env::set("METERED_API_KEY","metered-test-key");const auto cleanup=rt::scopeExit([deploy]{removed::setRemovedProviderTestAdapterForTest(std::nullopt);removed::setRemovedProviderApiKeyForTest(std::nullopt);debug_routing::resetDebugRoutingCacheForTest();config::config.isDeploy=deploy;});
    {
      // failed RemovedProvider fallback restores primary Codex correlation
      rt::console::Capture logs({rt::console::Method::info});removed::setRemovedProviderApiKeyForTest(std::string("removed-provider-test-key"));removed::RemovedProviderTestAdapter adapter;
      adapter.fetchResponses=[](json::object,removed::RemovedProviderResponsesOptions)->rt::Promise<removed::RemovedProviderResponsesResult>{throw rt::JsError("RemovedProvider fallback failed");};adapter.modelFromEvent=[](const json::object&){return std::optional<std::string>{};};adapter.isEligibleModel=[](const std::string& model){return model==h::DEFAULT_TEST_MODEL;};removed::setRemovedProviderTestAdapterForTest(adapter);debugScenario("normal");h::prepareFetchMock();
      rt::GlobalFetchOverride fetch(h::createFetchMock([](std::string,std::optional<std::string>,rt::RequestInit){return rt::fulfilledTask(rt::Response(R"({"error":{"message":"Codex primary failed"}})",rt::ResponseInit{.status=500,.headers=rt::Headers{{"Content-Type","application/json"},{"X-Request-Id","failed-codex-primary-id"}}}));}));
      auto handling=h::gatewayHandler(request("preserve primary correlation"));auto response=co_await std::move(handling);EXPECT_EQ(response.status(),500);EXPECT_EQ(response.headers().get("x-uos-upstream"),"chatgpt_codex");EXPECT_EQ(response.headers().get("x-uos-provider-request-id"),"failed-codex-primary-id");const auto body=co_await response.json();(void)body;const auto events=terminals(logs);ASSERT_EQ(events.size(),1U);const auto& terminal=events[0];EXPECT_EQ(rt::getString(terminal,"provider"),"chatgpt_codex");EXPECT_EQ(rt::getString(terminal,"provider_request_id"),"failed-codex-primary-id");EXPECT_EQ(rt::getNumber(terminal,"account_slot"),1);EXPECT(rt::getString(terminal,"account_cohort_id"));
      removed::setRemovedProviderTestAdapterForTest(std::nullopt);removed::setRemovedProviderApiKeyForTest(std::nullopt);
    }
    {
      // RemovedProvider recovery clears failed Codex request metadata
      rt::console::Capture logs({rt::console::Method::info});removed::setRemovedProviderApiKeyForTest(std::string("removed-provider-test-key"));debugScenario("removed_provider_first");h::prepareFetchMock();
      rt::GlobalFetchOverride fetch(h::createFetchMock([](std::string,std::optional<std::string>,rt::RequestInit){return rt::fulfilledTask(rt::Response(R"({"error":{"message":"Codex recovery failed"}})",rt::ResponseInit{.status=500,.headers=rt::Headers{{"Content-Type","application/json"},{"X-Request-Id","failed-codex-request-id"}}}));}));
      auto handling=h::gatewayHandler(request("recover through RemovedProvider"));auto response=co_await std::move(handling);EXPECT_EQ(response.status(),502);EXPECT_EQ(response.headers().get("x-uos-upstream"),"removed_provider");EXPECT(!response.headers().get("x-uos-provider-request-id"));const auto events=terminals(logs);ASSERT(!events.empty());const auto& terminal=events[0];EXPECT_EQ(rt::getString(terminal,"provider"),"removed_provider");EXPECT(terminal.at("provider_request_id").is_null());EXPECT(terminal.at("account_slot").is_null());EXPECT(terminal.at("account_cohort_id").is_null());if(auto body=response.body()){auto cancel=body->cancel();co_await std::move(cancel);}removed::setRemovedProviderApiKeyForTest(std::nullopt);
    }
    auto settled=h::settleHarness();co_await std::move(settled);
  }
}
