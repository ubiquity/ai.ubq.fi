#include "tests/helpers/prompt_cache_scope_experiment_harness.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include <algorithm>

namespace uos::tests::helpers::prompt_cache_scope_experiment_harness {
namespace {
  void require(bool condition, const std::string& message) { if (!condition) throw rt::JsError(message, "AssertionError"); }
  class ExperimentAtomic : public rt::ForwardingAtomicOperation {
  public:
    ExperimentAtomic(std::unique_ptr<rt::AtomicOperation> operation, ExperimentKv& owner_) : rt::ForwardingAtomicOperation(std::move(operation)), owner(owner_) {}
    rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override {
      writes.push_back({"set", key, value}); return rt::ForwardingAtomicOperation::set(std::move(key), std::move(value), options);
    }
    rt::AtomicOperation& delete_(rt::KvKey key) override {
      writes.push_back({"delete", key}); return rt::ForwardingAtomicOperation::delete_(std::move(key));
    }
    rt::Task<rt::KvCommitResult> commit() override {
      if (owner.beforeAtomicCommit) owner.beforeAtomicCommit(writes);
      if (owner.refuseAtomicCommit && owner.refuseAtomicCommit(writes)) { const rt::KvCommitResult refused; co_return refused; }
      auto task = rt::ForwardingAtomicOperation::commit(); const auto result = co_await std::move(task);
      if (result.ok && owner.afterAtomicCommit) owner.afterAtomicCommit(writes);
      co_return result;
    }
  private:
    ExperimentKv& owner;
    std::vector<AtomicWrite> writes;
  };
  rt::Task<std::string> versionstamp(rt::KvKey key, std::string message) {
    const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
    auto task = kv->get(key, strong); rt::Promise<rt::KvEntryMaybe<>> pending(std::move(task)); const auto row = co_await pending.wait();
    if (!row.versionstamp || row.versionstamp->empty()) throw rt::JsError(message);
    co_return *row.versionstamp;
  }
}
std::shared_ptr<ExperimentKv> kv = std::make_shared<ExperimentKv>();
std::string encodeKey(const rt::KvKey& key) { return codex_diff_kv::keyOf(key); }
double compareStrings(const std::string& left, const std::string& right) { return rt::compare(left, right); }
double cycleValue(std::span<const double> cycle, double step) {
  const std::vector<double> values(cycle.begin(), cycle.end());
  const auto* value = rt::at(values, step);
  if (!value) throw rt::JsError("cache telemetry cycle has no step " + rt::numberToString(step));
  return *value;
}
std::string fetchInputUrl(const rt::RequestInfo& input) {
  return std::visit([](const auto& value) -> std::string {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, rt::Request>) return value.url();
    else if constexpr (std::is_same_v<T, rt::URL>) return value.toString();
    else return value;
  }, input);
}
std::string requestBodyText(const rt::RequestInit& init) {
  if (!init.body || std::holds_alternative<std::nullptr_t>(*init.body)) return "";
  if (const auto* text = std::get_if<std::string>(&*init.body)) return *text;
  throw rt::JsError("scope experiment requests must send a JSON string body");
}
void ExperimentKv::clear() { rows.clear(); version = 0; atomicCalls = 0; beforeGet = {}; afterAtomicCommit = {}; beforeAtomicCommit = {}; refuseAtomicCommit = {}; }
void ExperimentKv::put(const rt::KvKey& key, const rt::KvValue& value) { rows.set(encodeKey(key), Row{key, value, stamp()}); }
rt::Task<rt::KvEntryMaybe<>> ExperimentKv::get(rt::KvKey key, rt::KvGetOptions options) {
  if (options.consistency) require(options.consistency == rt::KvConsistencyLevel::strong, "scope experiment KV reads must be strong");
  if (beforeGet) { rt::Promise<void> pending(beforeGet(key)); co_await pending.wait(); }
  else co_await rt::microtask();
  co_return entryOf(key);
}
std::unique_ptr<rt::AtomicOperation> ExperimentKv::atomic() { ++atomicCalls; return std::make_unique<ExperimentAtomic>(DiffKv::atomic(), *this); }
std::vector<std::string> targetKeyParts(const std::string& model) {
  return {"codex_chatgpt", "chatgpt_codex", "codex_account_pool", "responses_implicit_input_text_keyed_cycle_isolated_v5", model};
}
rt::KvKey evidenceKeyFor(const std::string& model) { return {PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX, "evidence", "codex_chatgpt", "chatgpt_codex", "codex_account_pool", "responses_implicit_input_text_keyed_cycle_isolated_v5", model}; }
rt::KvKey stateKeyFor(const std::string& model) { return {PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX, "state", "codex_chatgpt", "chatgpt_codex", "codex_account_pool", "responses_implicit_input_text_keyed_cycle_isolated_v5", model}; }
rt::KvKey leaseKey(const std::string& model) { return {PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX, "cycle_lease", "codex_chatgpt", "chatgpt_codex", "codex_account_pool", "responses_implicit_input_text_keyed_cycle_isolated_v5", model}; }
json::object makeAuth(const std::string& label) {
  return {{"access_token", "access-" + label}, {"refresh_token", "refresh-" + label}, {"account_id", "account-" + label}, {"updated_at_ms", rt::makeNumber(rt::dateNow())}};
}
rt::Task<std::string> authPoolVersionstamp() { return versionstamp(CODEX_AUTH_POOL_KV_KEY, "missing seeded Codex auth-pool versionstamp"); }
rt::Task<std::string> catalogVersionstamp() { return versionstamp(CODEX_MODELS_KV_KEY, "missing seeded Codex catalog versionstamp"); }
rt::Task<std::string> runtimeVersionstamp() { return versionstamp(RUNTIME_CONFIG_V2_KEY, "missing seeded runtime versionstamp"); }
rt::Task<PromotionBinding> promotionBinding() {
  rt::Promise<std::string> catalog(catalogVersionstamp()); const auto catalogStamp = co_await catalog.wait();
  rt::Promise<std::string> runtime(runtimeVersionstamp()); const auto runtimeStamp = co_await runtime.wait(); co_return PromotionBinding{catalogStamp, runtimeStamp};
}
const json::object& cacheControls() {
  static const json::object controls{{"key", true}, {"expected_usage_fields", json::array{"cached_tokens", "cache_write_tokens"}}, {"source", "catalog"}, {"verified_at_ms", 1000}};
  return controls;
}
json::object modelRecord(const std::string& model) { return modelRecord(model, cacheControls()); }
json::object modelRecord(const std::string& model, const json::object& controls) {
  return {{"slug", model}, {"supported_reasoning_levels", json::array{"none"}}, {"prompt_cache", json::object{{"version", 1}, {"providers", json::array{json::object{{"id", "codex_chatgpt"}, {"controls", controls}}}}}}};
}
void seed() { seed(SeedOptions{}); }
void seed(const SeedOptions& options) {
  kv->clear(); installHarness(); resetCodexAuthCacheForTest(); resetRuntimeConfigCacheForTest();
  const auto models = options.models.value_or(std::vector<std::string>{std::string(MODEL)});
  const auto defaultModel = options.defaultModel.value_or(std::string(MODEL));
  json::array records; for (const auto& model : models) records.push_back(modelRecord(model));
  const json::object snapshot{{"source", "chatgpt_codex"}, {"client_version", "0.201.0"}, {"updated_at_ms", rt::makeNumber(rt::dateNow())}, {"models", records}};
  const json::object pool{{"accounts", json::array{makeAuth("one"), makeAuth("two")}}, {"updated_at_ms", rt::makeNumber(rt::dateNow())}};
  kv->put(CODEX_AUTH_POOL_KV_KEY, rt::KvValue(pool)); kv->put(CODEX_MODELS_KV_KEY, rt::KvValue(snapshot));
  const json::object runtime{{"version", 2}, {"default_model", defaultModel}, {"default_reasoning_effort", "none"}, {"codex_models", snapshot}, {"updated_at_ms", rt::makeNumber(rt::dateNow())}};
  kv->put(RUNTIME_CONFIG_V2_KEY, rt::KvValue(runtime));
}
rt::Task<void> seedStage0Baseline(std::string model) {
  const cache::telemetry_gate::PromptCacheTelemetryBaselineTarget target{"chatgpt_codex", model};
  const cache::telemetry_gate::PromptCacheTelemetryGateOptions options{.release = std::string(TELEMETRY_RELEASE)};
  auto task = resolvePromptCacheTelemetryCounterKeys(target, options);
  rt::Promise<std::optional<cache::telemetry_gate::PromptCacheTelemetryCounterKeys>> pending(std::move(task)); const auto keys = co_await pending.wait();
  if (!keys) throw rt::JsError("missing Stage 0 counter keys");
  for (const auto& route : keys->routes) {
    kv->put(route.completed, rt::KvU64(5000)); kv->put(route.reported, rt::KvU64(5000)); kv->put(route.cache_write_reported, rt::KvU64(5000));
  }
}
rt::Task<cache::scope_experiment_model::PromptCacheScopeExperimentTelemetryBaseline> scopeBaselineFor(std::string model) {
  const cache::scope_targets::LoadPromptCacheScopeTargetInventoryOptions options{.kv = kv};
  auto task = loadPromptCacheScopeTargetInventory(options);
  rt::Promise<cache::scope_targets::PromptCacheScopeTargetInventory> pending(std::move(task)); const auto inventory = co_await pending.wait();
  if (inventory.status != "ready") throw rt::JsError("missing seeded target inventory");
  const auto target = std::find_if(inventory.targets.begin(), inventory.targets.end(), [&](const auto& row) { return row.provider == "codex_chatgpt" && row.model == model; });
  rt::Promise<std::string> runtime(runtimeVersionstamp()); const auto runtimeStamp = co_await runtime.wait();
  if (target == inventory.targets.end() || !std::holds_alternative<cache::scope_targets::PromptCacheScopeTargetProbeabilityProbeable>(target->probeability) ||
      !target->catalog_versionstamp || !target->codex_auth_pool_versionstamp || !target->codex_auth_pool_identity_fingerprint || !inventory.inventory_fingerprint)
    throw rt::JsError("missing seeded probeable target");
  co_return cache::scope_experiment_model::PromptCacheScopeExperimentTelemetryBaseline{{target->id, "codex_chatgpt", "chatgpt_codex", "codex_account_pool",
    model, "responses_implicit_input_text_keyed_cycle_isolated_v5", target->capability_fingerprint, *inventory.inventory_fingerprint,
    *target->catalog_versionstamp, runtimeStamp, *target->codex_auth_pool_versionstamp, *target->codex_auth_pool_identity_fingerprint, target->catalog_client_version}};
}
rt::Task<cache::scope_experiment_model::PromptCacheScopeExperimentResult> runExperiment(std::string model) {
  rt::Promise<cache::scope_experiment_model::PromptCacheScopeExperimentTelemetryBaseline> reading(scopeBaselineFor(model)); const auto baseline = co_await reading.wait();
  auto task = runPromptCacheScopeExperiment(baseline); rt::Promise<cache::scope_experiment_model::PromptCacheScopeExperimentResult> running(std::move(task));
  const auto result = co_await running.wait(); co_return result;
}
rt::Response sseCompleted(double cachedTokens, double cacheWriteTokens, const std::string& model) {
  const json::object usage{{"input_tokens", 3000}, {"output_tokens", 1}, {"total_tokens", 3001},
    {"input_tokens_details", json::object{{"cached_tokens", rt::makeNumber(cachedTokens)}, {"cache_write_tokens", rt::makeNumber(cacheWriteTokens)}}}};
  const json::object frame{{"type", "response.completed"}, {"response", json::object{{"model", model}, {"usage", usage}}}};
  const auto event = "data: " + rt::jsonStringify(frame) + "\n\n";
  const auto bytes = rt::TextEncoder().encode(event);
  rt::UnderlyingSource<rt::ByteChunk> source; source.start = [bytes](auto controller) { controller.enqueue(bytes); controller.close(); };
  const rt::ByteStream stream(source); const rt::Headers headers{{"Content-Type", "text/event-stream"}};
  return rt::Response(rt::BodyInit(stream), rt::ResponseInit{.status = 200, .headers = headers});
}
rt::Task<provider::health::ProviderHealthView> waitForCodexHealth(std::string accountId) {
  const auto deadline = rt::performanceNow() + 2000;
  for (;;) {
    rt::Promise<provider::health::ProviderHealthView> pending(getCodexProviderHealth(accountId)); const auto health = co_await pending.wait();
    if (health.state == "healthy") co_return health;
    if (rt::performanceNow() >= deadline) throw rt::JsError("Timed out waiting for " + accountId + " provider health");
    co_await rt::sleep(5);
  }
}
rt::Fetch ScopeTransport::fetcher() {
  return rt::Fetch([this](rt::RequestInfo input, rt::RequestInit init) -> rt::Response {
    if (fetchInputUrl(input) == "https://auth.openai.com/oauth/token") {
      ++refreshes;
      if (onRefresh) return onRefresh(refreshes);
      const json::object tokens{{"access_token", "access-refreshed-" + rt::numberToString(static_cast<double>(refreshes))},
        {"refresh_token", "refresh-refreshed-" + rt::numberToString(static_cast<double>(refreshes))}};
      return rt::Response::json(tokens);
    }
    const auto headers = init.headers.value_or(rt::Headers{});
    requests.push_back({headers.get("chatgpt-account-id"), headers.get("conversation_id"), requestBodyText(init)});
    const auto index = requests.size() - 1;
    if (onInference) return onInference(index, input, init);
    const auto body = rt::jsonParse(requestBodyText(init));
    const auto model = std::string(rt::getString(body, "model").value_or(MODEL));
    const auto step = static_cast<double>(index % 10);
    return sseCompleted(cycleValue(CACHE_READ_CYCLE, step), cycleValue(CACHE_WRITE_CYCLE_ALTERNATING, step), model);
  });
}
void installHarness() { ::uos::kv::setKvForTest(kv); }
void resetHarness() { kv->clear(); resetCodexAuthCacheForTest(); resetRuntimeConfigCacheForTest(); installHarness(); }
void cleanupHarness() { ::uos::kv::setKvForTest(nullptr); }
HarnessScope::HarnessScope() { config::config.codexAuthJsonB64.clear(); resetHarness(); resetProviderHealthThrottleForTest(); }
HarnessScope::~HarnessScope() { resetCodexAuthCacheForTest(); resetRuntimeConfigCacheForTest(); resetProviderHealthThrottleForTest(); cleanupHarness(); kv->clear(); config::config.codexAuthJsonB64 = saved; }
}
