// Differential cases for src/paid-fallback/removed-provider.ts (its only listed
// test file, tests/helpers/openai-compat-harness.ts, is a harness over the whole
// gateway). Every expected value was recorded by running the TypeScript module
// under Bun 1.4.0.
#include "uos/paid_fallback/removed_provider.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::paid_fallback::removed_provider;
}

ASYNC_TEST("differential: the removed provider boundary matches Bun") {
  EXPECT_EQ(readRemovedProviderApiKey(), std::nullopt);
  setRemovedProviderApiKeyForTest("k");
  EXPECT_EQ(readRemovedProviderApiKey(), std::optional<std::string>("k"));
  setRemovedProviderApiKeyForTest("");
  EXPECT_EQ(readRemovedProviderApiKey(), std::optional<std::string>(""));
  setRemovedProviderApiKeyForTest(std::nullopt);
  EXPECT_EQ(readRemovedProviderApiKey(), std::nullopt);
  EXPECT_EQ(isEligibleRemovedProviderModel("m"), false);
  const json::object event{{"model", "m"}};
  EXPECT_EQ(removedProviderModelFromEvent(event), std::nullopt);
  const json::object response{{"task_type", "x"}};
  EXPECT_EQ(removedProviderTaskTypeFromResponse(response), std::nullopt);
  const json::object value{{"a", 1}};
  EXPECT(&stripRemovedProviderMetadata(value) == &value);

  auto order = std::make_shared<std::vector<std::string>>();
  const json::object metadata{{"meta", 1}};
  const rt::Promise<std::nullptr_t> derived = deriveRemovedProviderSessionId(std::string("principal"), metadata);
  derived.then([order](std::nullptr_t) { order->push_back("derive:null"); });
  rt::queueMicrotask([order] { order->push_back("micro"); });
  const std::nullptr_t session = co_await derived.wait();
  EXPECT(session == nullptr);
  EXPECT_EQ(*order, std::vector<std::string>({"derive:null", "micro"}));

  rt::AbortController controller;
  RemovedProviderResponsesOptions options{"key", std::nullopt, controller.signal(), std::nullopt, {}};
  std::string disabled = "resolved";
  auto disabledCall = fetchRemovedProviderResponses(json::object(), options).wait();
  try {
    co_await std::move(disabledCall);
  } catch (const rt::JsError& error) {
    disabled = error.toString();
  }
  EXPECT_EQ(disabled, "Error: The removed provider is disabled.");

  auto events = std::make_shared<std::vector<std::string>>();
  RemovedProviderTestAdapter adapter;
  adapter.fetchResponses = [events](json::object body, RemovedProviderResponsesOptions opts) {
    return rt::Promise<RemovedProviderResponsesResult>(rt::async([events, body = std::move(body), opts = std::move(opts)]() -> rt::Task<RemovedProviderResponsesResult> {
      events->push_back("body:" + rt::jsonStringify(json::value(body)));
      if (opts.beforeDispatch) {
        const rt::Promise<void> before = opts.beforeDispatch();
        co_await before.wait();
      }
      if (opts.timing && opts.timing->onDispatch) opts.timing->onDispatch();
      if (opts.timing && opts.timing->onHeaders) opts.timing->onHeaders();
      RemovedProviderResponsesResult result;
      result.response = rt::Response(std::string("ok:" + opts.apiKey + ":" + opts.sessionId.value_or("null")));
      co_return result;
    }));
  };
  adapter.modelFromEvent = [](const json::object& candidate) -> std::optional<std::string> {
    const std::optional<std::string_view> model = rt::getString(candidate, "model");
    return model ? std::optional<std::string>(*model) : std::nullopt;
  };
  adapter.isEligibleModel = [](const std::string& model) { return model == "eligible"; };
  setRemovedProviderTestAdapterForTest(adapter);
  EXPECT_EQ(isEligibleRemovedProviderModel("eligible"), true);
  EXPECT_EQ(isEligibleRemovedProviderModel("other"), false);
  const json::object withModel{{"model", "x"}};
  EXPECT_EQ(removedProviderModelFromEvent(withModel), std::optional<std::string>("x"));
  EXPECT_EQ(removedProviderModelFromEvent(json::object()), std::nullopt);
  RemovedProviderResponsesOptions dispatchOptions = options;
  dispatchOptions.sessionId = "s";
  RemovedProviderResponseTiming timing;
  timing.onDispatch = [events] { events->push_back("dispatch"); };
  timing.onHeaders = [events] { events->push_back("headers"); };
  dispatchOptions.timing = timing;
  dispatchOptions.beforeDispatch = [events] {
    events->push_back("before");
    return rt::Promise<void>::resolve();
  };
  const json::object body{{"model", "x"}};
  auto dispatch = fetchRemovedProviderResponses(body, dispatchOptions).wait();
  const RemovedProviderResponsesResult result = co_await std::move(dispatch);
  const std::string text = co_await result.response.text();
  EXPECT_EQ(text, "ok:key:s");
  EXPECT_EQ(*events, std::vector<std::string>({"body:{\"model\":\"x\"}", "before", "dispatch", "headers"}));

  RemovedProviderTestAdapter throwing;
  throwing.fetchResponses = [](json::object, RemovedProviderResponsesOptions) -> rt::Promise<RemovedProviderResponsesResult> {
    throw rt::JsError("RemovedProvider must not run");
  };
  throwing.modelFromEvent = [](const json::object&) -> std::optional<std::string> { return std::nullopt; };
  throwing.isEligibleModel = [](const std::string&) { return true; };
  setRemovedProviderTestAdapterForTest(throwing);
  std::string syncThrow = "returned";
  try {
    (void)fetchRemovedProviderResponses(json::object(), options);
  } catch (const rt::JsError& error) {
    syncThrow = error.toString();
  }
  EXPECT_EQ(syncThrow, "Error: RemovedProvider must not run");

  setRemovedProviderTestAdapterForTest(std::nullopt);
  std::string disabledAgain = "resolved";
  auto againCall = fetchRemovedProviderResponses(json::object(), options).wait();
  try {
    co_await std::move(againCall);
  } catch (const rt::JsError& error) {
    disabledAgain = error.toString();
  }
  EXPECT_EQ(disabledAgain, "Error: The removed provider is disabled.");
}
