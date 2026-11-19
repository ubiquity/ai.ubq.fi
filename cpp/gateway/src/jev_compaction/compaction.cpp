/**
 * Gateway Jev compaction route, adapted from 0x4007/fast-jev-compaction (MIT),
 * commit c1eab5fdbd6bde7d67f2da070116496558836884. Selection and rendering stay
 * in the library; this module orchestrates and validates. Every failure leaves
 * the client's original history intact; kept content is never truncated.
 */
#include "uos/jev_compaction/compaction.hpp"
#include "uos/http.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/request.hpp"
#include "uos/request_policy.hpp"
#include "uos/provider/openrouter.hpp"
#include "uos/systemone/handlers.hpp"
#include "uos/lib/jev_compaction/client.hpp"
#include "uos/lib/jev_compaction/compact.hpp"
#include "uos/lib/jev_compaction/codex_items.hpp"
#include "uos/lib/jev_compaction/state.hpp"
#include "uos/jev_compaction/hybrid.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include <algorithm>
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace uos::jev_compaction::compaction {
  namespace {
    namespace library = lib::jev_compaction;
    namespace types = library::types;
    namespace items = library::codex_items;
    constexpr std::string_view TURN_METADATA_HEADER = "x-codex-turn-metadata";
    std::shared_ptr<types::JevAsker> askerForTest;

    const json::object* record(const json::value* value) { return value && value->is_object() ? &value->as_object() : nullptr; }

    std::string ssePayload(const std::string& itemId, const std::string& responseId, const std::string& text) {
      const json::object item{{"type", "message"}, {"role", "assistant"}, {"id", itemId},
                              {"content", json::array{json::object{{"type", "output_text"}, {"text", text}}}}};
      const auto done = rt::jsonStringify(json::object{{"type", "response.output_item.done"}, {"item", item}});
      const auto completed = rt::jsonStringify(json::object{{"type", "response.completed"}, {"response", json::object{{"id", responseId}, {"status", "completed"}}}});
      return "event: response.output_item.done\ndata: " + done + "\n\nevent: response.completed\ndata: " + completed + "\n\n";
    }

    std::string jsonCompletionPayload(const std::string& itemId, const std::string& responseId, const std::string& text) {
      return rt::jsonStringify(json::object{{"id", responseId}, {"object", "response"}, {"status", "completed"},
        {"output", json::array{json::object{{"type", "message"}, {"role", "assistant"}, {"id", itemId},
          {"content", json::array{json::object{{"type", "output_text"}, {"text", text}}}}}}}});
    }

    // Structural only: counts and sizes, never transcript text.
    std::string structuralHeader(const types::CompactResult::Stats& counts, std::int64_t fitted, bool floorBound, bool hybrid) {
      const auto number = [](std::int64_t value) { return rt::numberToString(static_cast<double>(value)); };
      std::vector<std::string> parts{"summary", "kept=" + number(counts.kept), "results_dropped=" + number(counts.resultsDropped),
        "calls_dropped=" + number(counts.callsDropped), "pinned=" + number(counts.pinned), "chars_before=" + number(counts.charsBefore),
        "chars_after=" + number(counts.charsAfter)};
      if (fitted > 0) parts.push_back("fitted=" + number(fitted));
      if (floorBound) parts.push_back("floor=1");
      if (hybrid) parts.push_back("hybrid=1");
      return rt::join(parts, "; ");
    }

    // Library messages may contain provider response text; only fixed classes,
    // status codes and bounded structural counts leave this classifier.
    std::string failureLogDetail(const FailureKind& kind, const std::string& message) {
      static const rt::Regex HTTP(R"(Jev request failed \((\d{3})\))");
      if (const auto match = HTTP.exec(message)) return "http=" + (*match)[1].value_or("");
      if (message.starts_with("Jev request timed out")) return "timeout";
      if (message.starts_with("Jev request aborted")) return "aborted";
      if (rt::includes(message, "malformed JSON")) return "malformed-response";
      if (message.starts_with("Invalid Jev answer")) return "invalid-answer";
      if (rt::includes(message, "no room for questions")) return "state-over-budget";
      if (rt::includes(message, "history too large for Jev")) return "state-too-large";
      if (rt::includes(message, "OPENROUTER_API_KEY")) return "missing-key";
      static const rt::Regex COUNTS(R"(dropped=(\d+), before=(\d+), after=(\d+))");
      if (const auto match = COUNTS.exec(message)) return "dropped=" + (*match)[1].value_or("") + " before=" + (*match)[2].value_or("") + " after=" + (*match)[3].value_or("");
      static const rt::Regex CHARS(R"(summary is (\d+) chars)");
      if (const auto match = CHARS.exec(message)) {
        static const rt::Regex COMPOSITION(R"(unpinned_kept=(\d+) pinned=(\d+) results_dropped=(\d+))");
        const auto composition = COMPOSITION.exec(message);
        return "summary-chars=" + (*match)[1].value_or("") + (composition ? " unpinned_kept=" + (*composition)[1].value_or("") +
          " pinned=" + (*composition)[2].value_or("") + " dropped=" + (*composition)[3].value_or("") : "");
      }
      return kind;
    }

    void logCompaction(const json::object& line) { rt::console::info("[ai.ubq.fi] jev_compaction", rt::jsonStringify(line)); }

    void assertEveryCandidateDecided(const types::CompactResult& result, const std::vector<types::ToolCall>& candidates) {
      std::set<std::string> decidedIds;
      for (const auto& decision : result.decisions) decidedIds.insert(decision.id);
      for (const auto& candidate : candidates)
        if (!decidedIds.contains(candidate.id)) throw CompactionUnavailable("jev-failed", "missing decision for " + candidate.id);
    }

    void assertCompactionReduced(const types::CompactResult::Stats& stats) {
      const auto dropped = stats.resultsDropped + stats.callsDropped;
      if (dropped == 0 || stats.charsAfter >= stats.charsBefore)
        throw CompactionUnavailable("no-reduction", "nothing was dropped (dropped=" + rt::numberToString(static_cast<double>(dropped)) +
          ", before=" + rt::numberToString(static_cast<double>(stats.charsBefore)) + ", after=" + rt::numberToString(static_cast<double>(stats.charsAfter)) + ")");
    }

    std::string renderCompactedSummary(const std::optional<items::CodexTranscript>& transcript, const std::vector<types::ToolCall>& calls,
                                      const types::CompactResult& result, const std::vector<types::CallDecision>& decisions) {
      if (!transcript) throw CompactionUnavailable("missing-input", "compaction request has no input items array");
      items::RenderOptions options;
      options.decisions = decisions;
      for (const auto& call : calls) options.callIds.set(call.id, call.tool_use_id);
      options.headChars = *COMPACTION_OPTIONS.truncateHeadChars;
      options.stats = result.stats;
      std::string summary;
      try { summary = items::renderSummary(*transcript, options); }
      catch (...) {
        const auto error = std::current_exception();
        const auto detail = rt::isError(error) ? rt::errorMessage(error).value_or("") : rt::reasonToString(error);
        throw CompactionUnavailable("render-failed", "render failed: " + rt::slice(detail, 0, 200));
      }
      if (rt::trim(summary).empty()) throw CompactionUnavailable("empty-summary", "renderer produced an empty summary");
      return summary;
    }

    struct FittedSummary { std::string summary; std::int64_t fittedDrops; types::CompactResult::Stats stats; bool floorBound = false; };

    // Drop only unpinned kept results, lowest keepResult first. Pinned data and
    // kept text are never dropped or truncated to force a successful memory.
    FittedSummary fitCompactedSummary(const std::optional<items::CodexTranscript>& transcript, const std::vector<types::ToolCall>& calls,
                                     const types::CompactResult& result, double inputTokens) {
      const auto acceptsFloor = [inputTokens](const std::string& floor) { return inputTokens > 0 && library::state::estimateTokens(floor) <= maxFloorSummaryTokens(inputTokens); };
      const auto summary = renderCompactedSummary(transcript, calls, result, result.decisions);
      if (static_cast<double>(rt::length(summary)) <= SUMMARY_CHAR_CAP) return {summary, 0, result.stats};
      std::vector<types::CallDecision> eligible;
      for (const auto& decision : result.decisions) if (decision.action == "keep" && decision.reason == "kept") eligible.push_back(decision);
      rt::sort(eligible, [](const auto& a, const auto& b) { return a.keepResult - b.keepResult; });
      std::vector<std::string> fitIds;
      for (const auto& decision : eligible) fitIds.push_back(decision.id);
      const auto renderWithDropped = [&](std::size_t count) {
        const std::set<std::string> dropped(fitIds.begin(), fitIds.begin() + static_cast<std::ptrdiff_t>(count));
        auto decisions = result.decisions;
        for (auto& decision : decisions) if (dropped.contains(decision.id)) { decision.action = "drop_result"; decision.reason = "result_dropped"; }
        return renderCompactedSummary(transcript, calls, result, decisions);
      };
      if (fitIds.empty()) {
        if (acceptsFloor(summary)) return {summary, 0, result.stats, true};
        throw CompactionUnavailable("summary-too-large", "summary is " + rt::numberToString(static_cast<double>(rt::length(summary))) +
          " chars (limit " + rt::numberToString(SUMMARY_CHAR_CAP) + "); unpinned_kept=0 pinned=" + rt::numberToString(static_cast<double>(result.stats.pinned)) +
          " results_dropped=" + rt::numberToString(static_cast<double>(result.stats.resultsDropped)));
      }
      const auto adjustStats = [&](std::size_t count) {
        const std::set<std::string> dropped(fitIds.begin(), fitIds.begin() + static_cast<std::ptrdiff_t>(count));
        std::int64_t droppedChars = 0;
        for (const auto& call : calls) if (dropped.contains(call.id)) droppedChars += call.resultChars;
        auto stats = result.stats;
        stats.kept = std::max<std::int64_t>(0, stats.kept - static_cast<std::int64_t>(count));
        stats.resultsDropped += static_cast<std::int64_t>(count);
        stats.charsAfter = std::max<std::int64_t>(0, stats.charsAfter - droppedChars);
        return stats;
      };
      const auto floor = renderWithDropped(fitIds.size());
      if (static_cast<double>(rt::length(floor)) > SUMMARY_CHAR_CAP) {
        if (acceptsFloor(floor)) return {floor, static_cast<std::int64_t>(fitIds.size()), adjustStats(fitIds.size()), true};
        const auto count = rt::numberToString(static_cast<double>(fitIds.size()));
        throw CompactionUnavailable("summary-too-large", "summary is " + rt::numberToString(static_cast<double>(rt::length(floor))) + " chars (limit " +
          rt::numberToString(SUMMARY_CHAR_CAP) + ") with all " + count + " unpinned kept results dropped; unpinned_kept=" + count +
          " pinned=" + rt::numberToString(static_cast<double>(result.stats.pinned)) + " results_dropped=" +
          rt::numberToString(static_cast<double>(result.stats.resultsDropped) + static_cast<double>(fitIds.size())));
      }
      std::size_t low = 0;
      std::size_t high = fitIds.size();
      while (high - low > 1) {
        const auto mid = (low + high) / 2;
        if (static_cast<double>(rt::length(renderWithDropped(mid))) <= SUMMARY_CHAR_CAP) high = mid;
        else low = mid;
      }
      const auto fitted = renderWithDropped(high);
      if (static_cast<double>(rt::length(fitted)) > SUMMARY_CHAR_CAP)
        throw CompactionUnavailable("summary-too-large", "summary is " + rt::numberToString(static_cast<double>(rt::length(fitted))) + " chars (limit " + rt::numberToString(SUMMARY_CHAR_CAP) + ")");
      return {fitted, static_cast<std::int64_t>(high), adjustStats(high)};
    }

    rt::Task<CompactionOutcome> buildCompactionResponseImpl(json::value body, std::shared_ptr<types::JevAsker> asker, BuildCompactionResponseOptions options) {
      const auto* requestBody = record(&body);
      const auto transcript = items::parseCodexInput(requestBody ? rt::get(*requestBody, "input") : nullptr);
      if (!transcript) throw CompactionUnavailable("missing-input", "compaction request has no input items array");
      if (transcript->entries.empty()) throw CompactionUnavailable("empty-transcript", "compaction request has no conversation content");
      const auto calls = library::state::collectToolCalls(transcript->messages, *COMPACTION_OPTIONS.preserveRecentMessages);
      std::vector<types::ToolCall> candidates;
      for (const auto& call : calls) if (!call.pinned) candidates.push_back(call);
      if (candidates.empty()) throw CompactionUnavailable("no-candidates", "no unpinned tool call/result pairs to decide");
      types::CompactResult result;
      try {
        rt::Promise<types::CompactResult> pending(library::compact::compact(transcript->messages, asker, COMPACTION_OPTIONS));
        result = co_await pending.wait();
      } catch (...) {
        const auto error = std::current_exception();
        const auto detail = rt::isError(error) ? rt::errorMessage(error).value_or("") : rt::reasonToString(error);
        throw CompactionUnavailable("jev-failed", rt::slice(detail, 0, 200));
      }
      assertEveryCandidateDecided(result, candidates);
      const auto inputTokens = library::state::estimateTokens(rt::jsonStringify(*rt::get(*requestBody, "input")));
      const auto fitted = fitCompactedSummary(transcript, calls, result, inputTokens);
      auto summary = fitted.summary;
      bool hybridApplied = false;
      std::optional<double> hybridMs;
      if (options.residue) {
        const auto hybridStarted = rt::dateNow();
        try {
          rt::Promise<std::optional<std::string>> rewriting(options.residue(summary));
          const auto rewritten = co_await rewriting.wait();
          if (rewritten && !rt::trim(*rewritten).empty() && rt::length(*rewritten) < rt::length(summary)) { summary = *rewritten; hybridApplied = true; }
        } catch (...) { /* Keep the pure Jev summary. */ }
        hybridMs = rt::dateNow() - hybridStarted;
      }
      auto renderedStats = fitted.stats;
      renderedStats.charsAfter = static_cast<std::int64_t>(rt::length(summary));
      assertCompactionReduced(renderedStats);
      json::object log{{"outcome", "ok"}, {"items", transcript->entries.size()}, {"candidates", candidates.size()}, {"kept", renderedStats.kept},
        {"results_dropped", renderedStats.resultsDropped}, {"calls_dropped", renderedStats.callsDropped}, {"pinned", renderedStats.pinned},
        {"chars_before", renderedStats.charsBefore}, {"chars_after", renderedStats.charsAfter}, {"fitted", fitted.fittedDrops},
        {"floor_bound", fitted.floorBound}, {"hybrid", hybridApplied}, {"hybrid_ms", hybridMs ? rt::makeNumber(*hybridMs) : json::value(nullptr)},
        {"input_estimate_tokens", rt::makeNumber(inputTokens)}, {"floor_tokens", fitted.floorBound ? rt::makeNumber(library::state::estimateTokens(summary)) : json::value(nullptr)},
        {"jev_requests", renderedStats.requests}, {"state_stage", renderedStats.stateStage.empty() ? "none" : renderedStats.stateStage},
        {"windows", renderedStats.windows}, {"total_ms", rt::makeNumber(renderedStats.ms)}};
      logCompaction(log);
      const auto suffix = rt::slice(rt::replaceAll(rt::crypto::randomUUID(), "-", ""), 0, 16);
      rt::OrderedMap<std::string, std::string> headers;
      headers.set("x-jev-compaction", structuralHeader(renderedStats, fitted.fittedDrops, fitted.floorBound, hybridApplied));
      if (options.stream) co_return CompactionOutcome{200, "text/event-stream", headers, ssePayload("msg_jev_" + suffix, "resp_jev_" + suffix, summary)};
      co_return CompactionOutcome{200, "application/json", headers, jsonCompletionPayload("msg_jev_" + suffix, "resp_jev_" + suffix, summary)};
    }

    rt::JsError timedOutError(double timeoutMs) { return rt::JsError("Jev request timed out after " + rt::numberToString(timeoutMs) + "ms"); }

    struct TimeoutFetchState {
      rt::AbortController controller;
      std::optional<rt::AbortSignal> external;
      rt::EventListener forwardExternalAbort;
      rt::Timeout timer;
      bool released = false;
      bool aborted = false;
      std::optional<rt::ReadableStreamDefaultReader<rt::ByteChunk>> reader;
      ~TimeoutFetchState() {
        try { release(); } catch (...) {}
      }
      void release() {
        if (released) return;
        released = true;
        rt::clearTimeout(timer);
        if (external) external->removeEventListener("abort", forwardExternalAbort);
      }
      rt::Reason sanitize(const rt::Reason& error) const {
        const auto reason = controller.signal().reason();
        if (rt::isError(reason) && rt::errorMessage(reason).value_or("").starts_with("Jev request timed out")) return reason;
        if (controller.signal().aborted()) return rt::makeReason(rt::JsError("Jev request aborted"));
        const auto detail = rt::isError(error) ? rt::errorMessage(error).value_or("") : "unknown transport failure";
        return rt::makeReason(rt::JsError("Jev transport error: " + rt::slice(detail, 0, 120)));
      }
    };

    rt::Promise<void> cancelReader(const std::shared_ptr<TimeoutFetchState>& state, rt::Reason reason) {
      const auto swallowCancellation = [](const rt::Reason&) {};
      return state->reader->cancelPromise(std::move(reason)).catch_(swallowCancellation);
    }

    rt::Task<void> pullTimeoutStream(std::shared_ptr<TimeoutFetchState> state, rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
      try {
        const auto next = co_await state->reader->readPromise().wait();
        if (next.done) {
          state->release();
          if (state->aborted) controller.error(state->sanitize(state->controller.signal().reason()));
          else controller.close();
          co_return;
        }
        controller.enqueue(*next.value);
      } catch (...) { state->release(); controller.error(state->sanitize(std::current_exception())); }
    }

    rt::Task<void> cancelTimeoutStream(std::shared_ptr<TimeoutFetchState> state, rt::Reason reason) {
      state->release();
      const auto cancelled = cancelReader(state, std::move(reason));
      co_await cancelled.wait();
    }

    std::shared_ptr<types::JevAsker> defaultAsker(const JevCompactionDeps& deps) {
      const auto apiKey = deps.apiKey.isUndefined() ? provider::openrouter::readOpenRouterApiKey() : deps.apiKey.toOptional();
      if (!apiKey || apiKey->empty()) throw CompactionUnavailable("missing-key", "OPENROUTER_API_KEY is not configured");
      library::client::JevClientOptions options;
      options.apiKey = *apiKey;
      options.baseUrl = std::string(provider::openrouter::OPENROUTER_SYSTEMONE_URL);
      options.model = std::string(systemone::handlers::SYSTEMONE_DEFAULT_MODEL);
      options.fetch = createJevTimeoutFetch(deps.fetch ? deps.fetch : rt::globalFetch(), JEV_TIMEOUT_MS);
      options.signal = deps.signal;
      return std::make_shared<library::client::JevClient>(options);
    }

    rt::Response cancelledResponse() {
      http::OpenaiErrorOptions options;
      options.type = "server_error";
      options.param = std::optional<std::string>{};
      return http::openaiError(499, "Request was cancelled.", "request_cancelled", options);
    }

    rt::Response failureResponse(const FailureKind& kind) {
      const auto message = "Jev compaction unavailable (" + kind + ")";
      if (kind == "missing-key" || kind == "jev-failed" || kind == "render-failed") {
        http::OpenaiErrorOptions options;
        options.type = "server_error";
        return http::openaiError(kind == "missing-key" ? 503 : 502, message, "jev_compaction_unavailable", options);
      }
      return http::openaiError(400, message, "jev_compaction_unavailable");
    }

    CompactionResidue resolveResidue(const JevCompactionDeps& deps) {
      if (deps.residue) return deps.residue;
      if (deps.asker || askerForTest) return {};
      return [](std::string summary) { return hybrid::runHybridResiduePass(std::move(summary)); };
    }

    FailureKind failureKind(const rt::Reason& error) {
      try { std::rethrow_exception(error); }
      catch (const CompactionUnavailable& unavailable) { return unavailable.kind; }
      catch (...) { return "jev-failed"; }
    }

    rt::Response unavailableAskerResponse(const rt::Reason& error) {
      const auto kind = failureKind(error);
      logCompaction(json::object{{"outcome", "failed"}, {"kind", kind}, {"detail", failureLogDetail(kind, rt::isError(error) ? rt::errorMessage(error).value_or("") : "")}});
      return failureResponse(kind);
    }

    rt::Task<rt::Response> handleJevResponsesCompactionImpl(rt::Request req, JevCompactionDeps deps) {
      if (deps.signal && deps.signal->aborted()) co_return cancelledResponse();
      rt::Promise<json::value> reading(request::readJsonBody(req));
      const auto body = co_await reading.wait();
      if (body.is_null()) { logCompaction(json::object{{"outcome", "failed"}, {"kind", "unparsable-body"}}); co_return failureResponse("unparsable-body"); }
      const auto* bodyRecord = record(&body);
      const auto parsedStream = request_policy::parseStreamField(bodyRecord ? rt::get(*bodyRecord, "stream") : nullptr);
      if (const auto* invalid = std::get_if<request_policy::ParseFieldNotOk>(&parsedStream)) {
        http::OpenaiErrorOptions options;
        options.param = std::optional<std::string>("stream");
        co_return http::openaiError(400, invalid->message, "invalid_request_error", options);
      }
      const bool stream = std::get<request_policy::ParseStreamFieldOk>(parsedStream).value;
      std::shared_ptr<types::JevAsker> asker;
      rt::Reason askerFailure;
      try { asker = deps.asker ? deps.asker : askerForTest ? askerForTest : defaultAsker(deps); }
      catch (...) { askerFailure = std::current_exception(); }
      if (askerFailure) co_return unavailableAskerResponse(askerFailure);
      std::optional<CompactionOutcome> outcome;
      rt::Reason failure;
      try {
        const BuildCompactionResponseOptions options{stream, resolveResidue(deps)};
        rt::Promise<CompactionOutcome> building(buildCompactionResponse(body, asker, options));
        outcome = co_await building.wait();
      } catch (...) { failure = std::current_exception(); }
      if (failure) {
        if (deps.signal && deps.signal->aborted()) { logCompaction(json::object{{"outcome", "failed"}, {"kind", "cancelled"}}); co_return cancelledResponse(); }
        const auto kind = failureKind(failure);
        const auto detail = rt::isError(failure) ? rt::errorMessage(failure).value_or("") : rt::reasonToString(failure);
        logCompaction(json::object{{"outcome", "failed"}, {"kind", kind}, {"detail", failureLogDetail(kind, detail)}});
        co_return failureResponse(kind);
      }
      if (deps.signal && deps.signal->aborted()) { logCompaction(json::object{{"outcome", "failed"}, {"kind", "cancelled"}}); co_return cancelledResponse(); }
      // Local success alone carries completion telemetry; no model usage is invented.
      rt::Headers headers{{"content-type", outcome->contentType}, {"cache-control", "no-store"}};
      for (const auto& [name, value] : outcome->headers) headers.set(name, value);
      const rt::ResponseInit init{.status = outcome->status, .headers = headers};
      const rt::Response response(outcome->body, init);
      co_return openai_telemetry::setResponseCompletionTelemetry(response, stream);
    }
  }

  double maxFloorSummaryTokens(double inputTokens) { return rt::Math::floor(inputTokens * (1 - SUMMARY_MIN_REDUCTION_RATIO)); }
  bool isJevCompactionRequest(const rt::Request& req) { return items::isResponsesCompaction(items::parseTurnMetadata(req.headers().get(TURN_METADATA_HEADER))); }
  rt::Task<CompactionOutcome> buildCompactionResponse(const json::value& body, std::shared_ptr<types::JevAsker> asker, const BuildCompactionResponseOptions& options) {
    return buildCompactionResponseImpl(body, std::move(asker), options);
  }

  JevFetch createJevTimeoutFetch(JevFetch baseFetch, double timeoutMs) {
    return JevFetch([baseFetch, timeoutMs](rt::RequestInfo input, rt::RequestInit init) -> rt::Task<rt::Response> {
      const auto state = std::make_shared<TimeoutFetchState>();
      state->external = init.signal;
      const auto weak = std::weak_ptr(state);
      state->forwardExternalAbort = rt::EventListener([weak] { if (const auto current = weak.lock()) current->controller.abort(current->external->reason()); });
      if (state->external) {
        if (state->external->aborted()) state->controller.abort(state->external->reason());
        else state->external->addEventListener("abort", state->forwardExternalAbort, {.once = true});
      }
      state->timer = rt::setTimeout([weak, timeoutMs] { if (const auto current = weak.lock()) current->controller.abort(rt::makeReason(timedOutError(timeoutMs))); }, timeoutMs);
      init.signal = state->controller.signal();
      rt::Promise<rt::Response> pending(baseFetch(std::move(input), std::move(init)));
      const auto fulfilled = [state, weak](rt::Response response) {
        const auto body = response.body();
        if (!body) { state->release(); return response; }
        state->reader = body->getReader();
        const rt::EventListener abort([weak] { if (const auto current = weak.lock()) { current->aborted = true; (void)cancelReader(current, current->controller.signal().reason()); } });
        state->controller.signal().addEventListener("abort", abort, {.once = true});
        rt::UnderlyingSource<rt::ByteChunk> source;
        source.pull = [state](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) { return pullTimeoutStream(state, std::move(controller)); };
        source.cancel = [state](rt::Reason reason) { return cancelTimeoutStream(state, std::move(reason)); };
        const rt::ByteStream stream(source);
        const rt::ResponseInit responseInit{.status = response.status(), .statusText = response.statusText(), .headers = response.headers()};
        return rt::Response(rt::BodyInit(stream), responseInit);
      };
      const auto rejected = [state](const rt::Reason& error) -> rt::Response { state->release(); std::rethrow_exception(state->sanitize(error)); };
      const auto resolved = pending.then(fulfilled, rejected);
      return resolved.wait();
    });
  }
  void setJevCompactionAskerForTest(std::shared_ptr<types::JevAsker> asker) { askerForTest = std::move(asker); }
  rt::Task<rt::Response> handleJevResponsesCompaction(rt::Request req) { return handleJevResponsesCompactionImpl(std::move(req), JevCompactionDeps{}); }
  rt::Task<rt::Response> handleJevResponsesCompaction(rt::Request req, const JevCompactionDeps& deps) { return handleJevResponsesCompactionImpl(std::move(req), deps); }
}
