// Differential cases for src/runtime-config.ts, recorded under Bun 1.4.0 (src/runtime-config.ts has
// one direct test file, tests/runtime_config_test.cpp; these cover the rest of its branches):
// - normalizeRuntimeConfig, buildRuntimeConfig and compactRuntimeCodexModels on the 81 inputs of
//   runtime_config_differential_vectors.inc, compared as JSON.stringify text (member order counts)
//   or as the thrown error's name and message;
// - the loadRuntimeConfig / cacheRuntimeConfig sequences below, whose expected values Bun printed for
//   the same steps against the same `{ get }` stub. One is a TS quirk kept on purpose: a load that
//   finishes without suspending (an explicit null KV) clears the in-flight slot before the promise is
//   stored, so the settled promise stays "in flight" and answers every later load until reset.
// Links the sources of tests/runtime_config_test.cpp.
#include "uos/runtime_config.hpp"

#include "uos/kv.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <stdexcept>
#include <string>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::runtime_config;

namespace {
#include "runtime_config_differential_vectors.inc"

  std::string text(const std::optional<RuntimeConfigV2>& config) { return config ? rt::jsonStringify(toJson(*config)) : std::string("null"); }

  // The Bun script's `{ get: async (key) => ({ key, value: stored, versionstamp: "1" }) }`.
  class StubKv final : public rt::Kv {
  public:
    rt::KvValue stored;
    bool fail = false;
    int reads = 0;

    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions) override {
      reads += 1;
      if (fail) throw rt::JsError("boom");
      rt::KvEntryMaybe<> entry;
      entry.key = std::move(key);
      entry.value = stored;
      entry.versionstamp = "1";
      co_return entry;
    }
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey>, rt::KvGetOptions) override { throw std::logic_error("unused"); }
    rt::Task<rt::KvCommitResult> set(rt::KvKey, rt::KvValue, rt::KvSetOptions) override { throw std::logic_error("unused"); }
    rt::Task<void> delete_(rt::KvKey) override { throw std::logic_error("unused"); }
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector, rt::KvListOptions) override { throw std::logic_error("unused"); }
    std::unique_ptr<rt::AtomicOperation> atomic() override { throw std::logic_error("unused"); }
    void close() override {}
  };

  // buildRuntimeConfig({ models: [{ slug: "m1" }], source: "s", updated_at_ms: 5 }, { nowMs: 10 })
  RuntimeConfigV2 baseConfig() {
    const json::value snapshot = rt::jsonParse(R"({"models":[{"slug":"m1"}],"source":"s","updated_at_ms":5})");
    return buildRuntimeConfig(snapshot.as_object(), BuildRuntimeConfigOptions{.nowMs = 10});
  }

  const std::string BASE_TEXT =
    R"({"version":2,"default_model":"m1","default_reasoning_effort":"medium","codex_models":{"models":[{"slug":"m1","supported_reasoning_levels":[]}],"source":"s","updated_at_ms":5},"updated_at_ms":10})";

  struct Stub {
    std::shared_ptr<rt::Kv> kv;
    StubKv* state;
  };
  Stub makeStub(const RuntimeConfigV2& stored) {
    auto stub = std::make_shared<StubKv>();
    stub->stored = rt::KvValue(toJson(stored));
    StubKv* state = stub.get();
    return Stub{std::move(stub), state};
  }
}

TEST("normalize, build and compact match the recorded Bun results") {
  const json::value vectors = rt::jsonParse(RUNTIME_CONFIG_VECTORS);
  std::size_t checked = 0;
  for (const json::value& vector : vectors.as_array()) {
    const json::object& entry = vector.as_object();
    const std::string op(entry.at("op").as_string());
    std::string expected;
    if (const json::value* out = rt::get(entry, "out")) {
      expected = rt::jsonStringify(*out);
    } else {
      const json::object& error = entry.at("error").as_object();
      expected = "error:" + std::string(error.at("name").as_string()) + ":" + std::string(error.at("message").as_string());
    }
    std::string actual;
    try {
      if (op == "normalize") {
        actual = text(normalizeRuntimeConfig(entry.at("input")));
      } else if (op == "build") {
        const json::object& options = entry.at("options").as_object();
        BuildRuntimeConfigOptions built;
        if (const std::optional<std::string_view> model = rt::getString(options, "defaultModel")) built.defaultModel = std::string(*model);
        if (const std::optional<std::string_view> effort = rt::getString(options, "defaultReasoningEffort")) built.defaultReasoningEffort = std::string(*effort);
        built.nowMs = rt::getNumber(options, "nowMs").value_or(42);
        actual = rt::jsonStringify(toJson(buildRuntimeConfig(entry.at("snapshot").as_object(), built)));
      } else {
        const json::value& input = entry.at("input");
        const bool undefinedInput = input.is_string() && input.get_string() == "<undefined>";
        actual = rt::jsonStringify(toJson(compactRuntimeCodexModels(undefinedInput ? nullptr : &input)));
      }
    } catch (const rt::JsError& error) {
      actual = "error:" + error.name + ":" + error.message;
    }
    EXPECT_EQ(actual, expected);
    checked += 1;
  }
  EXPECT_EQ(checked, std::size_t{81});
}

ASYNC_TEST("a load without KV leaves its settled promise in flight until reset") {
  resetRuntimeConfigCacheForTest();
  Stub stub = makeStub(baseConfig());
  auto noneLoad = loadRuntimeConfig(nullptr, 100);
  const std::optional<RuntimeConfigV2> none = co_await std::move(noneLoad);
  EXPECT_EQ(text(none), std::string("null"));
  auto stuckLoad = loadRuntimeConfig(stub.kv, 100);
  const std::optional<RuntimeConfigV2> stuck = co_await std::move(stuckLoad);
  EXPECT_EQ(text(stuck), std::string("null"));
  EXPECT_EQ(stub.state->reads, 0);

  resetRuntimeConfigCacheForTest();
  auto freshLoad = loadRuntimeConfig(stub.kv, 100);
  const std::optional<RuntimeConfigV2> fresh = co_await std::move(freshLoad);
  EXPECT_EQ(text(fresh), BASE_TEXT);
  EXPECT_EQ(stub.state->reads, 1);
  auto cachedLoad = loadRuntimeConfig(stub.kv, 200);
  const std::optional<RuntimeConfigV2> cached = co_await std::move(cachedLoad);
  EXPECT_EQ(text(cached), BASE_TEXT);
  EXPECT_EQ(stub.state->reads, 1);
  auto staleLoad = loadRuntimeConfig(nullptr, 100 + 300'001);
  const std::optional<RuntimeConfigV2> stale = co_await std::move(staleLoad);
  EXPECT_EQ(text(stale), BASE_TEXT);
  auto stillStuckLoad = loadRuntimeConfig(stub.kv, 100 + 600'002);
  const std::optional<RuntimeConfigV2> stillStuck = co_await std::move(stillStuckLoad);
  EXPECT_EQ(text(stillStuck), BASE_TEXT);
  EXPECT_EQ(stub.state->reads, 1);
  resetRuntimeConfigCacheForTest();
}

ASYNC_TEST("concurrent loads share one read") {
  resetRuntimeConfigCacheForTest();
  Stub stub = makeStub(baseConfig());
  const rt::Promise<std::optional<RuntimeConfigV2>> first(loadRuntimeConfig(stub.kv, 100));
  const rt::Promise<std::optional<RuntimeConfigV2>> second(loadRuntimeConfig(stub.kv, 100));
  const std::optional<RuntimeConfigV2> a = co_await first.wait();
  const std::optional<RuntimeConfigV2> b = co_await second.wait();
  EXPECT(a.has_value() && a->default_model == "m1");
  EXPECT(b.has_value() && b->default_model == "m1");
  EXPECT_EQ(stub.state->reads, 1);
  resetRuntimeConfigCacheForTest();
}

ASYNC_TEST("a failed read warns and serves the stale configuration for one more TTL") {
  resetRuntimeConfigCacheForTest();
  Stub stub = makeStub(baseConfig());
  rt::console::Capture capture({rt::console::Method::warn});
  stub.state->fail = true;
  auto noneLoad = loadRuntimeConfig(stub.kv, 100);
  const std::optional<RuntimeConfigV2> none = co_await std::move(noneLoad);
  EXPECT_EQ(text(none), std::string("null"));
  EXPECT_EQ(stub.state->reads, 1);
  ASSERT_EQ(capture.calls().size(), std::size_t{1});
  EXPECT(capture.calls()[0].args[0] == "[ai.ubq.fi] Runtime configuration refresh failed; using stale configuration:");
  EXPECT(capture.calls()[0].args[1].kind() == rt::console::Value::Kind::error && capture.calls()[0].args[1].errorInfo().message == "boom");

  resetRuntimeConfigCacheForTest();
  capture.clear();
  stub.state->reads = 0;
  stub.state->fail = false;
  auto warmLoad1 = loadRuntimeConfig(stub.kv, 100);
  co_await std::move(warmLoad1);
  stub.state->fail = true;
  auto staleLoad = loadRuntimeConfig(stub.kv, 100 + 300'000);
  const std::optional<RuntimeConfigV2> stale = co_await std::move(staleLoad);
  EXPECT(stale.has_value() && stale->default_model == "m1");
  EXPECT_EQ(stub.state->reads, 2);
  EXPECT_EQ(capture.calls().size(), std::size_t{1});
  auto recachedLoad = loadRuntimeConfig(stub.kv, 100 + 300'000 + 299'999);
  const std::optional<RuntimeConfigV2> recached = co_await std::move(recachedLoad);
  EXPECT(recached.has_value() && recached->default_model == "m1");
  EXPECT_EQ(stub.state->reads, 2);
  auto expiredLoad = loadRuntimeConfig(stub.kv, 100 + 600'000);
  const std::optional<RuntimeConfigV2> expired = co_await std::move(expiredLoad);
  EXPECT(expired.has_value() && expired->default_model == "m1");
  EXPECT_EQ(stub.state->reads, 3);
  resetRuntimeConfigCacheForTest();
}

ASYNC_TEST("an invalid stored configuration falls back to the stale one") {
  resetRuntimeConfigCacheForTest();
  Stub stub = makeStub(baseConfig());
  stub.state->stored = rt::KvValue(rt::jsonParse(R"({"version":2})"));
  auto noneLoad = loadRuntimeConfig(stub.kv, 100);
  const std::optional<RuntimeConfigV2> none = co_await std::move(noneLoad);
  EXPECT_EQ(text(none), std::string("null"));
  EXPECT_EQ(stub.state->reads, 1);
  stub.state->stored = rt::KvValue(toJson(baseConfig()));
  auto warmLoad2 = loadRuntimeConfig(stub.kv, 100);
  co_await std::move(warmLoad2);
  stub.state->stored = rt::KvValue(rt::jsonParse(R"({"version":3})"));
  auto staleLoad = loadRuntimeConfig(stub.kv, 100 + 300'000);
  const std::optional<RuntimeConfigV2> stale = co_await std::move(staleLoad);
  EXPECT(stale.has_value() && stale->default_model == "m1");
  EXPECT_EQ(stub.state->reads, 3);
  resetRuntimeConfigCacheForTest();
}

ASYNC_TEST("stored values with non-JSON parts normalize as the TS guards read them") {
  resetRuntimeConfigCacheForTest();
  Stub stub = makeStub(baseConfig());
  // { ...cfg, extra: new Date(5), codex_models: { ...cfg.codex_models, models: [new Map(), m1,
  //   { slug: "m2", supported_reasoning_levels: [new Date(1), "low", undefined], reasoning_effort_wire_map: new Uint8Array([1]) }] } }
  rt::KvValue stored(toJson(baseConfig()));
  rt::KvObject& object = stored.asObject();
  object.set("extra", rt::KvValue(rt::KvDate{5}));
  rt::KvObject& codexModels = object.find("codex_models")->asObject();
  rt::KvArray models;
  models.push_back(rt::KvValue(rt::KvMap{}));
  models.push_back(codexModels.at("models")[0]);
  rt::KvObject second;
  second.set("slug", rt::KvValue("m2"));
  rt::KvArray levels;
  levels.push_back(rt::KvValue(rt::KvDate{1}));
  levels.push_back(rt::KvValue("low"));
  levels.push_back(rt::KvValue());
  second.set("supported_reasoning_levels", rt::KvValue(std::move(levels)));
  second.set("reasoning_effort_wire_map", rt::KvValue(rt::KvBytes{1}));
  models.push_back(rt::KvValue(std::move(second)));
  codexModels.set("models", rt::KvValue(std::move(models)));
  stub.state->stored = stored;
  auto loadedLoad = loadRuntimeConfig(stub.kv, 100);
  const std::optional<RuntimeConfigV2> loaded = co_await std::move(loadedLoad);
  EXPECT_EQ(text(loaded),
            std::string(R"({"version":2,"default_model":"m1","default_reasoning_effort":"medium","codex_models":{"models":[{"slug":"m1","supported_reasoning_levels":[]},{"slug":"m2","supported_reasoning_levels":["low"]}],"source":"s","updated_at_ms":5},"updated_at_ms":10})"));
  resetRuntimeConfigCacheForTest();
}

ASYNC_TEST("cacheRuntimeConfig stores the normalized configuration or throws") {
  resetRuntimeConfigCacheForTest();
  Stub stub = makeStub(baseConfig());
  RuntimeConfigV2 padded = baseConfig();
  padded.default_model = " m1 ";
  cacheRuntimeConfig(padded, 1'000);
  auto cachedLoad = loadRuntimeConfig(stub.kv, 1'000 + 299'999);
  const std::optional<RuntimeConfigV2> cached = co_await std::move(cachedLoad);
  EXPECT_EQ(text(cached), BASE_TEXT);
  EXPECT_EQ(stub.state->reads, 0);
  RuntimeConfigV2 unknown = baseConfig();
  unknown.default_model = "zz";
  EXPECT_THROWS_MESSAGE(cacheRuntimeConfig(unknown, 1'000), RuntimeConfigError, "runtime config is invalid or exceeds one 4 KiB read unit");
  resetRuntimeConfigCacheForTest();
}

ASYNC_TEST("an undefined override opens the shared KV") {
  resetRuntimeConfigCacheForTest();
  Stub stub = makeStub(baseConfig());
  uos::kv::setKvForTest(stub.kv);
  auto sharedLoad = loadRuntimeConfig(std::nullopt, 100);
  const std::optional<RuntimeConfigV2> shared = co_await std::move(sharedLoad);
  EXPECT(shared.has_value() && shared->default_model == "m1");
  EXPECT_EQ(stub.state->reads, 1);
  uos::kv::setKvForTest(nullptr);
  resetRuntimeConfigCacheForTest();
  auto noneLoad = loadRuntimeConfig(std::nullopt, 100);
  const std::optional<RuntimeConfigV2> none = co_await std::move(noneLoad);
  EXPECT_EQ(text(none), std::string("null"));
  EXPECT_EQ(stub.state->reads, 1);
  auto reopenedLoad = loadRuntimeConfig(stub.kv, 100);
  const std::optional<RuntimeConfigV2> reopened = co_await std::move(reopenedLoad);
  EXPECT(reopened.has_value() && reopened->default_model == "m1");
  EXPECT_EQ(stub.state->reads, 2);
  uos::kv::setKvForTest(nullptr);
  resetRuntimeConfigCacheForTest();
}
