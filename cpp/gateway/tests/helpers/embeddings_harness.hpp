#pragma once
// Port of tests/helpers/embeddings-harness.ts; synthetic, in-session fixtures only.
#include "uos/embeddings/handlers.hpp"
#include "uos/embeddings/jobs.hpp"
#include "uos/embeddings/ledger.hpp"
#include "uos/kv.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"
#include <functional>
#include <type_traits>

namespace uos::tests::helpers::embeddings_harness {
  using TestDimension = double;
  using TestEncodingFormat = std::string;
  using TestInputType = std::string;
  inline constexpr double EMBEDDINGS_JOB_TTL_MS = embeddings::ledger::EMBEDDINGS_JOB_TTL_MS;
  inline constexpr double EMBEDDINGS_IDEMPOTENCY_MAX_RESPONSE_CHUNKS = embeddings::ledger::EMBEDDINGS_IDEMPOTENCY_MAX_RESPONSE_CHUNKS;
  inline constexpr double EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS = embeddings::ledger::EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS;
  inline constexpr double EMBEDDINGS_IDEMPOTENCY_RESPONSE_TTL_MS = embeddings::ledger::EMBEDDINGS_IDEMPOTENCY_RESPONSE_TTL_MS;
  inline const rt::KvKey VOYAGE_RATE_LIMIT_KEY = embeddings::ledger::VOYAGE_RATE_LIMIT_KEY;
  extern rt::OrderedMap<std::string, rt::KvValue> kvStore;
  extern rt::OrderedMap<std::string, std::optional<double>> kvExpirations;
  extern rt::OrderedMap<std::string, double> kvVersions;
  std::string keyToString(const rt::KvKey& key);
  bool keyHasPrefix(const rt::KvKey& key, const rt::KvKey& prefix);
  double compareKeyPart(const rt::KvKeyPart& a, const rt::KvKeyPart& b);
  double compareKeys(const rt::KvKey& a, const rt::KvKey& b);
  void resetVoyageRateLimit();
  std::string embeddingsProfileKey(TestInputType inputType = "document", TestDimension dimensions = 1024,
                                   TestEncodingFormat encodingFormat = "float", bool truncation = true);
  rt::KvKey embeddingsCacheKey(const std::string& hash, TestInputType inputType = "document", TestDimension dimensions = 1024,
                               TestEncodingFormat encodingFormat = "float", bool truncation = true);
  rt::KvKey embeddingsCacheGlobalIndexKey(double createdAtMs, const std::string& cacheProfileKey, const std::string& hash);
  rt::KvKey embeddingsJobKey(const std::string& tokenHash, const std::string& cacheProfileKey, const std::string& id);
  rt::KvKey embeddingsJobLookupKey(const std::string& tokenHash, const std::string& id);
  std::vector<double> testVector(TestDimension dimensions, double seed = 0);
  using CommitFailure = std::variant<bool, rt::Reason>;
  using FailNextAtomicCommit = std::function<CommitFailure(const std::vector<rt::AtomicCheck>&, const std::vector<rt::KvMutation>&)>;
  void setFailNextAtomicCommit(FailNextAtomicCommit value);
  std::optional<std::string> kvVersionstamp(const std::string& rawKey);
  void bumpKvVersion(const std::string& rawKey);
  class EmbeddingsKvStub : public rt::Kv {
  public:
    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override;
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey> keys, rt::KvGetOptions options = {}) override;
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override;
    rt::Task<void> delete_(rt::KvKey key) override;
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override;
    std::unique_ptr<rt::AtomicOperation> atomic() override;
    void close() override {}
  };
  extern std::shared_ptr<EmbeddingsKvStub> kvStub;
  extern std::optional<std::string> originalVoyageApiKey;
  struct FetchMockQueue { std::optional<rt::Promise<void>> chain{}; };
  extern FetchMockQueue fetchMockQueue;
  std::string requestInputUrl(const rt::RequestInfo& input);
  using FetchMockHandler = std::function<rt::Task<rt::Response>(std::string, std::optional<std::string>, rt::Headers)>;
  rt::Fetch makeFetchMock(FetchMockHandler handler);
  template<class Handler> FetchMockHandler adaptFetchMock(Handler handler) {
    return [handler](std::string url, std::optional<std::string> body, rt::Headers headers) mutable -> rt::Task<rt::Response> {
      if constexpr (std::is_same_v<std::invoke_result_t<Handler&, std::string, std::optional<std::string>, rt::Headers>, rt::Response>) co_return handler(url, body, headers);
      else { auto task = handler(url, body, headers); auto response = co_await std::move(task); co_return response; }
    };
  }
  template<class Handler, class Fn> auto withFetchMock(Handler handler, Fn fn) -> rt::Task<typename std::invoke_result_t<Fn&>::value_type> {
    using T = typename std::invoke_result_t<Fn&>::value_type;
    auto previous = fetchMockQueue.chain.value_or(rt::Promise<void>::resolve());
    auto [pending, release, reject] = rt::withResolvers<void>();
    (void)reject;
    fetchMockQueue.chain = pending;
    co_await previous.wait();
    const auto cleanup = rt::scopeExit([release] { release(); });
    rt::GlobalFetchOverride scope(makeFetchMock(adaptFetchMock(std::move(handler))));
    rt::Promise<T> running(fn());
    if constexpr (std::is_void_v<T>) co_await running.wait();
    else { auto result = co_await running.wait(); co_return result; }
  }
  rt::Response voyageOkResponse(double count, TestDimension dimensions = 1024);
  openai_telemetry::UsageContext uosIdempotencyUsageContext(const std::string& principal);
  rt::Request uosIdempotentRequest(const std::string& key, const json::value& input, const json::object& overrides = {});
  rt::Task<std::optional<std::string>> responseErrorCode(rt::Response response);
  rt::Task<rt::KvKey> uosEmbeddingsIdempotencyRecordKey(std::string principal, std::string key);
  rt::Task<rt::KvKey> uosEmbeddingsIdempotencyResponsePrefix(std::string principal, std::string key);
  rt::Task<std::string> uosEmbeddingsIdempotencyFingerprint(std::vector<std::string> input, TestInputType inputType = "document",
                                                         TestDimension dimensions = 1024, bool truncation = false);
  rt::Task<void> waitForAdmission(std::function<bool()> predicate, std::string label, double timeoutMs = 2000);
  void installHarness();
  void resetHarness();
  void cleanupHarness();
  // Test scope additionally clears credential sources even when used outside the sweep.
  class HarnessScope {
  public:
    HarnessScope();
    ~HarnessScope();
  private:
    rt::env::ScopedEnvironment environment_;
    std::string previousCodexAuth_;
  };
  using embeddings::handlers::handleUosEmbeddings;
  using embeddings::jobs::handleEmbeddingsJobCreate;
  using embeddings::jobs::handleEmbeddingsJobGet;
  using utils::sha256Hex;
  using kv::getKv;
}
