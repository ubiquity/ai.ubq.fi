#include "tests/helpers/embeddings_harness.hpp"
#include "uos/config.hpp"
#include "uos/defaults.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include <algorithm>
#include <limits>
#include <utility>

namespace uos::tests::helpers::embeddings_harness {
  rt::OrderedMap<std::string, rt::KvValue> kvStore;
  rt::OrderedMap<std::string, std::optional<double>> kvExpirations;
  rt::OrderedMap<std::string, double> kvVersions;
  std::shared_ptr<EmbeddingsKvStub> kvStub = std::make_shared<EmbeddingsKvStub>();
  std::optional<std::string> originalVoyageApiKey;
  FetchMockQueue fetchMockQueue;
  namespace {
    FailNextAtomicCommit failNextAtomicCommit;
    std::optional<std::optional<std::string>> harnessEnvironment;
    bool initialized = false;
    template<class T> rt::Task<T> jsAwait(rt::Task<T> task) { return rt::Promise<T>(std::move(task)).wait(); }
    class List final : public rt::KvListIterator {
      std::vector<rt::KvEntry<>> entries_;
      std::size_t index_ = 0;
    public:
      explicit List(std::vector<rt::KvEntry<>> entries) : entries_(std::move(entries)) {}
      rt::Task<std::optional<rt::KvEntry<>>> next() override {
        std::optional<rt::KvEntry<>> value;
        if (index_ < entries_.size()) value = entries_[index_++];
        return rt::Promise<std::optional<rt::KvEntry<>>>::resolve(value).wait();
      }
      rt::Task<void> return_() override { index_ = entries_.size(); return rt::fulfilledTask(); }
      std::string cursor() const override { return ""; }
    };
    void initialize() {
      if (initialized) return;
      initialized = true;
      kvStore.set(keyToString(defaults::DEFAULT_REASONING_EFFORT_KEY), rt::KvValue("low"));
      const json::object account{{"access_token", "access"}, {"refresh_token", "refresh"}, {"account_id", "acct"}, {"updated_at_ms", rt::makeNumber(rt::dateNow())}};
      kvStore.set(keyToString(rt::KvKey{"ubq_ai", "codex_auth"}), rt::KvValue(json::object{{"accounts", json::array{account}}, {"updated_at_ms", rt::makeNumber(rt::dateNow())}}));
      const json::object model{{"slug", "gpt-5-fixture-default"}, {"display_name", "GPT-5 Fixture Default"}, {"default_reasoning_level", "medium"},
        {"supported_reasoning_levels", json::array{"low", "medium", "high", "xhigh"}}};
      kvStore.set(keyToString(rt::KvKey{"ubq_ai", "codex_models"}), rt::KvValue(json::object{{"source", "chatgpt_codex"}, {"client_version", "0.125.0"},
        {"updated_at_ms", rt::makeNumber(rt::dateNow())}, {"models", json::array{model}}}));
      kvStore.set(keyToString(embeddings::ledger::VOYAGE_API_KEY_KV_KEY), rt::KvValue("voyage_test_key"));
      originalVoyageApiKey = rt::env::get("VOYAGEAI_API_KEY");
    }
  }
  std::string keyToString(const rt::KvKey& key) { return rt::kvJsonStringify(rt::kvValueFromKey(key)).value(); }
  bool keyHasPrefix(const rt::KvKey& key, const rt::KvKey& prefix) {
    if (key.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) if (!rt::kvKeyPartStrictEquals(key[i], prefix[i])) return false;
    return true;
  }
  double compareKeyPart(const rt::KvKeyPart& a, const rt::KvKeyPart& b) {
    if (rt::kvKeyPartStrictEquals(a, b)) return 0;
    if (a.isNumber() && b.isNumber()) return a.number() - b.number();
    if (a.isString() && b.isString()) return rt::localeCompare(a.string(), b.string());
    return rt::localeCompare(rt::kvToString(rt::kvValueFromKeyPart(a)), rt::kvToString(rt::kvValueFromKeyPart(b)));
  }
  double compareKeys(const rt::KvKey& a, const rt::KvKey& b) {
    for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
      if (i >= a.size()) return -1;
      if (i >= b.size()) return 1;
      const double diff = compareKeyPart(a[i], b[i]);
      if (diff != 0) return diff;
    }
    return 0;
  }
  void resetVoyageRateLimit() { kvStore.delete_(keyToString(VOYAGE_RATE_LIMIT_KEY)); }
  std::string embeddingsProfileKey(TestInputType type, TestDimension dimensions, TestEncodingFormat encoding, bool truncation) {
    return rt::jsonStringify(json::array{"voyage-profile-v2", "voyage-4-large", type, rt::makeNumber(dimensions), "float", encoding, truncation});
  }
  rt::KvKey embeddingsCacheKey(const std::string& hash, TestInputType type, TestDimension dimensions, TestEncodingFormat encoding, bool truncation) {
    return embeddings::ledger::embeddingsCacheKey(embeddingsProfileKey(type, dimensions, encoding, truncation), hash);
  }
  rt::KvKey embeddingsCacheGlobalIndexKey(double time, const std::string& profile, const std::string& hash) { return embeddings::ledger::embeddingsCacheGlobalIndexKey(time, profile, hash); }
  rt::KvKey embeddingsJobKey(const std::string& token, const std::string& profile, const std::string& id) { return embeddings::ledger::embeddingsJobKey(token, profile, id); }
  rt::KvKey embeddingsJobLookupKey(const std::string& token, const std::string& id) { return embeddings::ledger::embeddingsJobLookupKey(token, id); }
  std::vector<double> testVector(TestDimension dimensions, double seed) {
    std::vector<double> values;
    for (double i = 0; i < dimensions; ++i) values.push_back(seed + i / rt::Math::max(1.0, dimensions));
    return values;
  }
  void setFailNextAtomicCommit(FailNextAtomicCommit value) { failNextAtomicCommit = std::move(value); }
  std::optional<std::string> kvVersionstamp(const std::string& key) {
    if (!kvStore.has(key)) return std::nullopt;
    const auto version = kvVersions.get(key);
    return rt::padStart(rt::numberToString(version ? *version : 1), 20, "0");
  }
  void bumpKvVersion(const std::string& key) { const auto version = kvVersions.get(key); kvVersions.set(key, (version ? *version : 0) + 1); }
  rt::Task<rt::KvEntryMaybe<>> EmbeddingsKvStub::get(rt::KvKey key, rt::KvGetOptions) {
    const auto raw = keyToString(key);
    const auto value = kvStore.get(raw);
    return rt::fulfilledTask(rt::KvEntryMaybe<>{key, value && !value->isNull() && !value->isUndefined() ? *value : rt::KvValue(nullptr), kvVersionstamp(raw)});
  }
  rt::Task<std::vector<rt::KvEntryMaybe<>>> EmbeddingsKvStub::getMany(std::vector<rt::KvKey>, rt::KvGetOptions) { throw rt::JsError("Embeddings harness getMany is unavailable"); }
  rt::Task<rt::KvCommitResult> EmbeddingsKvStub::set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options) {
    const auto raw = keyToString(key);
    kvStore.set(raw, value); kvExpirations.set(raw, options.expireIn); bumpKvVersion(raw);
    return rt::fulfilledTask(rt::KvCommitResult{.ok = true, .versionstamp = ""});
  }
  rt::Task<void> EmbeddingsKvStub::delete_(rt::KvKey key) {
    const auto raw = keyToString(key);
    kvStore.delete_(raw); kvExpirations.delete_(raw); bumpKvVersion(raw);
    return rt::fulfilledTask();
  }
  std::unique_ptr<rt::KvListIterator> EmbeddingsKvStub::list(rt::KvListSelector selector, rt::KvListOptions options) {
    std::vector<rt::KvEntry<>> entries;
    if (selector.prefix) for (const auto& [raw, value] : kvStore) {
      const auto parsed = rt::jsonParse(raw);
      if (!parsed.is_array()) continue;
      const auto key = rt::kvKeyFromValue(rt::KvValue(parsed));
      if (keyHasPrefix(key, *selector.prefix)) entries.push_back({key, value, ""});
    }
    rt::sort(entries, [](const auto& a, const auto& b) { return compareKeys(a.key, b.key); });
    const double limit = rt::Math::max(0.0, rt::Math::trunc(options.limit.value_or(std::numeric_limits<double>::infinity())));
    if (limit < static_cast<double>(entries.size())) entries.resize(static_cast<std::size_t>(limit));
    return std::make_unique<List>(std::move(entries));
  }
  std::unique_ptr<rt::AtomicOperation> EmbeddingsKvStub::atomic() {
    return std::make_unique<rt::GenericAtomicOperation>([](std::vector<rt::AtomicCheck> checks, std::vector<rt::KvMutation> ops) -> rt::Task<rt::KvCommitResult> {
      for (const auto& check : checks) if (kvVersionstamp(keyToString(check.key)) != check.versionstamp) return rt::fulfilledTask(rt::KvCommitResult{});
      if (failNextAtomicCommit) {
        const auto failure = failNextAtomicCommit(checks, ops);
        const bool hasFailure = std::holds_alternative<rt::Reason>(failure) || std::get<bool>(failure);
        if (hasFailure) {
          failNextAtomicCommit = nullptr;
          if (const auto reason = std::get_if<rt::Reason>(&failure)) std::rethrow_exception(*reason);
          return rt::fulfilledTask(rt::KvCommitResult{});
        }
      }
      for (const auto& op : ops) {
        const auto raw = keyToString(op.key);
        if (op.type == rt::KvMutationType::set) { kvStore.set(raw, op.value); kvExpirations.set(raw, op.expireIn); }
        else { kvStore.delete_(raw); kvExpirations.delete_(raw); }
        bumpKvVersion(raw);
      }
      return rt::fulfilledTask(rt::KvCommitResult{.ok = true, .versionstamp = ""});
    });
  }
  std::string requestInputUrl(const rt::RequestInfo& input) {
    if (const auto text = std::get_if<std::string>(&input)) return *text;
    if (const auto url = std::get_if<rt::URL>(&input)) return url->toString();
    return std::get<rt::Request>(input).url();
  }
  rt::Fetch makeFetchMock(FetchMockHandler handler) {
    return rt::Fetch([handler](rt::RequestInfo input, rt::RequestInit init) {
      std::optional<std::string> text;
      if (init.body) if (const auto value = std::get_if<std::string>(&*init.body)) text = *value;
      return handler(requestInputUrl(input), text, init.headers.value_or(rt::Headers()));
    });
  }
  rt::Response voyageOkResponse(double count, TestDimension dimensions) {
    json::array data;
    for (double i = 0; i < count; ++i) data.push_back(json::object{{"embedding", json::value_from(testVector(dimensions, i + 0.1))}});
    const json::object payload{{"data", data}, {"usage", json::object{{"total_tokens", rt::makeNumber(count * 5)}}}};
    const rt::ResponseInit options{.status = 200, .headers = rt::Headers{{"Content-Type", "application/json"}}};
    return rt::Response(rt::jsonStringify(payload), options);
  }
  openai_telemetry::UsageContext uosIdempotencyUsageContext(const std::string& principal) {
    return {.keyId = std::nullopt, .kernelRepo = std::nullopt, .kernelOrg = std::nullopt, .idempotencyPrincipal = principal};
  }
  rt::Request uosIdempotentRequest(const std::string& key, const json::value& input, const json::object& overrides) {
    const auto type = rt::getString(overrides, "input_type").value_or("document");
    const auto dimensions = rt::getNumber(overrides, "dimensions").value_or(1024);
    const auto truncation = rt::getBool(overrides, "truncation").value_or(false);
    const json::object body{{"model", "voyage-4-large"}, {"input", input}, {"input_type", type}, {"dimensions", rt::makeNumber(dimensions)}, {"truncation", truncation}};
    return rt::Request("https://ai.ubq.fi/uos/embeddings", rt::RequestInit{.method = "POST",
      .headers = rt::Headers{{"Content-Type", "application/json"}, {"Idempotency-Key", key}}, .body = rt::jsonStringify(body)});
  }
  rt::Task<std::optional<std::string>> responseErrorCode(rt::Response response) {
    const auto value = co_await jsAwait(response.json());
    const auto error = rt::get(value, "error");
    const auto code_ = error ? utils::getString(rt::get(*error, "code")) : std::nullopt;
    co_return code_;
  }
  rt::Task<rt::KvKey> uosEmbeddingsIdempotencyRecordKey(std::string principal, std::string key) {
    const auto principalHash = co_await jsAwait(utils::sha256Hex("uos-embeddings-principal-v1:" + principal));
    const auto keyHash = co_await jsAwait(utils::sha256Hex("uos-embeddings-key-v1:" + key));
    co_return rt::KvKey{"embeddings", "idempotency", "v1", principalHash, keyHash};
  }
  rt::Task<rt::KvKey> uosEmbeddingsIdempotencyResponsePrefix(std::string principal, std::string key) {
    const auto principalHash = co_await jsAwait(utils::sha256Hex("uos-embeddings-principal-v1:" + principal));
    const auto keyHash = co_await jsAwait(utils::sha256Hex("uos-embeddings-key-v1:" + key));
    co_return rt::KvKey{"embeddings", "idempotency", "v1", "response", principalHash, keyHash};
  }
  rt::Task<std::string> uosEmbeddingsIdempotencyFingerprint(std::vector<std::string> input, TestInputType type, TestDimension dimensions, bool truncation) {
    std::vector<rt::Promise<std::string>> pending;
    for (const auto& text : input) pending.emplace_back(utils::sha256Hex(text));
    const auto hashes = co_await rt::all(std::move(pending)).wait();
    const json::array value{"uos-embeddings-idempotency-v1", "voyage", "voyage-4-large", type, rt::makeNumber(dimensions), "float", "float", truncation, json::value_from(hashes)};
    auto result = co_await jsAwait(utils::sha256Hex(rt::jsonStringify(value)));
    co_return result;
  }
  rt::Task<void> waitForAdmission(std::function<bool()> predicate, std::string label, double timeoutMs) {
    const double deadline = rt::performanceNow() + timeoutMs;
    while (!predicate()) {
      if (rt::performanceNow() >= deadline) throw rt::JsError("Timed out waiting for " + label);
      co_await rt::sleep(5);
    }
  }
  void installHarness() {
    initialize();
    if (!harnessEnvironment) harnessEnvironment.emplace(rt::env::get("VOYAGEAI_API_KEY"));
    rt::env::remove("VOYAGEAI_API_KEY");
    kv::setKvForTest(kvStub);
  }
  void resetHarness() { setFailNextAtomicCommit(nullptr); resetVoyageRateLimit(); installHarness(); }
  void cleanupHarness() {
    if (harnessEnvironment && *harnessEnvironment) rt::env::set("VOYAGEAI_API_KEY", **harnessEnvironment);
    else rt::env::remove("VOYAGEAI_API_KEY");
    harnessEnvironment = std::nullopt;
    kv::setKvForTest(nullptr);
    fetchMockQueue.chain.reset();
  }
  HarnessScope::HarnessScope() : environment_(std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w09c2"}}), previousCodexAuth_(config::config.codexAuthJsonB64) {
    config::config.codexAuthJsonB64.clear();
    installHarness();
  }
  HarnessScope::~HarnessScope() { cleanupHarness(); config::config.codexAuthJsonB64 = previousCodexAuth_; }
}
