#pragma once
// Port of tests/helpers/openai-compat-harness.ts.
#include "uos/openai_telemetry.hpp"
#include "uos/handler/index.hpp"
#include "uos/codex/banked_reset.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include <functional>
#include <type_traits>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace uos::tests::helpers::openai_compat_harness {
  inline auto gatewayHandler(rt::Request request, std::optional<handler::http::RequestDeliveryInfo> delivery = std::nullopt) { return handler::index::handler(std::move(request), std::move(delivery)); }
  inline constexpr std::string_view DEFAULT_TEST_MODEL = "gpt-5-fixture-default";
  inline constexpr std::string_view TERRA_TEST_MODEL = "gpt-5.6-terra";
  inline constexpr std::string_view TEMPORARY_FREE_SURPLUS_TEST_MODEL = "glm-5.2";
  inline const rt::KvKey TEST_CODEX_MODELS_KEY{"ubq_ai", "codex_models"};
  std::string keyToString(const rt::KvKey& key);
  extern rt::OrderedMap<std::string, rt::KvValue> kvStore;
  struct OpenAiAtomicOp { std::string type; rt::KvKey key; std::optional<rt::KvValue> value{}; };
  extern std::function<rt::Reason(const std::vector<OpenAiAtomicOp>&)> atomicCommitFailure;
  struct AtomicCommitObservation { std::function<void(const std::vector<OpenAiAtomicOp>&)> observer{}; };
  extern AtomicCommitObservation atomicCommitObservation;
  extern bool exposePaidFallbackLedgerEntries;
  void setAtomicCommitFailure(std::function<rt::Reason(const std::vector<OpenAiAtomicOp>&)> value);
  void setExposePaidFallbackLedgerEntries(bool value);
  class OpenAiKvStub final : public rt::Kv {
  public:
    using rt::Kv::get; using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override;
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey> keys, rt::KvGetOptions options = {}) override;
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override;
    rt::Task<void> delete_(rt::KvKey key) override;
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override;
    std::unique_ptr<rt::AtomicOperation> atomic() override;
    void close() override;
  };
  extern std::shared_ptr<OpenAiKvStub> kvStub;
  template<class T> struct Deferred {
    rt::Promise<T> promise; rt::Resolve<T> resolve;
    Deferred() : Deferred(rt::withResolvers<T>()) {}
  private:
    explicit Deferred(rt::Resolvers<T> parts) : promise(parts.promise),resolve(parts.resolve) {}
  };
  using VoidGate = Deferred<void>;
  rt::Promise<void> neverSettlingPromise();
  rt::Task<void> delayBy(double milliseconds);
  rt::Reason abortReason(const rt::AbortSignal& signal);
  rt::Promise<rt::Response> rejectOnAbort(rt::AbortSignal signal, std::function<void()> onAbort = {});
  double utf8ByteLength(const std::string& value);
  std::string encodeBase64(const rt::Bytes& bytes);
  std::string encodeBase64Url(const rt::Bytes& bytes);
  std::string encodeJsonBase64Url(const json::value& value);
  std::string toPublicKeyPem(const rt::Bytes& bytes);
  rt::Response sseResponse(const std::vector<std::string>& chunks);
  std::vector<std::string> baseSseChunks();
  rt::Response authoritativeCodexQuotaResponse(rt::Headers headers = {});
  rt::Request responsesRequest(json::object body = {}, std::optional<rt::AbortSignal> signal = std::nullopt);
  rt::Request deepSeekResponsesRequest(json::object body);
  rt::Request specialProviderChatRequest(json::object body, std::optional<rt::AbortSignal> signal = std::nullopt);
  rt::Request specialProviderResponsesRequest(json::object body, std::optional<rt::AbortSignal> signal = std::nullopt);
  std::vector<json::object> parseResponsesSseValues(const std::string& value);
  std::vector<json::object> parseResponsesSseEvents(const std::string& value);
  std::string deepSeekStreamChunk(const json::object& delta, const json::object& extra = {}, const std::string& id = "deepseek-responses-stream");
  std::vector<std::string> parseWarnings(const std::optional<std::string>& value);
  std::string extractResponseOutputText(const json::object& payload);
  std::vector<OpenAiAtomicOp> atomicWritesForKey(const std::vector<std::vector<OpenAiAtomicOp>>& commits, const rt::KvKey& key);
  void clearProviderHealthKeysFor(const std::string& accountId);
  void clearMeteredAndSurplusProviderHealth();
  struct SeedPaidFallbackKeyOptions {
    bool enabled = true; double limitMicrocredits = 1'000'000; double spentMicrocredits = 0; double reservedMicrocredits = 0;
    std::optional<std::string> reservationRequestId{}; std::optional<std::vector<std::string>> modelIds{}; std::optional<double> v3SettledMicrocredits{};
  };
  void seedPaidFallbackKey(const std::string& id, const SeedPaidFallbackKeyOptions& options = {});
  std::optional<json::object> getStoredPaidFallbackRequest(const std::string& keyId, const std::string& requestId);
  rt::Task<json::object> waitForPaidFallbackTerminal(std::string keyId, std::string requestId, std::string expected);
  rt::Task<void> seedPaidFallthroughProviders();
  void prepareFetchMock();
  using FetchHandler = std::function<rt::Task<rt::Response>(std::string, std::optional<std::string>, rt::RequestInit)>;
  rt::Fetch createFetchMock(FetchHandler handler);
  struct FetchMockQueue { std::optional<rt::Promise<void>> chain{}; };
  extern FetchMockQueue fetchMockQueue;
  struct FetchMockLease { rt::Promise<void> previous;rt::Resolve<void> release; };
  FetchMockLease acquireFetchMock();
  template<class T> rt::Task<T> withFetchMock(FetchHandler handler, std::function<rt::Task<T>()> run) {
    const auto lease=acquireFetchMock();co_await lease.previous.wait();const auto cleanup=rt::scopeExit(lease.release);
    prepareFetchMock(); rt::GlobalFetchOverride scope(createFetchMock(std::move(handler))); auto task = run(); rt::Promise<T> pending(std::move(task));
    if constexpr (std::is_void_v<T>) { co_await pending.wait(); co_return; } else { auto result = co_await pending.wait(); co_return result; }
  }
  struct ProviderSelectionScope {
    std::string encoded; std::optional<rt::KvValue> previous;
    explicit ProviderSelectionScope(std::optional<std::vector<std::string>> providerIds); ~ProviderSelectionScope();
  };
  rt::Promise<void> captureResolve(VoidGate& gate);
  void clearBankedResetRecords();
  codex::banked_reset::CodexBankedResetConfig liveBankedResetFixtureConfig();
  rt::Task<std::vector<std::string>> createVerifiedBankedResetFixture();
  rt::Task<std::vector<std::string>> createUnknownBankedResetFixture();
  rt::Task<void> seedPaidReasoningProvider(std::string provider);
  struct ReasoningProgressState { bool stopped=false;bool semanticEmitted=false;int upstreamCancellations=0; };
  struct ReasoningProgressIds { std::string provider;std::string route;std::string delivery;std::string requestId; };
  rt::Response reasoningProgressUpstreamResponse(std::shared_ptr<ReasoningProgressState> state, ReasoningProgressIds ids, VoidGate reasoningObserved, VoidGate semanticGate);
  struct ValidatedTerminalCancellationCase { std::string provider;std::string route;std::string terminalType; };
  rt::Task<void> runValidatedTerminalCancellationCase(ValidatedTerminalCancellationCase testCase);
  template<class T> rt::Task<T> withProviderSelection(std::optional<std::vector<std::string>> ids,std::function<rt::Task<T>()> run){ProviderSelectionScope scope(std::move(ids));auto task=run();rt::Promise<T> pending(std::move(task));if constexpr(std::is_void_v<T>){co_await pending.wait();co_return;}else{auto result=co_await pending.wait();co_return result;}}
  template<class T> rt::Task<T> withDiscoveryKeys(std::optional<std::string> key,std::function<rt::Task<T>()> run){const std::vector<std::pair<std::string,std::optional<std::string>>> values{{"METERED_API_KEY",key},{"SURPLUS_API_KEY",std::nullopt}};rt::env::ScopedOverride scope(values);auto task=run();rt::Promise<T> pending(std::move(task));if constexpr(std::is_void_v<T>){co_await pending.wait();co_return;}else{auto result=co_await pending.wait();co_return result;}}
  void seedHarness();
  void installHarness(); void resetHarness(); void cleanupHarness(); rt::Task<void> settleHarness();
  struct Fixture {
    std::optional<std::string> scratch = rt::env::get("TMPDIR");
    rt::env::ScopedEnvironment clean;
    rt::console::Capture logs{{rt::console::Method::info, rt::console::Method::warn, rt::console::Method::error}};
    Fixture(); ~Fixture();
  };
}
