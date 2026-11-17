#pragma once
// Port of src/jev_compaction/compaction.ts.
#include "uos/lib/jev_compaction/types.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/ordered.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace uos::jev_compaction::compaction {
  inline const lib::jev_compaction::types::CompactOptions COMPACTION_OPTIONS{
    .goal = std::nullopt, .keepThreshold = 0.5, .preserveRecentMessages = 6, .maxStateTokens = 25'000,
    .maxRequestTokens = 30'000, .truncateHeadChars = 300};
  inline constexpr double JEV_TIMEOUT_MS = 30'000;
  inline constexpr double SUMMARY_CHAR_CAP = 1'500'000;
  inline constexpr double SUMMARY_MIN_REDUCTION_RATIO = 0.15;
  double maxFloorSummaryTokens(double inputTokens);
  using FailureKind = std::string;
  class CompactionUnavailable : public rt::JsError {
  public:
    FailureKind kind;
    CompactionUnavailable(FailureKind kind_, std::string detail) : rt::JsError(std::move(detail), "CompactionUnavailable"), kind(std::move(kind_)) {}
  };
  using CompactionResidue = std::function<rt::Task<std::optional<std::string>>(std::string summary)>;
  struct CompactionOutcome {
    double status;
    std::string contentType;
    rt::OrderedMap<std::string, std::string> headers;
    std::string body;
  };
  bool isJevCompactionRequest(const rt::Request& req);
  struct BuildCompactionResponseOptions { bool stream; CompactionResidue residue{}; };
  rt::Task<CompactionOutcome> buildCompactionResponse(const json::value& body, std::shared_ptr<lib::jev_compaction::types::JevAsker> asker,
                                                    const BuildCompactionResponseOptions& options);
  using JevFetch = rt::Fetch;
  JevFetch createJevTimeoutFetch(JevFetch baseFetch, double timeoutMs = JEV_TIMEOUT_MS);
  struct JevCompactionDeps {
    std::optional<rt::AbortSignal> signal{};
    std::shared_ptr<lib::jev_compaction::types::JevAsker> asker{};
    JevFetch fetch{};
    rt::Nullable<std::string> apiKey{};
    CompactionResidue residue{};
  };
  void setJevCompactionAskerForTest(std::shared_ptr<lib::jev_compaction::types::JevAsker> asker);
  rt::Task<rt::Response> handleJevResponsesCompaction(rt::Request req);
  rt::Task<rt::Response> handleJevResponsesCompaction(rt::Request req, const JevCompactionDeps& deps);
}
