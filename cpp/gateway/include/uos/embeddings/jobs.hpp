#pragma once
// Port of src/embeddings/jobs.ts.
#include "uos/embeddings/ledger.hpp"
#include "uos/openai_telemetry.hpp"
namespace uos::embeddings::jobs {
  struct RunEmbeddingsJobAttemptParams {
    std::string reqId;
    std::shared_ptr<rt::Kv> kv;
    std::string apiKey;
    std::string tokenSeed;
    std::string tokenHash;
    rt::KvKey jobKey;
    rt::KvKey jobLookupKey;
    rt::KvEntryMaybe<> jobEntry;
    ledger::EmbeddingsJobRecord job;
    double deadlineMs;
    const openai_telemetry::UsageContext* usageContext = nullptr;
    // Backing row for source spreads; opaque stored fields survive rewrites.
    std::optional<rt::KvValue> jobRaw{};
  };
  rt::Task<rt::Response> runEmbeddingsJobAttempt(RunEmbeddingsJobAttemptParams params);
  rt::Task<rt::Response> handleEmbeddingsJobCreate(rt::Request req, std::optional<std::string> authToken,
                                                const openai_telemetry::UsageContext* usageContext = nullptr);
  rt::Task<rt::Response> handleEmbeddingsJobGet(rt::Request req, std::optional<std::string> authToken, std::string jobId,
                                             const openai_telemetry::UsageContext* usageContext = nullptr);
}
