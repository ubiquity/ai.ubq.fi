#pragma once
// Port of src/embeddings/voyage.ts.
#include "uos/embeddings/ledger.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/json.hpp"
#include "uos/upstream_wire.hpp"
#include <memory>
#include <utility>

namespace uos::embeddings::voyage {
  struct EmbeddingsCacheWriteResult { bool isNew; };
  rt::Task<EmbeddingsCacheWriteResult> writeEmbeddingsCacheEntryBestEffort(std::shared_ptr<rt::Kv> kv, std::string cacheProfileKey, std::string hash,
                                                                       std::vector<double> embedding, double createdAtMs, double deadlineMs);
  std::string resolveEmbeddingsJobTokenSeed(const std::string& jobId, const std::optional<std::string>& authToken,
                                           const openai_telemetry::UsageContext* usageContext = nullptr);
  std::optional<std::string> getEnv(const std::string& key);
  rt::Promise<void> sleep(double ms);
  rt::Promise<bool> sleepUnlessAborted(double ms, rt::AbortSignal signal);
  double estimateTokens(const std::string& text);
  double estimateTokenCount(const std::vector<std::string>& texts);
  struct EmbeddingsChunkItem {
    std::string hash;
    std::string text;
    friend bool operator==(const EmbeddingsChunkItem&, const EmbeddingsChunkItem&) = default;
  };
  std::vector<std::vector<EmbeddingsChunkItem>> chunkByTokenBudget(const std::vector<EmbeddingsChunkItem>& items, double maxItems, double maxTokens);
  struct VoyageBudgetResult {
    bool ok;
    std::optional<double> wait_ms{};
  };
  rt::Task<VoyageBudgetResult> tryReserveVoyageBudget(rt::Kv& kv, double tokens);
  rt::Task<VoyageBudgetResult> applyVoyageRateLimit(rt::Kv& kv, double tokens, double deadlineMs);
  ledger::EmbeddingsParseResult parseUosEmbeddingsRequest(const json::object& rawBody);
  ledger::EmbeddingsParseResult parseEmbeddingsJobRequest(const json::object& rawBody);
  bool isValidEmbeddingVector(const json::value* value, ledger::VoyageEmbeddingsDimension dimensions);
  bool isValidEmbeddingVector(const json::value& value, ledger::VoyageEmbeddingsDimension dimensions);
  bool isValidEmbeddingVector(const rt::KvValue& value, ledger::VoyageEmbeddingsDimension dimensions);
  std::string floatEmbeddingToBase64(const std::vector<double>& embedding);
  std::optional<ledger::EmbeddingsJobInputRecord> normalizeEmbeddingsJobInputRecord(const json::value* value);
  std::optional<ledger::EmbeddingsJobInputRecord> normalizeEmbeddingsJobInputRecord(const json::value& value);
  std::optional<ledger::EmbeddingsJobInputRecord> normalizeEmbeddingsJobInputRecord(const rt::KvValue& value);
  rt::Task<ledger::EmbeddingsJobInputRecord> encryptEmbeddingsJobInput(std::string tokenSeed, std::string text);
  rt::Task<std::optional<std::string>> decryptEmbeddingsJobInput(std::string tokenSeed, ledger::EmbeddingsJobInputRecord record);
  rt::Task<std::optional<std::string>> readVoyageApiKey(std::shared_ptr<rt::Kv> kv);
  struct FetchVoyageEmbeddingsParams {
    std::string apiKey;
    std::string model;
    std::vector<std::string> inputs;
    ledger::VoyageEmbeddingsInputType inputType;
    ledger::VoyageEmbeddingsDimension dimensions;
    ledger::VoyageEmbeddingsOutputDtype outputDtype;
    bool truncation;
    double deadlineMs;
    std::optional<rt::AbortSignal> downstreamSignal{};
    api_key_policy::ApiKeyUsageReservation::BeforeProviderDispatch beforeProviderDispatch{};
  };
  struct VoyageEmbeddingsResult {
    std::vector<std::vector<double>> vectors;
    std::optional<double> totalTokens;
  };
  // Own retry metadata on the shared carrier for JavaScript Error.status.
  struct VoyageEmbeddingsError : upstream_wire::ErrorWithStatus {
    std::optional<double> retry_after_ms;
    VoyageEmbeddingsError(std::string message_, double status_, std::optional<double> retryAfter)
      : ErrorWithStatus(std::move(message_), rt::makeNumber(status_)), retry_after_ms(retryAfter) {}
  };
  rt::Task<VoyageEmbeddingsResult> fetchVoyageEmbeddings(FetchVoyageEmbeddingsParams params);
}
