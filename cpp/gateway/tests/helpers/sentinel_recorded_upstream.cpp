#include "tests/helpers/sentinel_recorded_upstream.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/url.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace uos::tests::helpers::sentinel_recorded_upstream {
  namespace {
    using sentinel::upstream_capture::parseSentinelUpstreamTrace;
    using sentinel::upstream_capture::SentinelUpstreamAttempt;
    using sentinel::upstream_capture::SentinelUpstreamProvider;
    using sentinel::upstream_capture::SentinelUpstreamTrace;

    constexpr std::array<std::string_view, 6> PROVIDERS{"chatgpt_codex", "surplus", "metered", "cerebras", "deepseek", "lithos"};

    constexpr std::string_view ROUTE_INVALID = "Sentinel recorded upstream route is not an exact HTTPS endpoint";
    constexpr std::string_view ROUTE_AMBIGUOUS = "Sentinel recorded upstream routes are not unique across providers";
    constexpr std::string_view TRACE_INVALID = "Sentinel recorded upstream trace is invalid";
    constexpr std::string_view TRACE_EMPTY = "Sentinel recorded upstream trace has no attempts";
    constexpr std::string_view TRACE_TRUNCATED = "Sentinel recorded upstream trace is truncated";
    constexpr std::string_view ATTEMPT_PENDING = "Sentinel recorded upstream attempt is pending";
    constexpr std::string_view ATTEMPT_NO_HEADERS = "Sentinel recorded upstream attempt has no response headers";
    constexpr std::string_view ATTEMPT_INFORMATIONAL = "Sentinel recorded upstream attempt uses an unsupported informational status";
    constexpr std::string_view ATTEMPT_BODYLESS = "Sentinel recorded upstream bodyless attempt is inconsistent";
    constexpr std::string_view ATTEMPT_MIME = "Sentinel recorded upstream attempt lacks an exact MIME category";
    constexpr std::string_view DISPATCH_MISMATCH = "Sentinel recorded upstream dispatch does not match the next recorded attempt";
    constexpr std::string_view DISPATCH_EXTRA = "Sentinel recorded upstream received an extra dispatch";
    constexpr std::string_view DISPATCH_FAILED = "Sentinel recorded upstream replay is already failed";
    constexpr std::string_view STREAM_READ_ERROR = "Sentinel recorded upstream read failure is reproduced";
    constexpr std::string_view STREAM_OVERREAD = "Sentinel recorded upstream prefix was read beyond its recorded chunks";
    constexpr std::string_view INCOMPLETE = "Sentinel recorded upstream replay is incomplete";
    constexpr std::string_view FAILED = "Sentinel recorded upstream replay failed";

    struct ReplayAttempt {
      SentinelUpstreamAttempt attempt;
      std::vector<std::vector<std::uint8_t>> chunks;
      std::size_t nextChunk = 0;
      bool completed = false;
    };

    std::vector<std::uint8_t> decodeStandardBase64(std::string_view encoded) {
      // The trace parser enforces canonical padded base64, so atob never throws.
      return rt::atobBytes(encoded);
    }

    struct ReplayState {
      std::vector<std::shared_ptr<ReplayAttempt>> replayAttempts;
      std::unordered_map<SentinelUpstreamProvider, std::string> routeUrls;
      std::int64_t totalChunks = 0;
      std::int64_t dispatchedCount = 0;
      std::int64_t completedCount = 0;
      bool failed = false;
    };

    void markCompleted(ReplayState& replay, ReplayAttempt& state) {
      if (state.completed) return;
      state.completed = true;
      replay.completedCount += 1;
    }

    rt::Response makeReplayResponse(const std::shared_ptr<ReplayState>& replay, const std::shared_ptr<ReplayAttempt>& state) {
      const SentinelUpstreamAttempt& attempt = state->attempt;
      const std::optional<std::int64_t> status = attempt.status;
      // Validation guarantees only header-bearing attempts reach this point.
      if (!status) throw rt::JsError(std::string(ATTEMPT_NO_HEADERS));
      if (*status == 204 || *status == 205 || *status == 304) {
        markCompleted(*replay, *state);
        rt::ResponseInit init;
        init.status = static_cast<double>(*status);
        return rt::Response(nullptr, std::move(init));
      }
      rt::UnderlyingSource<rt::ByteChunk> source;
      source.pull = [replay, state](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
        if (state->nextChunk < state->chunks.size()) {
          // Exact recorded boundary: one original chunk per pull, zero
          // read-ahead (highWaterMark 0).
          controller.enqueue(state->chunks[state->nextChunk]);
          state->nextChunk += 1;
          return;
        }
        if (state->attempt.terminal == "eof") {
          // EOF closes only on the pull after the final chunk was delivered.
          controller.close();
          markCompleted(*replay, *state);
          return;
        }
        if (state->attempt.terminal == "read_error") {
          controller.error(rt::makeReason(rt::TypeError(std::string(STREAM_READ_ERROR))));
          markCompleted(*replay, *state);
          return;
        }
        // Recorded cancelled prefix: reading past the captured prefix is a
        // permanent failure and must error, never invent EOF.
        replay->failed = true;
        controller.error(rt::makeReason(rt::TypeError(std::string(STREAM_OVERREAD))));
      };
      source.cancel = [replay, state](rt::Reason) {
        if (state->attempt.terminal != "cancelled") return;
        if (state->nextChunk >= state->chunks.size()) {
          markCompleted(*replay, *state);
        } else {
          replay->failed = true;
        }
      };
      rt::QueuingStrategy strategy;
      strategy.highWaterMark = 0;
      rt::ByteStream stream(std::move(source), strategy);
      rt::ResponseInit init;
      init.status = static_cast<double>(*status);
      init.headers = rt::Headers{{"Content-Type", attempt.content_type == "application/json" ? "application/json" : "text/event-stream"}};
      return rt::Response(rt::BodyInit(std::move(stream)), std::move(init));
    }

    rt::Task<rt::Response> rejectWith(std::string_view message) {
      return rt::Promise<rt::Response>::reject(rt::makeReason(rt::TypeError(std::string(message)))).wait();
    }
  }

  RecordedUpstreamReplay createRecordedUpstreamReplay(const SentinelUpstreamTrace& trace, const json::object& routes) {
    SentinelUpstreamTrace parsed;
    // The parser's own error text may embed indices; every helper-visible
    // failure is a fixed static message instead.
    try {
      parsed = parseSentinelUpstreamTrace(trace);
    } catch (...) {
      throw rt::JsError(std::string(TRACE_INVALID));
    }

    if (parsed.attempts.empty()) throw rt::JsError(std::string(TRACE_EMPTY));
    if (parsed.attempts_truncated || parsed.bytes_truncated || parsed.chunks_truncated) {
      throw rt::JsError(std::string(TRACE_TRUNCATED));
    }

    auto replay = std::make_shared<ReplayState>();
    for (const std::string_view provider : PROVIDERS) {
      const std::optional<std::string_view> route = rt::getString(routes, provider);
      if (!route) throw rt::JsError(std::string(ROUTE_INVALID));
      std::optional<rt::URL> url;
      try {
        url.emplace(*route);
      } catch (...) {
        throw rt::JsError(std::string(ROUTE_INVALID));
      }
      if (url->protocol() != "https:" || !url->username().empty() || !url->password().empty() || !url->hash().empty()) {
        throw rt::JsError(std::string(ROUTE_INVALID));
      }
      replay->routeUrls.insert_or_assign(std::string(provider), url->href());
    }
    std::unordered_set<std::string> distinctRoutes;
    for (const auto& [provider, href] : replay->routeUrls) distinctRoutes.insert(href);
    if (distinctRoutes.size() != PROVIDERS.size()) throw rt::JsError(std::string(ROUTE_AMBIGUOUS));

    for (const SentinelUpstreamAttempt& attempt : parsed.attempts) {
      auto state = std::make_shared<ReplayAttempt>();
      state->attempt = attempt;
      if (attempt.terminal == "pending") throw rt::JsError(std::string(ATTEMPT_PENDING));
      if (attempt.terminal == "fetch_error") {
        // The parser enforces status null, no content type and no chunks here.
        replay->replayAttempts.push_back(std::move(state));
        continue;
      }
      if (!attempt.status) throw rt::JsError(std::string(ATTEMPT_NO_HEADERS));
      if (*attempt.status < 200) throw rt::JsError(std::string(ATTEMPT_INFORMATIONAL));
      if (*attempt.status == 204 || *attempt.status == 205 || *attempt.status == 304) {
        // Bodyless responses are only reproducible when the terminal was EOF
        // with no chunks; MIME evidence is not required for a null body.
        if (attempt.terminal != "eof" || !attempt.chunks_base64.empty()) {
          throw rt::JsError(std::string(ATTEMPT_BODYLESS));
        }
        replay->replayAttempts.push_back(std::move(state));
        continue;
      }
      if (attempt.content_type != "application/json" && attempt.content_type != "text/event-stream") {
        throw rt::JsError(std::string(ATTEMPT_MIME));
      }
      for (const std::string& encoded : attempt.chunks_base64) state->chunks.push_back(decodeStandardBase64(encoded));
      replay->replayAttempts.push_back(std::move(state));
    }
    for (const std::shared_ptr<ReplayAttempt>& state : replay->replayAttempts) replay->totalChunks += static_cast<std::int64_t>(state->chunks.size());

    rt::Fetch fetchImpl([replay](rt::RequestInfo input, rt::RequestInit) -> rt::Task<rt::Response> {
      if (replay->failed) return rejectWith(DISPATCH_FAILED);
      std::string requestedUrl;
      try {
        if (const rt::Request* request = std::get_if<rt::Request>(&input)) {
          requestedUrl = request->url();
        } else if (const rt::URL* location = std::get_if<rt::URL>(&input)) {
          requestedUrl = rt::URL(location->href()).href();
        } else {
          requestedUrl = rt::URL(std::get<std::string>(input)).href();
        }
      } catch (...) {
        replay->failed = true;
        return rejectWith(DISPATCH_MISMATCH);
      }
      const std::shared_ptr<ReplayAttempt>* state =
        replay->dispatchedCount < static_cast<std::int64_t>(replay->replayAttempts.size()) ? &replay->replayAttempts[static_cast<std::size_t>(replay->dispatchedCount)] : nullptr;
      if (!state) {
        replay->failed = true;
        return rejectWith(DISPATCH_EXTRA);
      }
      const auto route = replay->routeUrls.find((*state)->attempt.provider);
      if (route == replay->routeUrls.end() || requestedUrl != route->second) {
        replay->failed = true;
        return rejectWith(DISPATCH_MISMATCH);
      }
      replay->dispatchedCount += 1;
      if ((*state)->attempt.terminal == "fetch_error") {
        markCompleted(*replay, **state);
        return rejectWith(STREAM_READ_ERROR);
      }
      return rt::Promise<rt::Response>::resolve(makeReplayResponse(replay, *state)).wait();
    });

    RecordedUpstreamReplay result;
    result.fetch = std::move(fetchImpl);
    result.assertComplete = [replay] {
      if (replay->failed) throw rt::JsError(std::string(FAILED));
      const auto attempts = static_cast<std::int64_t>(replay->replayAttempts.size());
      if (replay->dispatchedCount != attempts || replay->completedCount != attempts) {
        throw rt::JsError(std::string(INCOMPLETE));
      }
    };
    result.snapshot = [replay] {
      RecordedUpstreamReplaySnapshot snapshot;
      snapshot.attemptsDispatched = replay->dispatchedCount;
      snapshot.attemptsCompleted = replay->completedCount;
      // Chunks the consumer actually pulled, which is how a claim that the
      // whole recorded prefix was replayed is attested without requiring a
      // parser to read past its own terminal marker.
      for (const std::shared_ptr<ReplayAttempt>& state : replay->replayAttempts) snapshot.chunksConsumed += static_cast<std::int64_t>(state->nextChunk);
      snapshot.chunksTotal = replay->totalChunks;
      snapshot.failed = replay->failed;
      return snapshot;
    };
    return result;
  }
}
