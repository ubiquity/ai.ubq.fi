#include "uos/admin/capacity_observation_sync.hpp"

#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "uos/provider/capacity_analytics.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/text_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uos::admin::capacity_observation_sync {
  namespace {
    namespace capacity_analytics = provider::capacity_analytics;

    constexpr double MAX_OBSERVATION_BODY_BYTES = 4 * 1024 * 1024;

    // `Promise<string | null>`: std::nullopt is null.
    rt::Task<std::optional<std::string>> readBoundedBody(rt::Request request) {
      const double contentLength = rt::stringToNumber(request.headers().get("content-length").value_or(""));
      if (rt::Number::isFinite(contentLength) && contentLength > MAX_OBSERVATION_BODY_BYTES) co_return std::nullopt;
      const std::optional<rt::ByteStream> body = request.body();
      if (!body) co_return std::string();
      const rt::ReadableStreamDefaultReader<rt::ByteChunk> reader = body->getReader();
      std::vector<rt::ByteChunk> chunks;
      double total = 0;
      for (;;) {
        rt::ReadableStreamReadResult<rt::ByteChunk> step = co_await reader.read();
        if (step.done) break;
        total += static_cast<double>(step.value->size());
        if (total > MAX_OBSERVATION_BODY_BYTES) {
          // `await reader.cancel().catch(() => {})`
          const rt::Promise<void> cancelled = reader.cancelPromise().catch_([](const rt::Reason&) {});
          co_await cancelled.wait();
          co_return std::nullopt;
        }
        chunks.push_back(std::move(*step.value));
      }
      std::vector<std::uint8_t> merged;
      merged.reserve(static_cast<std::size_t>(total));
      for (const rt::ByteChunk& chunk : chunks) merged.insert(merged.end(), chunk.begin(), chunk.end());
      co_return rt::decodeUtf8(merged);
    }

    rt::Task<rt::Response> handleIngest(rt::Request request, std::optional<std::shared_ptr<rt::Kv>> kvOption, std::function<double()> now) {
      std::shared_ptr<rt::Kv> kv;
      if (!kvOption) {
        const rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
        kv = co_await opening.wait();
      } else {
        kv = *kvOption;
      }
      if (!kv) co_return http::openaiError(503, "KV unavailable", "server_error");
      const std::optional<std::string> body = co_await readBoundedBody(request);
      if (!body) co_return http::openaiError(413, "Capacity observation payload too large", "invalid_request_error");
      json::value parsed;
      bool malformed = false;
      try {
        parsed = rt::jsonParse(*body);
      } catch (...) {
        malformed = true;
      }
      if (malformed) co_return http::openaiError(400, "Invalid capacity observation JSON", "invalid_request_error");
      if (!parsed.is_object()) {
        co_return http::openaiError(400, "Invalid capacity observation envelope", "invalid_request_error");
      }
      // `parsed as { host?: unknown; history?: unknown }`
      const json::object& envelope = parsed.get_object();
      const json::value* host = rt::get(envelope, "host");
      if (!capacity_analytics::isRemoteCapacityHostId(host)) co_return http::openaiError(400, "Invalid capacity observation host", "invalid_request_error");
      std::optional<std::vector<provider::capacity_contract::ProviderCapacityHistoryPoint>> points =
        capacity_analytics::parseRemoteCapacityObservations(rt::get(envelope, "history"));
      if (!points) co_return http::openaiError(400, "Invalid capacity observations", "invalid_request_error");
      const double nowMs = rt::Math::trunc(now ? now() : rt::dateNow());
      const std::string hostId(host->get_string());
      double accepted = 0;
      bool unavailable = false;
      try {
        rt::Task<double> storing = capacity_analytics::storeRemoteCapacityObservations(kv, hostId, std::move(*points), nowMs);
        accepted = co_await std::move(storing);
      } catch (...) {
        unavailable = true;
      }
      if (unavailable) co_return http::openaiError(503, "Capacity observation storage unavailable", "server_error");
      json::object payload;
      payload["accepted"] = rt::makeNumber(accepted);
      payload["host"] = hostId;
      co_return http::json(200, json::value(std::move(payload)), rt::Headers{{"Cache-Control", "no-store"}});
    }
  }

  rt::Task<rt::Response> handleCapacityObservationIngest(rt::Request request) {
    return handleCapacityObservationIngest(std::move(request), CapacityObservationIngestOptions{});
  }

  rt::Task<rt::Response> handleCapacityObservationIngest(rt::Request request, const CapacityObservationIngestOptions& options) {
    return handleIngest(std::move(request), options.kv, options.now);
  }
}
