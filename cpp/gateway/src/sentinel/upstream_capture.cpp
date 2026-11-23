#include "uos/sentinel/upstream_capture.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <memory>
#include <span>
#include <utility>

namespace uos::sentinel::upstream_capture {
  namespace {
    // `new Set(values).has(value)` over a constant list.
    template <std::size_t N>
    bool setHas(const std::array<std::string_view, N>& values, std::string_view value) {
      return std::find(values.begin(), values.end(), value) != values.end();
    }

    constexpr const auto& SAFE_RESPONSE_HEADER_NAME_SET = SENTINEL_UPSTREAM_SAFE_RESPONSE_HEADER_NAMES;

    constexpr std::array<std::string_view, 7> PROVIDER_SET{"chatgpt_codex", "surplus", "metered", "cerebras", "deepseek", "lithos", "openrouter"};
    constexpr std::array<std::string_view, 5> TERMINAL_SET{"pending", "fetch_error", "eof", "read_error", "cancelled"};
    constexpr std::array<std::string_view, 3> CONTENT_TYPE_SET{"text/event-stream", "application/json", "other"};

    json::object freezeAttempt(const SentinelUpstreamAttempt& attempt) {
      json::object frozen;
      rt::objectSet(frozen, "provider", json::value(json::string(attempt.provider)));
      rt::objectSet(frozen, "status", attempt.status ? rt::makeNumber(*attempt.status) : json::value(nullptr));
      rt::objectSet(frozen, "content_type", attempt.content_type ? json::value(json::string(*attempt.content_type)) : json::value(nullptr));
      json::array chunks;
      chunks.reserve(attempt.chunks_base64.size());
      for (const std::string& chunk : attempt.chunks_base64) chunks.emplace_back(json::string(chunk));
      rt::objectSet(frozen, "chunks_base64", json::value(std::move(chunks)));
      rt::objectSet(frozen, "terminal", json::value(json::string(attempt.terminal)));
      if (attempt.headers) rt::objectSet(frozen, "headers", json::value(*attempt.headers));
      if (attempt.headers_truncated) rt::objectSet(frozen, "headers_truncated", json::value(*attempt.headers_truncated));
      if (attempt.chunk_times_ms) {
        json::array times;
        times.reserve(attempt.chunk_times_ms->size());
        for (double time : *attempt.chunk_times_ms) times.push_back(rt::makeNumber(time));
        rt::objectSet(frozen, "chunk_times_ms", json::value(std::move(times)));
      }
      if (attempt.started_at_ms) rt::objectSet(frozen, "started_at_ms", rt::makeNumber(*attempt.started_at_ms));
      rt::objectSet(frozen, "headers_at_ms", attempt.headers_at_ms);
      rt::objectSet(frozen, "ended_at_ms", attempt.ended_at_ms);
      return frozen;
    }

    json::object freezeTrace(const SentinelUpstreamTrace& value) {
      json::object frozen;
      rt::objectSet(frozen, "version", rt::makeNumber(value.version));
      json::array attempts;
      attempts.reserve(value.attempts.size());
      for (const SentinelUpstreamAttempt& attempt : value.attempts) attempts.emplace_back(freezeAttempt(attempt));
      rt::objectSet(frozen, "attempts", json::value(std::move(attempts)));
      rt::objectSet(frozen, "attempts_truncated", json::value(value.attempts_truncated));
      rt::objectSet(frozen, "bytes_truncated", json::value(value.bytes_truncated));
      rt::objectSet(frozen, "chunks_truncated", json::value(value.chunks_truncated));
      return frozen;
    }

    std::string encodeBase64(std::span<const std::uint8_t> bytes) { return rt::toBase64(bytes); }

    std::vector<std::uint8_t> decodeBase64(std::string_view value) { return rt::atobBytes(value); }

    bool isCanonicalBase64(std::string_view value) {
      static const rt::Regex STANDARD_BASE64("^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$");
      if (value.empty() || !STANDARD_BASE64.test(value) || rt::length(value) % 4 != 0) return false;
      try {
        std::vector<std::uint8_t> decoded = decodeBase64(value);
        const std::string reencoded = encodeBase64(decoded);
        std::fill(decoded.begin(), decoded.end(), std::uint8_t{0});
        return reencoded == value;
      } catch (...) {
        return false;
      }
    }

    template <class T>
    [[noreturn]] void cloneIndex(const T&, std::size_t index, std::string_view message) {
      throw rt::JsError(std::string(message) + " (attempt " + rt::numberToString(static_cast<double>(index)) + ")");
    }

    std::string canonicalChunk(const json::value& encoded, std::size_t index) {
      if (!encoded.is_string() || !isCanonicalBase64(encoded.get_string())) {
        cloneIndex(encoded, index, "Sentinel upstream chunk base64 is invalid");
      }
      return std::string(encoded.get_string());
    }

    struct SentinelTraceAccumulator {
      std::vector<SentinelUpstreamAttempt> attempts;
      std::vector<std::vector<std::uint8_t>> decodedChunks;
      std::int64_t totalBytes = 0;
      std::int64_t totalChunks = 0;
    };

    constexpr std::array<std::string_view, 5> SENTINEL_ATTEMPT_V1_KEYS{"provider", "status", "content_type", "chunks_base64", "terminal"};
    constexpr std::array<std::string_view, 11> SENTINEL_ATTEMPT_V2_KEYS{
      "provider",
      "status",
      "content_type",
      "chunks_base64",
      "terminal",
      "headers",
      "headers_truncated",
      "chunk_times_ms",
      "started_at_ms",
      "headers_at_ms",
      "ended_at_ms",
    };

    /** Validate one attempt's exact key set for its trace version and return it as a raw record. */
    const json::object& sentinelAttemptRecord(const json::value& attempt, std::size_t index, std::int64_t version) {
      const std::span<const std::string_view> expected =
        version == 2 ? std::span<const std::string_view>(SENTINEL_ATTEMPT_V2_KEYS) : std::span<const std::string_view>(SENTINEL_ATTEMPT_V1_KEYS);
      const json::object* record = attempt.if_object();
      const std::vector<std::string> keys = record ? rt::objectKeys(*record) : std::vector<std::string>();
      const bool unexpected = std::any_of(keys.begin(), keys.end(), [&](const std::string& key) { return std::find(expected.begin(), expected.end(), key) == expected.end(); });
      if (keys.size() != expected.size() || unexpected) {
        cloneIndex(attempt, index, "Sentinel upstream attempt keys are invalid");
      }
      return *record;
    }

    SentinelUpstreamProvider sentinelAttemptProvider(const json::value* value, std::size_t index) {
      if (!value || !value->is_string() || !setHas(PROVIDER_SET, value->get_string())) {
        cloneIndex(value, index, "Sentinel upstream attempt provider is invalid");
      }
      return std::string(value->get_string());
    }

    std::optional<std::int64_t> sentinelAttemptStatus(const json::value* value, std::size_t index) {
      if (value && value->is_null()) return std::nullopt;
      const std::optional<double> number = rt::getNumber(value);
      if (!number || !rt::Number::isSafeInteger(*number) || *number < 100 || *number > 599) {
        cloneIndex(value, index, "Sentinel upstream attempt status is invalid");
      }
      return static_cast<std::int64_t>(*number);
    }

    std::optional<SentinelUpstreamContentType> sentinelAttemptContentType(const json::value* value, std::size_t index) {
      if (value && value->is_null()) return std::nullopt;
      if (!value || !value->is_string() || !setHas(CONTENT_TYPE_SET, value->get_string())) {
        cloneIndex(value, index, "Sentinel upstream attempt content type is invalid");
      }
      return std::string(value->get_string());
    }

    SentinelUpstreamTerminal sentinelAttemptTerminal(const json::value* value, std::size_t index) {
      if (!value || !value->is_string() || !setHas(TERMINAL_SET, value->get_string())) {
        cloneIndex(value, index, "Sentinel upstream attempt terminal is invalid");
      }
      return std::string(value->get_string());
    }

    const json::array& sentinelAttemptChunks(const json::value* value, std::size_t index) {
      const json::array* chunks = rt::getArray(value);
      if (!chunks) {
        cloneIndex(value, index, "Sentinel upstream attempt chunks are invalid");
      }
      return *chunks;
    }

    SentinelUpstreamSafeHeaders sentinelAttemptHeaders(const json::value* value, std::size_t index) {
      const json::object* record = rt::getObject(value);
      if (!record) {
        cloneIndex(value, index, "Sentinel upstream attempt headers are invalid");
      }
      SentinelUpstreamSafeHeaders headers;
      for (const json::key_value_pair* entry : rt::objectEntries(*record)) {
        const std::string_view name(entry->key());
        const json::value& headerValue = entry->value();
        if (!setHas(SAFE_RESPONSE_HEADER_NAME_SET, name)) {
          cloneIndex(name, index, "Sentinel upstream attempt header name is not allowlisted");
        }
        if (!headerValue.is_string() || rt::length(headerValue.get_string()) == 0 || rt::length(headerValue.get_string()) > SENTINEL_UPSTREAM_MAX_HEADER_VALUE_CHARS) {
          cloneIndex(name, index, "Sentinel upstream attempt header value is invalid");
        }
        const std::string text(headerValue.get_string());
        if (rt::trim(text) != text) cloneIndex(name, index, "Sentinel upstream attempt header value is not canonical");
        rt::objectSet(headers, name, json::value(json::string(text)));
      }
      return headers;
    }

    std::vector<double> sentinelAttemptChunkTimes(const json::value* value, std::size_t index, std::size_t chunkCount) {
      const json::array* candidates = rt::getArray(value);
      if (!candidates || candidates->size() != chunkCount) {
        cloneIndex(value, index, "Sentinel upstream attempt chunk timing does not match its chunks");
      }
      std::vector<double> times;
      double previous = 0;
      for (const json::value& candidate : *candidates) {
        const std::optional<double> number = rt::getNumber(candidate);
        if (!number || !rt::Number::isSafeInteger(*number) || *number < 0 || *number > static_cast<double>(SENTINEL_UPSTREAM_MAX_TIMING_MS) || *number < previous) {
          cloneIndex(candidate, index, "Sentinel upstream attempt chunk timing is invalid");
        }
        previous = *number;
        times.push_back(previous);
      }
      return times;
    }

    double sentinelAttemptClock(const json::value* value, std::size_t index, std::string_view label) {
      const std::optional<double> number = rt::getNumber(value);
      if (!number || !rt::Number::isSafeInteger(*number) || *number <= 0 || *number > rt::Number::MAX_SAFE_INTEGER) {
        cloneIndex(value, index, "Sentinel upstream attempt " + std::string(label) + " is invalid");
      }
      return *number;
    }

    void assertSentinelAttemptHeaderConsistency(const std::optional<std::int64_t>& status,
                                                const std::optional<SentinelUpstreamContentType>& contentType,
                                                std::size_t chunkCount,
                                                std::size_t index) {
      if (!status && contentType) {
        cloneIndex(status, index, "Sentinel upstream pre-header attempt cannot carry a content type");
      }
      if (status && !contentType) {
        cloneIndex(status, index, "Sentinel upstream header-bearing attempt requires a content type");
      }
      if (!status && chunkCount > 0) {
        cloneIndex(status, index, "Sentinel upstream pre-header attempt cannot have chunks");
      }
    }

    void assertSentinelAttemptTerminalConsistency(const std::optional<std::int64_t>& status,
                                                  const std::optional<SentinelUpstreamContentType>& contentType,
                                                  const SentinelUpstreamTerminal& terminal,
                                                  std::size_t chunkCount,
                                                  std::size_t index) {
      if (terminal == "fetch_error" && (status || contentType || chunkCount > 0)) {
        cloneIndex(terminal, index, "Sentinel upstream fetch_error attempt is inconsistent");
      }
      if ((terminal == "eof" || terminal == "read_error" || terminal == "cancelled") && !status) {
        cloneIndex(terminal, index, "Sentinel upstream header-bearing terminal requires a status");
      }
    }

    /** Retain one attempt's chunks under the aggregate trace bounds, in wire order. */
    std::vector<std::string> retainSentinelChunks(const json::array& encodedChunks, std::size_t index, SentinelTraceAccumulator& state) {
      if (state.totalChunks + static_cast<std::int64_t>(encodedChunks.size()) > SENTINEL_UPSTREAM_MAX_CHUNKS) {
        throw rt::JsError("Sentinel upstream trace has too many chunks");
      }
      state.totalChunks += static_cast<std::int64_t>(encodedChunks.size());
      std::vector<std::string> chunks;
      for (const json::value& encoded : encodedChunks) {
        std::string text = canonicalChunk(encoded, index);
        state.decodedChunks.push_back(decodeBase64(text));
        state.totalBytes += static_cast<std::int64_t>(state.decodedChunks.back().size());
        if (state.totalBytes > SENTINEL_UPSTREAM_MAX_BYTES) {
          throw rt::JsError("Sentinel upstream trace exceeds its byte bound");
        }
        chunks.push_back(std::move(text));
      }
      return chunks;
    }

    /** One version-2 attempt's validated header and timing metadata. */
    struct SentinelAttemptMetadata {
      SentinelUpstreamSafeHeaders headers;
      bool headersTruncated = false;
      std::vector<double> chunkTimes;
      double startedAtMs = 0;
      std::optional<double> headersAtMs;
      std::optional<double> endedAtMs;
    };

    /** Reject an attempt whose recorded timing disagrees with its status or terminal. */
    void assertSentinelAttemptTiming(const std::optional<std::int64_t>& status,
                                     const SentinelUpstreamTerminal& terminal,
                                     const SentinelAttemptMetadata& metadata,
                                     std::size_t index) {
      const double startedAtMs = metadata.startedAtMs;
      const std::optional<double>& headersAtMs = metadata.headersAtMs;
      const std::optional<double>& endedAtMs = metadata.endedAtMs;
      const std::vector<double>& chunkTimes = metadata.chunkTimes;
      if (status && !headersAtMs) {
        cloneIndex(status, index, "Sentinel upstream header-bearing attempt requires a header time");
      }
      if (!status && headersAtMs) {
        cloneIndex(status, index, "Sentinel upstream pre-header attempt cannot carry a header time");
      }
      if (headersAtMs && *headersAtMs < startedAtMs) {
        cloneIndex(headersAtMs, index, "Sentinel upstream attempt header time predates its start");
      }
      if (endedAtMs && *endedAtMs < headersAtMs.value_or(startedAtMs)) {
        cloneIndex(endedAtMs, index, "Sentinel upstream attempt end time predates its headers");
      }
      if ((terminal == "pending") != !endedAtMs) {
        cloneIndex(terminal, index, "Sentinel upstream attempt end time disagrees with its terminal");
      }
      if (!endedAtMs) return;
      const double elapsed = *endedAtMs - startedAtMs;
      if (elapsed > static_cast<double>(SENTINEL_UPSTREAM_MAX_TIMING_MS) || std::any_of(chunkTimes.begin(), chunkTimes.end(), [elapsed](double time) { return time > elapsed; })) {
        cloneIndex(endedAtMs, index, "Sentinel upstream attempt timing exceeds its bounds");
      }
    }

    /** Validate and normalize one version-2 attempt's headers and timing metadata. */
    SentinelAttemptMetadata sentinelAttemptV2Metadata(const json::object& wire,
                                                      const std::optional<std::int64_t>& status,
                                                      const SentinelUpstreamTerminal& terminal,
                                                      std::size_t chunksLength,
                                                      std::size_t index) {
      SentinelAttemptMetadata metadata;
      metadata.headers = sentinelAttemptHeaders(rt::get(wire, "headers"), index);
      // A local primitive keeps the validated boolean type explicit: the wire value
      // stays rejected when it is not a boolean, and no assertion is needed.
      const json::value* headersTruncated = rt::get(wire, "headers_truncated");
      if (!headersTruncated || !headersTruncated->is_bool()) {
        cloneIndex(headersTruncated, index, "Sentinel upstream attempt header truncation flag is invalid");
      }
      metadata.headersTruncated = headersTruncated->get_bool();
      metadata.chunkTimes = sentinelAttemptChunkTimes(rt::get(wire, "chunk_times_ms"), index, chunksLength);
      metadata.startedAtMs = sentinelAttemptClock(rt::get(wire, "started_at_ms"), index, "start time");
      const json::value* headersAt = rt::get(wire, "headers_at_ms");
      metadata.headersAtMs = headersAt && headersAt->is_null() ? std::nullopt : std::optional<double>(sentinelAttemptClock(headersAt, index, "header time"));
      const json::value* endedAt = rt::get(wire, "ended_at_ms");
      metadata.endedAtMs = endedAt && endedAt->is_null() ? std::nullopt : std::optional<double>(sentinelAttemptClock(endedAt, index, "end time"));
      assertSentinelAttemptTiming(status, terminal, metadata, index);
      return metadata;
    }

    /** Validate one attempt, decode its retained chunks, and append it to the trace under construction. */
    void appendSentinelAttempt(const json::value& raw, std::size_t index, SentinelTraceAccumulator& state, std::int64_t version) {
      const json::object& wire = sentinelAttemptRecord(raw, index, version);
      SentinelUpstreamProvider provider = sentinelAttemptProvider(rt::get(wire, "provider"), index);
      const std::optional<std::int64_t> status = sentinelAttemptStatus(rt::get(wire, "status"), index);
      std::optional<SentinelUpstreamContentType> contentType = sentinelAttemptContentType(rt::get(wire, "content_type"), index);
      SentinelUpstreamTerminal terminal = sentinelAttemptTerminal(rt::get(wire, "terminal"), index);
      std::vector<std::string> chunks = retainSentinelChunks(sentinelAttemptChunks(rt::get(wire, "chunks_base64"), index), index, state);
      assertSentinelAttemptHeaderConsistency(status, contentType, chunks.size(), index);
      assertSentinelAttemptTerminalConsistency(status, contentType, terminal, chunks.size(), index);
      SentinelUpstreamAttempt attempt;
      attempt.provider = std::move(provider);
      attempt.status = status;
      attempt.content_type = std::move(contentType);
      attempt.chunks_base64 = std::move(chunks);
      attempt.terminal = std::move(terminal);
      if (version == 2) {
        SentinelAttemptMetadata metadata = sentinelAttemptV2Metadata(wire, attempt.status, attempt.terminal, attempt.chunks_base64.size(), index);
        attempt.headers = std::move(metadata.headers);
        attempt.headers_truncated = metadata.headersTruncated;
        attempt.chunk_times_ms = std::move(metadata.chunkTimes);
        attempt.started_at_ms = metadata.startedAtMs;
        attempt.headers_at_ms = rt::orNull(metadata.headersAtMs);
        attempt.ended_at_ms = rt::orNull(metadata.endedAtMs);
      }
      state.attempts.push_back(std::move(attempt));
    }

    /** Code-unit ordering, exactly the default `Array.prototype.sort` comparison. */
    int compareCodeUnits(std::string_view left, std::string_view right) {
      if (rt::lessThan(left, right)) return -1;
      if (rt::lessThan(right, left)) return 1;
      return 0;
    }

    std::string canonicalValue(const json::value& value);

    std::string canonicalEntry(const json::key_value_pair& entry) { return rt::jsonQuote(entry.key()) + ":" + canonicalValue(entry.value()); }

    std::string canonicalValue(const json::value& value) {
      if (value.is_null()) return "null";
      if (value.is_string()) return rt::jsonQuote(value.get_string());
      if (rt::isNumber(value) || value.is_bool()) return rt::jsonStringify(value);
      if (const json::array* array = value.if_array()) {
        std::string body;
        for (std::size_t index = 0; index < array->size(); ++index) {
          if (index > 0) body += ",";
          body += canonicalValue((*array)[index]);
        }
        return "[" + body + "]";
      }
      if (const json::object* object = value.if_object()) {
        std::vector<const json::key_value_pair*> entries = rt::objectEntries(*object);
        rt::sort(entries, [](const json::key_value_pair* left, const json::key_value_pair* right) { return compareCodeUnits(left->key(), right->key()); });
        std::string body;
        for (std::size_t index = 0; index < entries.size(); ++index) {
          if (index > 0) body += ",";
          body += canonicalEntry(*entries[index]);
        }
        return "{" + body + "}";
      }
      throw rt::JsError("Sentinel upstream trace contains an unsupported value");
    }

    struct MutableAttempt {
      SentinelUpstreamProvider provider;
      std::optional<std::int64_t> status;
      std::optional<SentinelUpstreamContentType> contentType;
      std::vector<std::vector<std::uint8_t>> chunks;
      SentinelUpstreamTerminal terminal;
      bool bookkeepingFailed = false;
      SentinelUpstreamSafeHeaders headers;
      bool headersTruncated = false;
      std::vector<double> chunkTimesMs;
      double startedAtMs = 0;
      std::optional<double> headersAtMs;
      std::optional<double> endedAtMs;
    };

    SentinelUpstreamAttemptHandle noopHandle() {
      SentinelUpstreamAttemptHandle handle;
      handle.wrap = [](rt::Response response) { return response; };
      handle.recordFetchError = [] {};
      return handle;
    }

    /** Monotonic, bounded elapsed milliseconds measured from one attempt's start. */
    double elapsedMs(double startedAtMs, double nowMs) {
      return rt::Math::min(static_cast<double>(SENTINEL_UPSTREAM_MAX_TIMING_MS), rt::Math::max(0.0, rt::Math::trunc(nowMs - startedAtMs)));
    }

    struct RecorderState {
      std::vector<std::shared_ptr<MutableAttempt>> attempts;
      bool attemptsTruncated = false;
      bool bytesTruncated = false;
      bool chunksTruncated = false;
      std::int64_t totalBytes = 0;
      std::int64_t totalChunks = 0;
      bool sealed = false;
      bool disposed = false;
      std::optional<SentinelUpstreamTrace> sealedTrace;
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelUpstreamAttempt& attempt) { out = freezeAttempt(attempt); }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelUpstreamTrace& trace) { out = freezeTrace(trace); }

  SentinelUpstreamTrace emptySentinelUpstreamTrace() {
    SentinelUpstreamTrace trace;
    trace.version = 2;
    trace.attempts_truncated = false;
    trace.bytes_truncated = false;
    trace.chunks_truncated = false;
    return trace;
  }

  std::optional<SentinelUpstreamContentType> normalizeSentinelUpstreamContentType(const std::optional<std::string>& raw) {
    if (!raw) return std::nullopt;
    const std::vector<std::string> parts = rt::split(*raw, ";");
    const std::string mime = parts.empty() ? std::string() : rt::toLowerCase(rt::trim(parts[0]));
    if (mime == "text/event-stream" || mime == "application/json") return mime;
    return std::string("other");
  }

  SentinelUpstreamTrace parseSentinelUpstreamTrace(const json::value& value) {
    const json::object* record = value.if_object();
    if (!record) {
      throw rt::JsError("Sentinel upstream trace is not an object");
    }
    constexpr std::array<std::string_view, 5> expectedKeys{"version", "attempts", "attempts_truncated", "bytes_truncated", "chunks_truncated"};
    const std::vector<std::string> actualKeys = rt::objectKeys(*record);
    if (actualKeys.size() != expectedKeys.size() || std::any_of(actualKeys.begin(), actualKeys.end(), [&](const std::string& key) { return !setHas(expectedKeys, key); })) {
      throw rt::JsError("Sentinel upstream trace keys are invalid");
    }
    const std::optional<double> declaredVersion = rt::getNumber(*record, "version");
    if (!declaredVersion || (*declaredVersion != 1 && *declaredVersion != 2)) throw rt::JsError("Sentinel upstream trace version is invalid");
    const std::int64_t version = *declaredVersion == 1 ? 1 : 2;
    const std::optional<bool> attemptsTruncated = rt::getBool(*record, "attempts_truncated");
    const std::optional<bool> bytesTruncated = rt::getBool(*record, "bytes_truncated");
    const std::optional<bool> chunksTruncated = rt::getBool(*record, "chunks_truncated");
    if (!attemptsTruncated || !bytesTruncated || !chunksTruncated) {
      throw rt::JsError("Sentinel upstream trace truncation flags are invalid");
    }
    const json::array* attempts = rt::getArray(*record, "attempts");
    if (!attempts) throw rt::JsError("Sentinel upstream trace attempts are invalid");
    if (static_cast<std::int64_t>(attempts->size()) > SENTINEL_UPSTREAM_MAX_ATTEMPTS) {
      throw rt::JsError("Sentinel upstream trace has too many attempts");
    }
    SentinelTraceAccumulator state;
    auto zeroDecoded = rt::scopeExit([&state] {
      for (std::vector<std::uint8_t>& chunk : state.decodedChunks) std::fill(chunk.begin(), chunk.end(), std::uint8_t{0});
    });
    for (std::size_t index = 0; index < attempts->size(); index += 1) {
      appendSentinelAttempt((*attempts)[index], index, state, version);
    }
    SentinelUpstreamTrace trace;
    trace.version = version;
    trace.attempts = std::move(state.attempts);
    trace.attempts_truncated = *attemptsTruncated;
    trace.bytes_truncated = *bytesTruncated;
    trace.chunks_truncated = *chunksTruncated;
    return trace;
  }

  SentinelUpstreamTrace parseSentinelUpstreamTrace(const SentinelUpstreamTrace& value) { return parseSentinelUpstreamTrace(json::value_from(value)); }

  bool isSentinelUpstreamTrace(const json::value& value) {
    try {
      parseSentinelUpstreamTrace(value);
      return true;
    } catch (...) {
      return false;
    }
  }

  bool isSentinelUpstreamTrace(const SentinelUpstreamTrace& value) { return isSentinelUpstreamTrace(json::value_from(value)); }

  NormalizedSentinelUpstreamSafeHeaders normalizeSentinelUpstreamSafeHeaders(const rt::Headers& headers) {
    SentinelUpstreamSafeHeaders retained;
    bool truncated = false;
    for (const std::string_view name : SENTINEL_UPSTREAM_SAFE_RESPONSE_HEADER_NAMES) {
      const std::optional<std::string> raw = headers.get(name);
      if (!raw) continue;
      const std::string value = rt::trim(*raw);
      if (value.empty()) continue;
      if (rt::length(value) > SENTINEL_UPSTREAM_MAX_HEADER_VALUE_CHARS) {
        truncated = true;
        continue;
      }
      rt::objectSet(retained, name, json::value(json::string(value)));
    }
    NormalizedSentinelUpstreamSafeHeaders normalized;
    normalized.headers = std::move(retained);
    normalized.truncated = truncated;
    return normalized;
  }

  std::string canonicalSentinelUpstreamJson(const SentinelUpstreamTrace& trace) { return canonicalValue(json::value_from(trace)); }

  SentinelUpstreamRecorder createSentinelUpstreamRecorder() {
    auto state = std::make_shared<RecorderState>();

    const auto finishAttempt = [](MutableAttempt& attempt, const SentinelUpstreamTerminal& terminal, double nowMs) {
      if (attempt.terminal != "pending" || attempt.bookkeepingFailed) return;
      // A bodyless response records EOF with zero bytes; a header-bearing
      // read/cancel terminal requires its status which is set at wrap time.
      attempt.terminal = terminal;
      attempt.endedAtMs = rt::Math::max(attempt.headersAtMs.value_or(attempt.startedAtMs), attempt.startedAtMs, nowMs);
    };

    const auto retainChunk = [state](MutableAttempt& attempt, const rt::ByteChunk& chunk, double nowMs) {
      if (state->sealed || state->disposed || attempt.bookkeepingFailed || attempt.terminal != "pending" || chunk.empty()) {
        return;
      }
      if (state->totalChunks >= SENTINEL_UPSTREAM_MAX_CHUNKS) {
        state->chunksTruncated = true;
        // The byte bound may be exhausted at the same time; each respective
        // bound that prevents capture must set its flag before returning.
        if (state->totalBytes >= SENTINEL_UPSTREAM_MAX_BYTES) state->bytesTruncated = true;
        return;
      }
      if (state->totalBytes >= SENTINEL_UPSTREAM_MAX_BYTES) {
        state->bytesTruncated = true;
        return;
      }
      const std::int64_t remaining = SENTINEL_UPSTREAM_MAX_BYTES - state->totalBytes;
      const std::int64_t byteLength = static_cast<std::int64_t>(chunk.size());
      const std::int64_t keep = byteLength <= remaining ? byteLength : remaining;
      std::vector<std::uint8_t> copy(chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(keep));
      attempt.chunks.push_back(std::move(copy));
      const double previous = attempt.chunkTimesMs.empty() ? 0.0 : attempt.chunkTimesMs.back();
      attempt.chunkTimesMs.push_back(rt::Math::max(previous, elapsedMs(attempt.startedAtMs, nowMs)));
      state->totalChunks += 1;
      state->totalBytes += keep;
      if (keep < byteLength) state->bytesTruncated = true;
    };

    const auto wrapStream = [finishAttempt, retainChunk](const std::shared_ptr<MutableAttempt>& attempt, const rt::Response& response) -> rt::Response {
      std::optional<rt::ByteStream> body = response.body();
      if (!body) {
        finishAttempt(*attempt, "eof", rt::dateNow());
        return response;
      }
      const rt::ReadableStreamDefaultReader<rt::ByteChunk> reader = body->getReader();
      rt::UnderlyingSource<rt::ByteChunk> source;
      source.pull = [attempt, reader, finishAttempt, retainChunk](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) -> rt::Task<void> {
        rt::ReadableStreamReadResult<rt::ByteChunk> result;
        rt::Reason error;
        try {
          result = co_await reader.read();
        } catch (...) {
          error = std::current_exception();
        }
        if (error) {
          finishAttempt(*attempt, "read_error", rt::dateNow());
          try {
            controller.error(error);
          } catch (...) {
            // The downstream may have cancelled while the upstream read failed.
          }
          co_return;
        }
        if (attempt->terminal == "cancelled") co_return;
        if (result.done) {
          finishAttempt(*attempt, "eof", rt::dateNow());
          try {
            controller.close();
          } catch (...) {
            // Concurrent consumer cancellation may have closed the wrapper first.
          }
          co_return;
        }
        try {
          const double nowMs = rt::dateNow();
          retainChunk(*attempt, *result.value, nowMs);
        } catch (...) {
          // Recorder-side bookkeeping failure must never replace a valid
          // original chunk: disable further retention for this attempt and
          // leave it pending so the sealed trace truthfully reports
          // incomplete coverage instead of inventing complete bytes.
          attempt->bookkeepingFailed = true;
        }
        try {
          controller.enqueue(std::move(*result.value));
        } catch (...) {
          // The downstream cancelled after the read; the cancel callback owns
          // the terminal transition and underlying reader cancellation.
        }
      };
      source.cancel = [attempt, reader, finishAttempt](rt::Reason reason) -> rt::Task<void> {
        finishAttempt(*attempt, "cancelled", rt::dateNow());
        return reader.cancel(std::move(reason));
      };
      rt::QueuingStrategy strategy;
      strategy.highWaterMark = 0;
      rt::ByteStream stream(std::move(source), strategy);
      rt::ResponseInit init;
      init.status = static_cast<double>(response.status());
      init.statusText = response.statusText();
      init.headers = response.headers();
      return rt::Response(rt::BodyInit(std::move(stream)), std::move(init));
    };

    const auto wrapAttempt = [wrapStream](const std::shared_ptr<MutableAttempt>& attempt, const rt::Response& response) -> rt::Response {
      attempt->status = response.status() >= 100 && response.status() <= 599 ? std::optional<std::int64_t>(response.status()) : std::nullopt;
      // A real response always carries a content type: a missing or empty MIME
      // normalizes to the fixed `other` literal, never null. The null slot only
      // exists before headers.
      attempt->contentType = normalizeSentinelUpstreamContentType(response.headers().get("content-type")).value_or("other");
      try {
        NormalizedSentinelUpstreamSafeHeaders normalized = normalizeSentinelUpstreamSafeHeaders(response.headers());
        attempt->headers = std::move(normalized.headers);
        attempt->headersTruncated = normalized.truncated;
        attempt->headersAtMs = rt::dateNow();
        return wrapStream(attempt, response);
      } catch (...) {
        return response;
      }
    };

    SentinelUpstreamRecorder recorder;
    recorder.startAttempt = [state, wrapAttempt](SentinelUpstreamProvider provider) -> SentinelUpstreamAttemptHandle {
      if (state->sealed || state->disposed) return noopHandle();
      if (static_cast<std::int64_t>(state->attempts.size()) >= SENTINEL_UPSTREAM_MAX_ATTEMPTS) {
        state->attemptsTruncated = true;
        return noopHandle();
      }
      auto attempt = std::make_shared<MutableAttempt>();
      attempt->provider = std::move(provider);
      attempt->status = std::nullopt;
      attempt->contentType = std::nullopt;
      attempt->terminal = "pending";
      attempt->bookkeepingFailed = false;
      attempt->headersTruncated = false;
      attempt->startedAtMs = rt::dateNow();
      attempt->headersAtMs = std::nullopt;
      attempt->endedAtMs = std::nullopt;
      state->attempts.push_back(attempt);
      SentinelUpstreamAttemptHandle handle;
      handle.wrap = [wrapAttempt, attempt](rt::Response response) { return wrapAttempt(attempt, response); };
      handle.recordFetchError = [state, attempt] {
        if (state->sealed || state->disposed || attempt->terminal != "pending" || attempt->status) return;
        attempt->terminal = "fetch_error";
        attempt->endedAtMs = rt::Math::max(attempt->startedAtMs, rt::dateNow());
      };
      return handle;
    };
    recorder.snapshotAndSeal = [state]() -> SentinelUpstreamTrace {
      const std::optional<SentinelUpstreamTrace>& sealedSnapshot = state->sealedTrace;
      if (state->sealed && sealedSnapshot) return *sealedSnapshot;
      state->sealed = true;
      SentinelUpstreamTrace trace;
      trace.version = 2;
      for (const std::shared_ptr<MutableAttempt>& attempt : state->attempts) {
        SentinelUpstreamAttempt sealedAttempt;
        sealedAttempt.provider = attempt->provider;
        sealedAttempt.status = attempt->status;
        sealedAttempt.content_type = attempt->contentType;
        for (const std::vector<std::uint8_t>& chunk : attempt->chunks) sealedAttempt.chunks_base64.push_back(encodeBase64(chunk));
        sealedAttempt.headers = attempt->headers;
        sealedAttempt.headers_truncated = attempt->headersTruncated;
        sealedAttempt.chunk_times_ms = attempt->chunkTimesMs;
        sealedAttempt.started_at_ms = attempt->startedAtMs;
        sealedAttempt.headers_at_ms = rt::orNull(attempt->headersAtMs);
        sealedAttempt.ended_at_ms = rt::orNull(attempt->endedAtMs);
        sealedAttempt.terminal = attempt->terminal;
        trace.attempts.push_back(std::move(sealedAttempt));
      }
      trace.attempts_truncated = state->attemptsTruncated;
      trace.bytes_truncated = state->bytesTruncated;
      trace.chunks_truncated = state->chunksTruncated;
      state->sealedTrace = trace;
      return trace;
    };
    recorder.dispose = [state] {
      state->disposed = true;
      for (const std::shared_ptr<MutableAttempt>& attempt : state->attempts) {
        for (std::vector<std::uint8_t>& chunk : attempt->chunks) std::fill(chunk.begin(), chunk.end(), std::uint8_t{0});
        attempt->chunks = {};
        attempt->chunkTimesMs = {};
        attempt->headers = json::object();
      }
      state->totalBytes = 0;
      state->totalChunks = 0;
    };
    return recorder;
  }
}
