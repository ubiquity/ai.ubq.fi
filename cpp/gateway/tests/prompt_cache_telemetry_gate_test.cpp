// Port of tests/prompt-cache-telemetry-gate.test.ts: all seven source cases, preserving the five gate cases.
// Links the sources of tests/handler_terminal_usage_rollup_test.cpp.
#include "tests/helpers/terminal_log_harness.hpp"
#include "uos/cache/telemetry_gate.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::cache::telemetry_gate;

namespace {
  std::string encodeKey(const rt::KvKey& key) {
    json::array parts;
    for (const rt::KvKeyPart& part : key) parts.push_back(json::value(part.string()));
    return rt::jsonStringify(json::value(std::move(parts)));
  }

  // The test's `class MemoryKv`: only `get` and `atomic().sum().commit()` exist; any other member
  // is not a function.
  class MemoryKv final : public rt::Kv {
  public:
    rt::OrderedMap<std::string, rt::KvValue> values;
    int atomicCommits = 0;
    bool failNextCommit = false;

    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions) override {
      const rt::KvValue* stored = values.get(encodeKey(key));
      rt::KvEntryMaybe<> entry{key, stored != nullptr ? *stored : rt::KvValue(nullptr),
                               stored != nullptr ? std::optional<std::string>("00000000000000000001") : std::nullopt};
      co_return entry;
    }
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey>, rt::KvGetOptions) override { throw rt::TypeError("kv.getMany is not a function"); }
    rt::Task<rt::KvCommitResult> set(rt::KvKey, rt::KvValue, rt::KvSetOptions) override { throw rt::TypeError("kv.set is not a function"); }
    rt::Task<void> delete_(rt::KvKey) override { throw rt::TypeError("kv.delete is not a function"); }
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector, rt::KvListOptions) override { throw rt::TypeError("kv.list is not a function"); }
    std::unique_ptr<rt::AtomicOperation> atomic() override;
    void close() override { throw rt::TypeError("kv.close is not a function"); }

    void setCounter(const rt::KvKey& key, std::uint64_t value) { values.set(encodeKey(key), rt::KvValue(rt::KvU64(value))); }
    void setRawCounter(const rt::KvKey& key, rt::KvValue value) { values.set(encodeKey(key), std::move(value)); }
  };

  class SumOperation final : public rt::AtomicOperation {
  public:
    explicit SumOperation(MemoryKv& kv_) : kv(kv_) {}
    using rt::AtomicOperation::check;
    using rt::AtomicOperation::max;
    using rt::AtomicOperation::min;
    using rt::AtomicOperation::sum;
    rt::AtomicOperation& sum(rt::KvKey key, const rt::BigInt& amount) override {
      sums.emplace_back(std::move(key), amount);
      return *this;
    }
    rt::Task<rt::KvCommitResult> commit() override {
      kv.atomicCommits += 1;
      if (kv.failNextCommit) {
        kv.failNextCommit = false;
        throw rt::JsError("raw-model-must-not-reach-the-result");
      }
      for (const auto& [key, amount] : sums) {
        const rt::KvValue* existing = kv.values.get(encodeKey(key));
        const rt::BigInt current = existing != nullptr && existing->isU64() ? existing->asU64().toBigInt() : rt::BigInt(0);
        kv.values.set(encodeKey(key), rt::KvValue(rt::KvU64(current + amount)));
      }
      rt::KvCommitResult committed{true, "00000000000000000001"};
      co_return committed;
    }
    rt::AtomicOperation& check(std::vector<rt::AtomicCheck>) override { throw rt::TypeError("operation.check is not a function"); }
    rt::AtomicOperation& mutate(std::vector<rt::KvMutation>) override { throw rt::TypeError("operation.mutate is not a function"); }
    rt::AtomicOperation& min(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("operation.min is not a function"); }
    rt::AtomicOperation& max(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("operation.max is not a function"); }
    rt::AtomicOperation& set(rt::KvKey, rt::KvValue, rt::KvSetOptions) override { throw rt::TypeError("operation.set is not a function"); }
    rt::AtomicOperation& delete_(rt::KvKey) override { throw rt::TypeError("operation.delete is not a function"); }

  private:
    MemoryKv& kv;
    std::vector<std::pair<rt::KvKey, rt::BigInt>> sums;
  };

  std::unique_ptr<rt::AtomicOperation> MemoryKv::atomic() { return std::make_unique<SumOperation>(*this); }

  const std::string RELEASE = "0123456789abcdef0123456789abcdef01234567";
  PromptCacheTelemetryEvent event() {
    return PromptCacheTelemetryEvent{.provider = "chatgpt_codex", .model = "gpt-cache-target", .route = "responses", .status = 200, .completed = true,
                                     .usageTelemetryStatus = "reported", .cacheWriteTokensPresent = true};
  }
  PromptCacheTelemetryGateOptions optionsFor(const std::shared_ptr<MemoryKv>& kv, std::string release = RELEASE) {
    PromptCacheTelemetryGateOptions options;
    options.kv = std::shared_ptr<rt::Kv>(kv);
    options.release = std::move(release);
    return options;
  }
  const PromptCacheTelemetryBaselineTarget target{.provider = "chatgpt_codex", .model = "gpt-cache-target"};

  const PromptCacheTelemetryCounterKeys::Route* routeKeys(const PromptCacheTelemetryCounterKeys& keys, std::string_view route) {
    for (const PromptCacheTelemetryCounterKeys::Route& entry : keys.routes)
      if (entry.route == route) return &entry;
    return nullptr;
  }
  const PromptCacheTelemetryRouteBaseline* routeBaseline(const PromptCacheTelemetryBaselineResult& result, std::string_view route) {
    for (const PromptCacheTelemetryRouteBaseline& entry : result.routes)
      if (entry.route == route) return &entry;
    return nullptr;
  }
  std::optional<std::uint64_t> counterAt(MemoryKv& kv, const rt::KvKey& key) {
    const rt::KvValue* value = kv.values.get(encodeKey(key));
    if (value == nullptr || !value->isU64()) return std::nullopt;
    return value->asU64().value;
  }
}

ASYNC_TEST("prompt-cache telemetry gate atomically stores only redacted completed 2xx inference counters") {
  const std::shared_ptr<MemoryKv> kv = std::make_shared<MemoryKv>();
  const std::string rawModel = "gpt-secret-model-name-must-not-persist";
  PromptCacheTelemetryEvent secret = event();
  secret.model = rawModel;
  const PromptCacheTelemetryRecordResult recorded = co_await recordPromptCacheTelemetry(secret, optionsFor(kv));
  EXPECT_EQ(recorded.status, std::string("recorded"));
  EXPECT_EQ(recorded.reason, std::string("recorded"));
  EXPECT(recorded.model_hash.has_value() && !recorded.model_hash->empty());
  EXPECT(!rt::includes(rt::jsonStringify(json::value(toJson(recorded))), rawModel));
  EXPECT_EQ(kv->atomicCommits, 1);
  EXPECT_EQ(kv->values.size(), std::size_t{3});
  for (const auto& [key, value] : kv->values) {
    EXPECT(!rt::includes(key, rawModel));
    EXPECT(value.isU64());
    EXPECT(value.isU64() && value.asU64().value == 1);
  }

  PromptCacheTelemetryEvent incompleteEvent = event();
  incompleteEvent.completed = false;
  const PromptCacheTelemetryRecordResult incomplete = co_await recordPromptCacheTelemetry(incompleteEvent, optionsFor(kv));
  PromptCacheTelemetryEvent failedEvent = event();
  failedEvent.status = 503;
  const PromptCacheTelemetryRecordResult failed = co_await recordPromptCacheTelemetry(failedEvent, optionsFor(kv));
  EXPECT_EQ(incomplete.reason, std::string("not_completed_2xx"));
  EXPECT_EQ(failed.reason, std::string("not_completed_2xx"));
  EXPECT_EQ(kv->atomicCommits, 1);

  kv->failNextCommit = true;
  const PromptCacheTelemetryRecordResult unavailable = co_await recordPromptCacheTelemetry(secret, optionsFor(kv));
  EXPECT_EQ(unavailable.status, std::string("unavailable"));
  EXPECT_EQ(unavailable.reason, std::string("kv_unavailable"));
  EXPECT(!rt::includes(rt::jsonStringify(json::value(toJson(unavailable))), rawModel));
}

ASYNC_TEST("prompt-cache telemetry gate records invalid usage separately from reported coverage") {
  const std::shared_ptr<MemoryKv> kv = std::make_shared<MemoryKv>();
  PromptCacheTelemetryEvent invalidEvent = event();
  invalidEvent.usageTelemetryStatus = "invalid";
  const PromptCacheTelemetryRecordResult recorded = co_await recordPromptCacheTelemetry(invalidEvent, optionsFor(kv));
  EXPECT_EQ(recorded.status, std::string("recorded"));
  EXPECT_EQ(kv->atomicCommits, 1);

  const std::optional<PromptCacheTelemetryCounterKeys> keys = co_await resolvePromptCacheTelemetryCounterKeys(target, optionsFor(kv));
  ASSERT(keys.has_value());
  const PromptCacheTelemetryCounterKeys::Route* responseKeys = routeKeys(*keys, "responses");
  ASSERT(responseKeys != nullptr);
  EXPECT_EQ(counterAt(*kv, responseKeys->completed), std::optional<std::uint64_t>(1));
  EXPECT_EQ(counterAt(*kv, responseKeys->invalid), std::optional<std::uint64_t>(1));
  EXPECT(!kv->values.has(encodeKey(responseKeys->reported)));
  EXPECT(!kv->values.has(encodeKey(responseKeys->cache_write_reported)));

  const PromptCacheTelemetryBaselineResult baseline = co_await readPromptCacheTelemetryBaseline(target, optionsFor(kv));
  ASSERT(baseline.aggregate.has_value());
  EXPECT_EQ(baseline.aggregate->invalid, std::string("1"));
  const PromptCacheTelemetryRouteBaseline* responses = routeBaseline(baseline, "responses");
  EXPECT(responses != nullptr && responses->invalid == "1");
  EXPECT_EQ(baseline.aggregate->reported, std::string("0"));
  EXPECT_EQ(baseline.aggregate->cache_write_reported, std::string("0"));
}

ASYNC_TEST("prompt-cache telemetry gate fails closed for an unknown release and malformed durable counters") {
  const std::shared_ptr<MemoryKv> kv = std::make_shared<MemoryKv>();
  const PromptCacheTelemetryRecordResult unknown = co_await recordPromptCacheTelemetry(event(), optionsFor(kv, "unknown"));
  EXPECT(rt::deepEqual(json::value(toJson(unknown)),
                       rt::jsonParse(R"({"status":"ignored","reason":"unknown_release","release":null,"provider":null,"route":null,"model_hash":null})")));
  EXPECT_EQ(kv->atomicCommits, 0);

  const PromptCacheTelemetryBaselineResult unknownBaseline = co_await readPromptCacheTelemetryBaseline(target, optionsFor(kv, "unknown"));
  EXPECT_EQ(unknownBaseline.status, std::string("not_ready"));
  EXPECT_EQ(unknownBaseline.reason, std::string("unknown_release"));

  PromptCacheTelemetryGateOptions releaseOnly;
  releaseOnly.release = RELEASE;
  const std::optional<PromptCacheTelemetryCounterKeys> keys = co_await resolvePromptCacheTelemetryCounterKeys(target, releaseOnly);
  ASSERT(keys.has_value());
  const PromptCacheTelemetryCounterKeys::Route* responseKeys = routeKeys(*keys, "responses");
  ASSERT(responseKeys != nullptr);
  kv->setRawCounter(responseKeys->completed, rt::KvValue::object({{"value", rt::KvValue(rt::BigInt(1))}}));
  const PromptCacheTelemetryBaselineResult malformed = co_await readPromptCacheTelemetryBaseline(target, optionsFor(kv));
  EXPECT_EQ(malformed.status, std::string("not_ready"));
  EXPECT_EQ(malformed.reason, std::string("invalid_counter"));
}

ASYNC_TEST("prompt-cache telemetry gate requires aggregate volume, every observed route, and reported coverage") {
  const std::shared_ptr<MemoryKv> kv = std::make_shared<MemoryKv>();
  const std::optional<PromptCacheTelemetryCounterKeys> keys = co_await resolvePromptCacheTelemetryCounterKeys(target, optionsFor(kv));
  ASSERT(keys.has_value());
  const PromptCacheTelemetryCounterKeys::Route* responseKeys = routeKeys(*keys, "responses");
  const PromptCacheTelemetryCounterKeys::Route* chatKeys = routeKeys(*keys, "chat.completions");
  ASSERT(responseKeys != nullptr);
  ASSERT(chatKeys != nullptr);
  const auto minimum = static_cast<std::uint64_t>(PROMPT_CACHE_TELEMETRY_MIN_COMPLETED);
  kv->setCounter(responseKeys->completed, minimum);
  kv->setCounter(responseKeys->reported, minimum);
  kv->setCounter(responseKeys->cache_write_reported, minimum);
  kv->setCounter(chatKeys->completed, 999);
  kv->setCounter(chatKeys->reported, 999);
  kv->setCounter(chatKeys->cache_write_reported, 999);

  const PromptCacheTelemetryBaselineResult undersizedRoute = co_await readPromptCacheTelemetryBaseline(target, optionsFor(kv));
  EXPECT_EQ(undersizedRoute.status, std::string("not_ready"));
  EXPECT_EQ(undersizedRoute.reason, std::string("route_completed_below_minimum"));
  EXPECT(undersizedRoute.aggregate.has_value() && undersizedRoute.aggregate->completed == "10999");

  PromptCacheTelemetryEvent chat = event();
  chat.route = "chat.completions";
  co_await recordPromptCacheTelemetry(chat, optionsFor(kv));
  const PromptCacheTelemetryBaselineResult eligible = co_await readPromptCacheTelemetryBaseline(target, optionsFor(kv));
  EXPECT_EQ(eligible.status, std::string("eligible"));
  EXPECT_EQ(eligible.reason, std::string("eligible"));
  ASSERT(eligible.aggregate.has_value());
  EXPECT_EQ(eligible.aggregate->completed, std::string("11000"));
  EXPECT_EQ(eligible.aggregate->reported_coverage, std::optional<double>(1));
  json::array routes;
  for (const PromptCacheTelemetryRouteBaseline& route : eligible.routes)
    routes.push_back(json::array{json::value(route.route), json::value(route.completed), route.reported_coverage_passed, route.cache_write_reported_coverage_passed});
  EXPECT(rt::deepEqual(routes, rt::jsonParse(R"([["responses","10000",true,true],["chat.completions","1000",true,true]])")));

  PromptCacheTelemetryEvent partial = chat;
  partial.usageTelemetryStatus = "partial";
  for (int index = 0; index < 6; index += 1) co_await recordPromptCacheTelemetry(partial, optionsFor(kv));
  const PromptCacheTelemetryBaselineResult incompleteCoverage = co_await readPromptCacheTelemetryBaseline(target, optionsFor(kv));
  EXPECT_EQ(incompleteCoverage.status, std::string("not_ready"));
  EXPECT_EQ(incompleteCoverage.reason, std::string("route_reported_coverage_below_minimum"));
  const PromptCacheTelemetryRouteBaseline* chatRoute = routeBaseline(incompleteCoverage, "chat.completions");
  ASSERT(chatRoute != nullptr);
  EXPECT_EQ(chatRoute->completed, std::string("1006"));
  EXPECT_EQ(chatRoute->reported, std::string("1000"));
  EXPECT(!chatRoute->reported_coverage_passed);
}

ASYNC_TEST("prompt-cache telemetry gate requires complete cache-write coverage and counts a valid zero as present") {
  const std::shared_ptr<MemoryKv> kv = std::make_shared<MemoryKv>();
  const std::optional<PromptCacheTelemetryCounterKeys> keys = co_await resolvePromptCacheTelemetryCounterKeys(target, optionsFor(kv));
  ASSERT(keys.has_value());
  const PromptCacheTelemetryCounterKeys::Route* responseKeys = routeKeys(*keys, "responses");
  ASSERT(responseKeys != nullptr);

  PromptCacheTelemetryEvent absentEvent = event();
  absentEvent.cacheWriteTokensPresent = false;
  const PromptCacheTelemetryRecordResult absent = co_await recordPromptCacheTelemetry(absentEvent, optionsFor(kv));
  const PromptCacheTelemetryRecordResult zero = co_await recordPromptCacheTelemetry(event(), optionsFor(kv));
  EXPECT_EQ(absent.status, std::string("recorded"));
  EXPECT_EQ(zero.status, std::string("recorded"));
  EXPECT_EQ(counterAt(*kv, responseKeys->cache_write_reported), std::optional<std::uint64_t>(1));

  kv->setCounter(responseKeys->completed, 10'000);
  kv->setCounter(responseKeys->reported, 10'000);
  kv->setCounter(responseKeys->cache_write_reported, 9'949);
  const PromptCacheTelemetryBaselineResult belowBoundary = co_await readPromptCacheTelemetryBaseline(target, optionsFor(kv));
  EXPECT_EQ(belowBoundary.status, std::string("not_ready"));
  EXPECT_EQ(belowBoundary.reason, std::string("aggregate_cache_write_reported_coverage_below_minimum"));
  EXPECT(belowBoundary.aggregate.has_value() && belowBoundary.aggregate->cache_write_reported_coverage == std::optional<double>(0.9949));

  kv->setCounter(responseKeys->cache_write_reported, 9'950);
  const PromptCacheTelemetryBaselineResult boundary = co_await readPromptCacheTelemetryBaseline(target, optionsFor(kv));
  EXPECT_EQ(boundary.status, std::string("eligible"));
  EXPECT(boundary.aggregate.has_value() && boundary.aggregate->cache_write_reported_coverage == std::optional<double>(0.995));

  kv->setCounter(responseKeys->cache_write_reported, 10'001);
  const PromptCacheTelemetryBaselineResult malformed = co_await readPromptCacheTelemetryBaseline(target, optionsFor(kv));
  EXPECT_EQ(malformed.status, std::string("not_ready"));
  EXPECT_EQ(malformed.reason, std::string("invalid_counter"));
}

ASYNC_TEST("terminal telemetry failures do not change non-stream or SSE responses") {
  uos::tests::helpers::terminal_log_harness::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  int telemetryAttempts = 0;
  int analyticsAttempts = 0;
  const std::vector<std::string> contentTypes{"application/json", "text/event-stream"};
  for (const auto& contentType : contentTypes) {
    const auto body = contentType == "application/json" ? std::string("complete") : std::string("data: complete\n\n");
    auto options = uos::tests::helpers::terminal_log_harness::terminalOptions("telemetry-failure-" + contentType);
    options.recordTelemetry = [&telemetryAttempts](auto) {
      ++telemetryAttempts;
      return rt::Promise<PromptCacheTelemetryRecordResult>::reject(rt::makeReason(rt::JsError("optional terminal telemetry unavailable")));
    };
    options.recordCacheAnalytics = [&analyticsAttempts](auto) {
      ++analyticsAttempts;
      return rt::Promise<uos::cache::prompt_analytics_core::PromptCacheAnalyticsRecordResult>::reject(rt::makeReason(rt::JsError("optional cache analytics unavailable")));
    };
    const rt::ResponseInit init{.status = 200, .headers = rt::Headers{{"Content-Type", contentType}}};
    auto wrapped = uos::handler::terminal_log::withTerminalRequestLog(rt::Response(body, init), options);
    auto response = co_await wrapped.wait();
    EXPECT_EQ(response.status(), 200);
    auto text = co_await response.text();
    EXPECT_EQ(text, body);
  }
  EXPECT_EQ(telemetryAttempts, 2);
  EXPECT_EQ(analyticsAttempts, 2);
  EXPECT_EQ(uos::tests::helpers::terminal_log_harness::terminalPayloads(logs).size(), std::size_t{2});
}

ASYNC_TEST("a terminal writer can finish after setup of a later writer throws") {
  uos::tests::helpers::terminal_log_harness::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  const auto resume = rt::withResolvers<void>();
  const auto done = rt::withResolvers<void>();
  auto capture = std::make_shared<std::string>("terminal writer callback");
  const std::weak_ptr observer = capture;
  auto options = uos::tests::helpers::terminal_log_harness::terminalOptions("writer-lifetime");
  options.recordTelemetry = [capture = std::move(capture), waiting = resume.promise, finish = done.resolve](PromptCacheTelemetryEvent)
    -> rt::Task<PromptCacheTelemetryRecordResult> {
    co_await waiting.wait();
    EXPECT_EQ(*capture, "terminal writer callback");
    finish();
    co_return PromptCacheTelemetryRecordResult{"ignored", "unsupported_route", std::nullopt, std::nullopt, std::nullopt, std::nullopt};
  };
  options.recordCacheAnalytics = [](auto) -> rt::Promise<uos::cache::prompt_analytics_core::PromptCacheAnalyticsRecordResult> {
    throw rt::JsError("controlled writer setup failure");
  };
  const rt::Response response("ok");
  auto logging = uos::handler::terminal_log::withTerminalRequestLog(response, std::move(options));
  const auto result = co_await logging.wait();
  EXPECT_EQ(result.status(), 200);
  ASSERT(!observer.expired());
  resume.resolve();
  co_await done.promise.wait();
  co_await rt::microtask();
  EXPECT(observer.expired());
}

TEST("quota accounting warnings expose only error classes") {
  rt::console::Capture warnings({rt::console::Method::warn});
  const std::string secret = "quota-secret-must-not-reach-logs";
  uos::handler::terminal_log::warnQuotaAccountingFailure({"responses", "quota-warning-redaction"}, rt::makeReason(rt::TypeError(secret)));
  ASSERT(warnings.calls().size() == 1);
  const auto text = warnings.calls()[0].text;
  EXPECT(!rt::includes(text, secret));
  EXPECT(rt::includes(text, "TypeError"));
}
