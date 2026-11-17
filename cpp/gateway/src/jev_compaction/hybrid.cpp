#include "uos/jev_compaction/hybrid.hpp"

#include "uos/provider/cerebras.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <memory>
#include <utility>

namespace uos::jev_compaction::hybrid {
  namespace {
    constexpr std::string_view HYBRID_PROMPT = R"prompt(You are compacting a coding-agent session memory so another model can continue the session.

Input: a machine-generated memory in sections:
- [user] / [assistant] / [developer] text blocks kept verbatim from the transcript;
- [tool call <id>] provenance lines with their input, labelled keep / drop_result / drop_call;
- [tool result <id>] blocks: kept verbatim or a bounded head ending in a truncation note.

Produce one compact memory that a continuation model can rely on, following these rules exactly:
1. Preserve every identifier verbatim and exactly as written: hashes, revisions, paths, thread ids, service names, versions, commands, exit codes, file names.
2. Resolve time order. When the same fact appears more than once, keep only the latest value. If an earlier blocker was later resolved, keep only the resolution (a historical note is allowed only when it explains a current decision).
3. Merge documents that repeat (the same instructions or memory pasted multiple times) into a single copy.
4. Keep: the goal and its current status, decisions and approvals with their times, ownership, exact next steps, and evidence pointers (paths and hashes).
5. Drop: superseded text, process chatter, duplicated documents, and bulk from truncated tool results.
6. Never invent facts, values, or identifiers that are not present in the input. When the memory shows a
   state change (an approval, acknowledgement, consumption or merge), keep the LATEST state exactly; never
   downgrade a state to its earlier form.
7. Output plain markdown between 20000 and 35000 characters. If you cannot fit everything, drop narrative
   prose first; never drop identifiers, current states, decisions, or next steps.

Memory to compact:
)prompt";
    const std::array<rt::Regex, 4>& identifierPatterns() {
      static const std::array<rt::Regex, 4> patterns{
        rt::Regex(R"(\b\d{4}-\d{2}-\d{2}(?:[T ]\d{2}:\d{2}(?::\d{2})?)?\b)", "g"),
        rt::Regex(R"(\b[0-9a-f]{8,64}\b)", "g"),
        rt::Regex(R"([0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12})", "g"),
        rt::Regex(R"(\b[\w-]+\.(?:json|md|ts|sqlite3|sh|yml|yaml|service)\b)", "g")};
      return patterns;
    }
    constexpr std::string_view LEADING_TRIM = "`\"'()[]{},;:";
    constexpr std::string_view TRAILING_TRIM = "`\"'()[]{},;:.";
    std::string trimToken(const std::string& raw) {
      std::int64_t start = 0; const auto size = rt::length(raw);
      while (start < size && rt::includes(LEADING_TRIM, rt::charAt(raw, start))) ++start;
      auto end = size;
      while (end > start && rt::includes(TRAILING_TRIM, rt::charAt(raw, end - 1))) --end;
      return rt::slice(raw, start, end);
    }
    struct Occurrence { std::string token; double index; };
    std::vector<Occurrence> pathOccurrences(const std::string& text) {
      std::vector<Occurrence> out;
      static const rt::Regex PATH_CHAR("[A-Za-z0-9_.@-]");
      std::int64_t cursor = 0; const auto size = static_cast<std::int64_t>(rt::length(text));
      while (cursor < size) {
        const auto start = rt::indexOf(text, "/", cursor); if (start == -1) break;
        auto end = start + 1;
        while (end < size && PATH_CHAR.test(rt::charAt(text, end))) ++end;
        auto candidate = rt::slice(text, start, end);
        if (rt::length(candidate) >= 8 && rt::includes(rt::slice(candidate, 1), "/")) out.push_back({candidate, static_cast<double>(start)});
        cursor = std::max(end, start + 1);
      }
      return out;
    }
    std::vector<Occurrence> slugOccurrences(const std::string& text) {
      std::vector<Occurrence> out;
      static const rt::Regex SPACE(R"(\s+)"); static const rt::Regex HEX("[0-9a-f]{8,}"); static const rt::Regex LETTER("[a-z-]");
      for (const auto& raw : rt::split(text, SPACE)) {
        const auto token = trimToken(raw);
        if (rt::length(token) >= 8 && HEX.test(token) && LETTER.test(token)) out.push_back({token, static_cast<double>(rt::lastIndexOf(text, token))});
      }
      return out;
    }
    std::string renderLedgerSection(const std::vector<LedgerEntry>& entries, double memoryChars) {
      std::vector<std::string> lines;
      for (const auto& entry : entries) if (entry.lastIndex >= memoryChars * 0.6) {
        lines.push_back("- " + entry.token + (entry.count > 1 ? " (seen " + rt::numberToString(entry.count) + "x)" : ""));
      }
      return lines.empty() ? "" : "\n\n## Verbatim identifiers (last seen in the final 40% of the rewritten memory)\n" + rt::join(lines, "\n") + "\n";
    }
    std::string renderTailSection(const std::string& summary) {
      const double size = static_cast<double>(rt::length(summary)); const double tailChars = rt::Math::floor(size * HYBRID_TAIL_FRACTION);
      return tailChars <= 0 ? "" : "\n\n## Verbatim latest transcript tail (" + rt::numberToString(tailChars) + " chars)\n" + rt::slice(summary, size - tailChars);
    }
    json::object residueBody(const std::string& memory) {
      const auto required = harvestLedger(memory); std::vector<std::string> lines;
      for (const auto& entry : required) lines.push_back("- " + entry.token);
      const auto block = required.empty() ? "" : "\n\nREQUIRED IDENTIFIERS - every one of these must appear verbatim in your output; do not drop, rename, or summarize them away:\n" + rt::join(lines, "\n");
      return {{"model", std::string(provider::cerebras::CEREBRAS_GPT_OSS_120B_MODEL)}, {"stream", false}, {"reasoning_effort", "medium"},
              {"max_completion_tokens", 16'000}, {"messages", json::array{json::object{{"role", "user"}, {"content", std::string(HYBRID_PROMPT) + memory + block}}}}};
    }
    void hybridFailureLog(const std::string& detail) {
      rt::console::warn("[ai.ubq.fi] jev_compaction_hybrid " + rt::jsonStringify(json::object{{"outcome", "failed"}, {"detail", rt::slice(detail, 0, 120)}}));
    }
    struct ResidueWork {
      std::vector<std::string> chunks;
      std::vector<std::string> texts;
      std::size_t failed = 0;
      std::size_t cursor = 0;
      std::string apiKey;
      double timeoutMs;
      rt::Fetch fetcher;
    };
    rt::Task<void> residueWorker(std::shared_ptr<ResidueWork> work) {
      for (;;) {
        const auto index = work->cursor++; if (index >= work->chunks.size()) co_return;
        const auto chunk = work->chunks[index];
        try {
          provider::cerebras::CerebrasChatCompletionsOptions options;
          options.apiKey = work->apiKey; options.signal = rt::AbortSignal::timeout(work->timeoutMs); options.fetcher = work->fetcher;
          auto task = provider::cerebras::fetchCerebrasChatCompletions(residueBody(chunk), options);
          rt::Promise<rt::Response> pending(std::move(task)); auto upstream = co_await pending.wait();
          if (!upstream.ok()) throw rt::JsError("upstream-" + rt::numberToString(upstream.status()));
          rt::Promise<json::value> parsed(upstream.json()); const auto payload = co_await parsed.wait();
          const auto text = residueText(payload); if (!text) throw rt::JsError("empty-output");
          work->texts[index] = *text;
        } catch (...) {
          ++work->failed; work->texts[index] = chunk;
          const auto failure = std::current_exception();
          hybridFailureLog("chunk-" + std::string(rt::isError(failure) && rt::errorName(failure) == "TimeoutError" ? "timeout" : "failed") + "-" + rt::numberToString(static_cast<double>(index)));
        }
      }
    }
  }
  std::vector<LedgerEntry> harvestLedger(const std::string& text, double max) {
    rt::OrderedMap<std::string, LedgerEntry> byToken;
    const auto add = [&byToken](const std::string& token, double index) {
      if (rt::length(token) < 8) return;
      if (auto* existing = byToken.get(token)) { ++existing->count; existing->lastIndex = std::max(existing->lastIndex, index); }
      else byToken.set(token, LedgerEntry{token, index, 1});
    };
    for (const auto& pattern : identifierPatterns()) for (const auto& match : rt::matchAll(text, pattern)) add(match.str(), static_cast<double>(match.index));
    for (const auto& occurrence : pathOccurrences(text)) add(occurrence.token, occurrence.index);
    for (const auto& occurrence : slugOccurrences(text)) add(occurrence.token, occurrence.index);
    std::vector<LedgerEntry> latestFirst; for (const auto& [token, entry] : byToken) { (void)token; latestFirst.push_back(entry); }
    rt::sort(latestFirst, [](const LedgerEntry& a, const LedgerEntry& b) { return b.lastIndex - a.lastIndex; });
    double end = rt::Math::trunc(max); if (rt::Number::isNaN(end)) end = 0;
    const double size = static_cast<double>(latestFirst.size()); end = end < 0 ? std::max(size + end, 0.0) : std::min(end, size);
    latestFirst.resize(static_cast<std::size_t>(end));
    rt::sort(latestFirst, [](const LedgerEntry& a, const LedgerEntry& b) { return a.lastIndex - b.lastIndex; }); return latestFirst;
  }
  std::string composeHybridSummary(const std::string& modelText, const std::string& pureSummary) {
    const auto ledger = harvestLedger(pureSummary);
    return rt::concat(rt::concat(modelText, renderLedgerSection(ledger, static_cast<double>(rt::length(pureSummary)))), renderTailSection(pureSummary));
  }
  std::vector<std::string> chunkMemoryForResidue(const std::string& memory, double maxChars) {
    if (maxChars <= 0 || static_cast<double>(rt::length(memory)) <= maxChars) return {memory};
    std::vector<std::string> chunks; std::string current;
    const auto flush = [&] { if (!current.empty()) { chunks.push_back(current); current.clear(); } };
    static const rt::Regex BOUNDARY(R"(\n(?=\[))");
    for (const auto& block : rt::split(memory, BOUNDARY)) {
      const double size = static_cast<double>(rt::length(block));
      if (size > maxChars) { flush(); for (double start = 0; start < size; start += maxChars) chunks.push_back(rt::slice(block, start, start + maxChars)); continue; }
      if (!current.empty() && static_cast<double>(rt::length(current)) + size + 1 > maxChars) { flush(); current = block; }
      else current = current.empty() ? block : rt::concat(rt::concat(current, "\n"), block);
    }
    flush(); return chunks;
  }
  std::optional<std::string> residueText(const json::value* payload) {
    if (!payload || (!payload->is_object() && !payload->is_array())) return std::nullopt;
    const auto* choices = rt::getArray(*payload, "choices"); if (!choices || choices->empty()) return std::nullopt;
    if ((*choices)[0].is_null()) throw rt::TypeError("null is not an object (evaluating 'choices[0].message')");
    const auto* message = rt::get((*choices)[0], "message");
    if (!message || (!message->is_object() && !message->is_array())) return std::nullopt;
    const auto content = utils::getString(rt::get(*message, "content"));
    return content && !rt::trim(*content).empty() ? content : std::nullopt;
  }
  std::optional<std::string> residueText(const json::value& payload) { return residueText(&payload); }
  rt::Task<std::optional<std::string>> runHybridResiduePass(std::string summary) { return runHybridResiduePass(std::move(summary), HybridResidueDeps{}); }
  rt::Task<std::optional<std::string>> runHybridResiduePass(std::string summary, HybridResidueDeps deps) {
    const auto apiKey = deps.apiKey == rt::undefined ? provider::cerebras::readCerebrasApiKey() :
                                                   deps.apiKey == nullptr ? std::nullopt : std::optional<std::string>(*deps.apiKey);
    if (!apiKey || apiKey->empty()) { hybridFailureLog("missing-key"); co_return std::nullopt; }
    const auto chunks = chunkMemoryForResidue(summary, deps.chunkChars.value_or(HYBRID_INPUT_CHUNK_CHARS));
    try {
      auto work = std::make_shared<ResidueWork>(); work->chunks = chunks; work->texts.resize(chunks.size());
      work->apiKey = *apiKey; work->timeoutMs = deps.timeoutMs.value_or(HYBRID_RESIDUE_TIMEOUT_MS); work->fetcher = deps.fetcher;
      std::vector<rt::Promise<void>> workers;
      const auto count = std::max<std::size_t>(1, std::min<std::size_t>(static_cast<std::size_t>(HYBRID_CHUNK_CONCURRENCY), chunks.size()));
      for (std::size_t i = 0; i < count; ++i) workers.emplace_back(residueWorker(work));
      auto all = rt::all(std::move(workers)); co_await all.wait();
      if (work->failed == chunks.size()) { hybridFailureLog("all-chunks-failed"); co_return std::nullopt; }
      const auto composed = composeHybridSummary(rt::join(work->texts, "\n\n"), summary);
      if (rt::trim(composed).empty() || rt::length(composed) >= rt::length(summary)) { hybridFailureLog("no-reduction"); co_return std::nullopt; }
      co_return composed;
    } catch (...) {
      const auto failure = std::current_exception(); const auto message = rt::isError(failure) ? rt::errorMessage(failure).value_or("") : "";
      if (rt::startsWith(message, "upstream-") || message == "empty-output") hybridFailureLog(message);
      else hybridFailureLog(rt::isError(failure) && rt::errorName(failure) == "TimeoutError" ? "timeout" : "transport");
    }
    co_return std::nullopt;
  }
}
