// Links: tests/lithos_models_differential_test.cpp src/provider/lithos_models.cpp src/provider/lithos.cpp src/provider/lithos_rate_limits.cpp
// src/sentinel/upstream_capture.cpp src/utils.cpp
#include "uos/provider/lithos_models.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/gzip.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/test_async.hpp"
namespace rt = uos::rt;
namespace json = uos::json;
namespace lm = uos::provider::lithos_models;
namespace {
#include "lithos_models_differential_vectors.inc"
  struct FetchState { double calls = 0; std::string scenario = "normal"; std::vector<rt::Resolve<rt::Response>> pending; };
  json::object normalPayload() { return rt::jsonParse(R"({"data":[null,{}, {"id":" A ","created":3},{"id":"A","created":9},{"id":"B","created":2.5},{"id":"C","created":-2},{"id":"","created":4}]})").as_object(); }
  rt::Task<rt::Response> scriptedFetch(std::shared_ptr<FetchState> state, rt::RequestInit init) {
    ++state->calls;
    if (state->scenario == "throw") throw rt::JsError("fixture network failure");
    if (state->scenario == "bad") co_return rt::Response::json(json::object{{"data", 3}});
    if (state->scenario == "429") co_return rt::Response(std::string(""), rt::ResponseInit{.status = 429});
    if (state->scenario == "wait") {
      auto [promise, resolve, reject] = rt::withResolvers<rt::Response>();
      static_cast<void>(reject); state->pending.push_back(resolve);
      auto response = co_await promise.wait(); co_return response;
    }
    if (state->scenario == "abort") { co_await rt::microtask(); std::rethrow_exception(init.signal->reason()); }
    co_return rt::Response::json(normalPayload());
  }
}
ASYNC_TEST("Lithos model catalog differential: cache, normalization, backoff, force, fencing, abort and coalescing") {
  rt::env::ScopedEnvironment environment({{"HOME", "/nonexistent/w05c6-home"}, {"LITHOSAI_API_KEY", "fixture-key"}});
  double now = 1780000000000.0;
  rt::globalDateNow() = [&now] { return now; };
  auto state = std::make_shared<FetchState>();
  rt::Fetch fetcher([state](rt::RequestInfo, rt::RequestInit init) { return scriptedFetch(state, init); });
  const auto previousTransport = rt::fetchTransport();
  const auto cleanup = rt::scopeExit([previousTransport] { rt::setFetchTransport(previousTransport); });
  json::array trace;
  const auto record = [&](const std::string& label, const std::optional<lm::LithosModelsSnapshot>& value) { trace.push_back(json::object{{"label", label}, {"value", value ? json::value_from(*value) : json::value(nullptr)}, {"calls", state->calls}}); };
  lm::FetchLithosModelsOptions cachedOnly{.cachedOnly = true};
  auto cachedTask = lm::fetchLithosModels(cachedOnly); auto cached = co_await std::move(cachedTask); record("cached empty", cached);
  rt::env::remove("LITHOSAI_API_KEY");
  lm::FetchLithosModelsOptions options{.fetcher = fetcher};
  auto absentTask = lm::fetchLithosModels(options); auto absent = co_await std::move(absentTask); record("no key", absent);
  rt::env::set("LITHOSAI_API_KEY", "fixture-key");
  auto listedTask = lm::fetchLithosModels(options); auto listed = co_await std::move(listedTask); record("listed", listed);
  auto cacheTask = lm::fetchLithosModels(options); auto cache = co_await std::move(cacheTask); record("cache", cache);
  now += 300001; state->scenario = "bad";
  auto badTask = lm::fetchLithosModels(options); auto bad = co_await std::move(badTask); record("bad payload", bad);
  state->scenario = "normal";
  auto backoffTask = lm::fetchLithosModels(options); auto backoff = co_await std::move(backoffTask); record("backoff", backoff);
  options.force = true;
  auto forceTask = lm::fetchLithosModels(options); auto forced = co_await std::move(forceTask); record("forced", forced);
  state->scenario = "429";
  auto httpTask = lm::fetchLithosModels(options); auto http = co_await std::move(httpTask); record("http failure", http);
  state->scenario = "throw";
  auto throwTask = lm::fetchLithosModels(options); auto thrown = co_await std::move(throwTask); record("throw failure", thrown);
  state->scenario = "wait";
  rt::Promise<std::optional<lm::LithosModelsSnapshot>> older(lm::fetchLithosModels(options));
  rt::Promise<std::optional<lm::LithosModelsSnapshot>> newer(lm::fetchLithosModels(options));
  state->pending[1](rt::Response::json(rt::jsonParse(R"({"data":[{"id":"newest","created":8}]})")));
  auto newerValue = co_await newer.wait(); record("newer", newerValue);
  state->pending[0](rt::Response::json(rt::jsonParse(R"({"data":[{"id":"stale","created":1}]})")));
  auto olderValue = co_await older.wait(); record("older fenced", olderValue);
  state->scenario = "abort";
  rt::AbortController controller; options.signal = controller.signal();
  rt::Promise<std::optional<lm::LithosModelsSnapshot>> abort(lm::fetchLithosModels(options));
  controller.abort(rt::makeReason(rt::JsError("caller cancelled")));
  bool rejected = false;
  try { auto value = co_await abort.wait(); static_cast<void>(value); } catch (const rt::JsError& error) {
    rejected = true; trace.push_back(json::object{{"label", "caller abort"}, {"value", json::object{{"name", error.name}, {"message", error.message}}}, {"calls", state->calls}});
  }
  EXPECT(rejected);
  record("snapshot", lm::lithosModelsSnapshot());
  now += 300001; state->scenario = "wait";
  auto transport = std::make_shared<rt::MockTransport>([state](rt::Request request) { rt::RequestInit init{.signal = request.signal()}; return scriptedFetch(state, init); });
  rt::setFetchTransport(transport);
  rt::Promise<std::optional<lm::LithosModelsSnapshot>> one(lm::fetchLithosModels());
  rt::Promise<std::optional<lm::LithosModelsSnapshot>> two(lm::fetchLithosModels());
  state->pending[2](rt::Response::json(rt::jsonParse(R"({"data":[{"id":"coalesced","created":2}]})")));
  auto oneValue = co_await one.wait(); record("coalesced one", oneValue);
  auto twoValue = co_await two.wait(); record("coalesced two", twoValue);
  const auto expected = rt::jsonParse(rt::decodeUtf8(rt::gzip::gunzipSync(rt::fromBase64(recordedVectors))));
  EXPECT(rt::deepEqual(trace, expected));
  EXPECT_EQ(trace.size(), 15u);
  EXPECT_EQ(state->calls, 9);
}
