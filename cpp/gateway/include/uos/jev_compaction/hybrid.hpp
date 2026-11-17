#pragma once
// Port of src/jev_compaction/hybrid.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <vector>

namespace uos::jev_compaction::hybrid {
  inline constexpr double HYBRID_RESIDUE_TIMEOUT_MS = 20'000;
  inline constexpr double HYBRID_TAIL_FRACTION = 0.12;
  inline constexpr double HYBRID_LEDGER_LIMIT = 300;
  inline constexpr double HYBRID_INPUT_CHUNK_CHARS = 150'000;
  inline constexpr double HYBRID_CHUNK_CONCURRENCY = 4;

  struct LedgerEntry {
    std::string token;
    double lastIndex;
    double count;
    friend bool operator==(const LedgerEntry&, const LedgerEntry&) = default;
  };
  std::vector<LedgerEntry> harvestLedger(const std::string& text, double max = HYBRID_LEDGER_LIMIT);
  std::string composeHybridSummary(const std::string& modelText, const std::string& pureSummary);
  std::vector<std::string> chunkMemoryForResidue(const std::string& memory, double maxChars);
  std::optional<std::string> residueText(const json::value* payload);
  std::optional<std::string> residueText(const json::value& payload);

  struct HybridResidueDeps {
    rt::Fetch fetcher{};
    rt::Nullable<std::string> apiKey{};
    std::optional<double> timeoutMs{};
    std::optional<double> chunkChars{};
  };
  rt::Task<std::optional<std::string>> runHybridResiduePass(std::string summary);
  rt::Task<std::optional<std::string>> runHybridResiduePass(std::string summary, HybridResidueDeps deps);
}
