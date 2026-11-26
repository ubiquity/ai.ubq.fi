#include "uos/sentinel/replay_observation.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/config.hpp"
#include "uos/request.hpp"
#include "uos/utils.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/url.hpp"

#include <openssl/crypto.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace uos::sentinel::replay_observation {
  namespace {
    using replay_model::SentinelClientBodyObservation;
    using replay_model::SentinelClientFailureObservation;
    using replay_model::SentinelFailureObservation;

    const rt::Regex& REQUEST_ID() {
      static const rt::Regex pattern("^[A-Za-z0-9._:-]{1,128}$", "");
      return pattern;
    }
    /**
     * Statuses a persisted row may carry. `unknown` and `status_not_retained` are
     * derived at read time and are deliberately absent: they must never be decoded
     * from storage. `expired` IS persisted by retention reclamation so that an
     * expired payload stays reportable after its manifest and chunks are deleted.
     */
    constexpr std::array<std::string_view, 6> STORED_CAPTURE_STATUSES{"ready", "incomplete", "disabled", "failed", "evicted", "expired"};
    const rt::Regex& STATUS_REASON() {
      static const rt::Regex pattern("^[A-Za-z0-9_.:-]{1,128}$", "");
      return pattern;
    }

    // `typeof value === "object" && value !== null` (utils.isRecord): arrays and every object kind.
    bool isSafeIntegerNumber(const rt::KvValue& value) { return value.isNumber() && rt::Number::isSafeInteger(value.asNumber()); }

    // `x.fill(0)` on a byte array that may hold request or response secrets.
    void zero(rt::Bytes& bytes) {
      if (!bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size());
    }

    /** A blank Content-Type header is treated as absent. */
    std::optional<std::string> optionalContentType(const std::optional<std::string>& value) {
      if (!value) return std::nullopt;
      std::string trimmed = rt::trim(*value);
      return trimmed.empty() ? std::nullopt : std::optional<std::string>(std::move(trimmed));
    }

    // rt::kvIsObject(value) && !Array.isArray(value)
    bool isPlainRecord(const json::value& value) { return utils::isRecord(value) && !value.is_array(); }
    bool isPlainRecord(const json::value* value) { return value && isPlainRecord(*value); }

    struct TerminalBody {
      std::optional<std::string> base64;
      bool truncated;
    };

    /** Bounded base64 of an observed downstream terminal body, with its truncation state. */
    TerminalBody boundedTerminalBody(std::span<const std::uint8_t> bytes) {
      if (bytes.empty()) return {.base64 = std::nullopt, .truncated = false};
      const std::span<const std::uint8_t> bounded =
        bytes.subspan(0, std::min(bytes.size(), static_cast<std::size_t>(replay_model::SENTINEL_REPLAY_MAX_DOWNSTREAM_BODY_BYTES)));
      return {.base64 = rt::toBase64(bounded), .truncated = bounded.size() < bytes.size()};
    }

    std::optional<std::string> errorKind(const json::value* value) {
      if (!utils::isRecord(value)) return std::nullopt;
      if (std::optional<std::string> code = boundedFailureKind(rt::get(value, "code"))) return code;
      return boundedFailureKind(rt::get(value, "type"));
    }

    std::optional<std::string> errorParam(const json::value* value) { return utils::isRecord(value) ? boundedErrorParam(rt::get(value, "param")) : std::nullopt; }

    // `Omit<SentinelClientBodyObservation, "stream" | "framing_valid">`
    struct BodySemantic {
      bool completed;
      std::optional<std::string> terminal_type;
      std::optional<std::string> failure_kind;
      rt::Nullable<std::string> error_code{};
      rt::Nullable<std::string> error_param{};
      rt::Nullable<std::string> terminal_body_base64{};
      std::optional<bool> terminal_body_truncated{};
    };

    // `{ stream, framing_valid, ...semantic }`
    SentinelClientBodyObservation withSemantic(bool stream, bool framingValid, const BodySemantic& semantic) {
      return {
        .stream = stream,
        .completed = semantic.completed,
        .terminal_type = semantic.terminal_type,
        .failure_kind = semantic.failure_kind,
        .framing_valid = framingValid,
        .error_code = semantic.error_code,
        .error_param = semantic.error_param,
        .terminal_body_base64 = semantic.terminal_body_base64,
        .terminal_body_truncated = semantic.terminal_body_truncated,
      };
    }

    /** Semantic terminal of an embeddings-job payload, or null for other statuses. */
    std::optional<BodySemantic> embeddingsJobObservation(const std::optional<std::string>& semanticStatus, const std::optional<std::string>& nestedError) {
      if (semanticStatus == "failed") {
        return BodySemantic{.completed = false, .terminal_type = "job.failed", .failure_kind = nestedError ? *nestedError : std::string("job_failed")};
      }
      if (semanticStatus == "queued" || semanticStatus == "running" || semanticStatus == "in_progress") {
        return BodySemantic{.completed = false, .terminal_type = "job.queued", .failure_kind = nestedError};
      }
      if (semanticStatus == "succeeded" || semanticStatus == "completed") {
        return BodySemantic{.completed = true, .terminal_type = "job.succeeded", .failure_kind = std::nullopt};
      }
      return std::nullopt;
    }

    // `typeof value === "string" ? value.trim().toLowerCase() : null`
    std::optional<std::string> trimmedLower(const json::value* value) {
      if (const std::optional<std::string_view> text = rt::getString(value)) return rt::toLowerCase(rt::trim(*text));
      return std::nullopt;
    }

    std::optional<BodySemantic> responseSemanticObservation(double status, const json::value& parsed) {
      const std::optional<std::string> semanticStatus = trimmedLower(rt::get(parsed, "status"));
      const std::optional<std::string> object = trimmedLower(rt::get(parsed, "object"));
      const std::optional<std::string> nestedError = errorKind(rt::get(parsed, "error"));
      if (object == "embeddings.job") {
        std::optional<BodySemantic> job = embeddingsJobObservation(semanticStatus, nestedError);
        if (job) return job;
      }
      if (semanticStatus == "failed") {
        return BodySemantic{.completed = false,
                            .terminal_type = "response.failed",
                            .failure_kind = nestedError ? *nestedError : std::string("response_failed"),
                            .error_code = rt::orNull(nestedError)};
      }
      if (semanticStatus == "incomplete") {
        const json::value* incompleteDetails = rt::get(parsed, "incomplete_details");
        const json::value* details = utils::isRecord(incompleteDetails) ? incompleteDetails : nullptr;
        std::optional<std::string> reason = boundedFailureKind(details ? rt::get(details, "reason") : nullptr);
        return BodySemantic{.completed = false, .terminal_type = "response.incomplete", .failure_kind = reason ? reason : nestedError};
      }
      if (semanticStatus == "completed" || semanticStatus == "succeeded") {
        return BodySemantic{.completed = true, .terminal_type = "response.completed", .failure_kind = std::nullopt};
      }
      if (utils::isRecord(rt::get(parsed, "error"))) {
        return BodySemantic{.completed = false,
                            .terminal_type = "http.error",
                            .failure_kind = nestedError,
                            .error_code = rt::orNull(nestedError),
                            .error_param = rt::orNull(errorParam(rt::get(parsed, "error")))};
      }
      if (status == 202) return BodySemantic{.completed = false, .terminal_type = "http.accepted", .failure_kind = std::nullopt};
      return std::nullopt;
    }

    std::optional<double> boundedContentLength(const rt::Headers& headers) {
      const std::optional<std::string> raw = headers.get("content-length");
      if (!raw) return std::nullopt;
      static const rt::Regex DECIMAL("^(?:0|[1-9]\\d*)$", "");
      if (!DECIMAL.test(*raw)) throw rt::JsError("Response Content-Length is invalid");
      const double parsed = rt::stringToNumber(*raw);
      if (!rt::Number::isSafeInteger(parsed)) throw rt::JsError("Response Content-Length is invalid");
      return parsed;
    }

    rt::Task<rt::Bytes> readBoundedResponseClone(rt::Response response) {
      const std::optional<double> declared = boundedContentLength(response.headers());
      if (declared && *declared > replay_model::SENTINEL_REPLAY_MAX_BUFFERED_OBSERVATION_BYTES) {
        throw rt::JsError("Response body is too large for sentinel inspection");
      }
      const rt::Response clone = response.clone();
      const std::optional<rt::ByteStream> body = clone.body();
      if (!body) co_return rt::Bytes();
      const rt::ReadableStreamDefaultReader<rt::ByteChunk> reader = body->getReader();
      std::vector<rt::Bytes> chunks;
      double total = 0;
      rt::Reason failure;
      rt::Bytes result;
      try {
        for (;;) {
          rt::ReadableStreamReadResult<rt::ByteChunk> item = co_await reader.read();
          if (item.done) break;
          const rt::ByteChunk& value = *item.value;
          if (static_cast<double>(value.size()) > replay_model::SENTINEL_REPLAY_MAX_BUFFERED_OBSERVATION_BYTES - total) {
            rt::spawn(reader.cancel(), rt::ignoreRejection);
            throw rt::JsError("Response body is too large for sentinel inspection");
          }
          total += static_cast<double>(value.size());
          chunks.push_back(value);
        }
        result = replay_model::concatBytes(chunks);
      } catch (...) {
        failure = std::current_exception();
      }
      for (rt::Bytes& chunk : chunks) zero(chunk);
      reader.releaseLock();
      if (failure) std::rethrow_exception(failure);
      co_return result;
    }

    int terminalRank(const std::optional<std::string>& terminalType) {
      if (terminalType == "response.failed" || terminalType == "error") return 3;
      if (terminalType == "response.incomplete") return 2;
      if (terminalType == "response.completed" || terminalType == "[DONE]") return 1;
      return 0;
    }

    constexpr std::array<std::string_view, 4> SSE_KNOWN_FIELDS{"event", "data", "id", "retry"};

    struct SseEventFields {
      std::optional<std::string> eventName;
      std::vector<std::string> data;
      bool hasUnknownField;
    };

    /** Splits one raw SSE event into its `event` name, `data` lines and field check. */
    SseEventFields parseSseEventFields(const std::string& rawEvent) {
      std::vector<std::string> data;
      std::optional<std::string> eventName;
      bool hasUnknownField = false;
      for (const std::string& line : rt::split(rawEvent, "\n")) {
        if (line.empty() || line.starts_with(":")) continue;
        const std::size_t separator = line.find(':');
        const std::string field = separator == std::string::npos ? line : line.substr(0, separator);
        std::string value = separator == std::string::npos ? std::string() : line.substr(separator + 1);
        // .replace(/^ /, "")
        if (value.starts_with(" ")) value.erase(0, 1);
        if (std::find(SSE_KNOWN_FIELDS.begin(), SSE_KNOWN_FIELDS.end(), field) == SSE_KNOWN_FIELDS.end()) hasUnknownField = true;
        if (field == "event") eventName = value;
        else if (field == "data") data.push_back(value);
      }
      return {.eventName = eventName, .data = data, .hasUnknownField = hasUnknownField};
    }

    // `response?.error`
    const json::value* responseError(const json::value* response) { return response ? rt::get(response, "error") : nullptr; }

    /** The terminal observation for a failed Responses terminal event. */
    BodySemantic failedEventObservation(const std::string& type, const json::value* response, const std::optional<std::string>& topLevelError) {
      std::optional<std::string> kind = errorKind(responseError(response));
      return {
        .completed = false,
        .terminal_type = type,
        .failure_kind = kind ? *kind : (topLevelError ? *topLevelError : std::string("response_failed")),
        .error_code = rt::orNull(kind ? kind : topLevelError),
        .error_param = rt::orNull(errorParam(responseError(response))),
      };
    }

    /** The terminal observation for an incomplete Responses terminal event. */
    BodySemantic incompleteEventObservation(const std::string& type, const json::value* response, const std::optional<std::string>& topLevelError) {
      const json::value* incompleteDetails = response ? rt::get(response, "incomplete_details") : nullptr;
      const json::value* details = response && utils::isRecord(incompleteDetails) ? incompleteDetails : nullptr;
      std::optional<std::string> reason = boundedFailureKind(details ? rt::get(details, "reason") : nullptr);
      std::optional<std::string> kind = errorKind(responseError(response));
      return {
        .completed = false,
        .terminal_type = type,
        .failure_kind = reason ? reason : (kind ? kind : topLevelError),
        .error_code = rt::orNull(kind ? kind : topLevelError),
        .error_param = rt::orNull(errorParam(responseError(response))),
      };
    }

    /** The terminal observation for an error event or a top-level error payload. */
    BodySemantic errorEventObservation(const json::value& parsed, const std::optional<std::string>& topLevelError) {
      const json::value* error = rt::get(parsed, "error");
      const json::value* nested = utils::isRecord(error) ? error : &parsed;
      return {
        .completed = false,
        .terminal_type = "error",
        .failure_kind = topLevelError ? *topLevelError : std::string("error"),
        .error_code = rt::orNull(std::optional<std::string>(topLevelError ? *topLevelError : std::string("error"))),
        .error_param = rt::orNull(errorParam(nested)),
      };
    }

    /** The terminal observation one parsed SSE event carries, or null when it is not terminal. */
    std::optional<BodySemantic> sseTerminalObservation(const std::optional<std::string>& type, const json::value& parsed, const json::value* response,
                                                       const std::optional<std::string>& topLevelError) {
      if (type == "response.failed") return failedEventObservation(*type, response, topLevelError);
      if (type == "response.incomplete") return incompleteEventObservation(*type, response, topLevelError);
      if (type == "error" || utils::isRecord(rt::get(parsed, "error"))) return errorEventObservation(parsed, topLevelError);
      if (type == "response.completed") return BodySemantic{.completed = true, .terminal_type = type, .failure_kind = std::nullopt};
      return std::nullopt;
    }

    std::optional<BodySemantic> sseEventObservation(const std::string& rawEvent) {
      const SseEventFields fields = parseSseEventFields(rawEvent);
      if (fields.data.empty()) return std::nullopt;
      const std::string joined = rt::join(fields.data, "\n");
      if (joined == "[DONE]") return BodySemantic{.completed = true, .terminal_type = "[DONE]", .failure_kind = std::nullopt};
      json::value parsed;
      try {
        parsed = rt::jsonParse(joined);
      } catch (...) {
        return std::nullopt;
      }
      if (!isPlainRecord(parsed)) return std::nullopt;
      std::optional<std::string> type = fields.eventName;
      if (const std::optional<std::string_view> declared = rt::getString(parsed, "type")) type = std::string(*declared);
      const json::value* responseMember = rt::get(parsed, "response");
      const json::value* response = isPlainRecord(responseMember) ? responseMember : nullptr;
      std::optional<std::string> topLevelError = errorKind(rt::get(parsed, "error"));
      if (!topLevelError) topLevelError = type == "error" ? boundedFailureKind(rt::get(parsed, "code")) : std::nullopt;
      return sseTerminalObservation(type, parsed, response, topLevelError);
    }

    /** Failure kind used when no terminal SSE frame was observed. */
    std::optional<std::string> missingTerminalFailureKind(bool framingValid, bool terminalMissing, const std::string& termination) {
      if (framingValid) return std::nullopt;
      if (!terminalMissing) return "invalid_sse_framing";
      return termination == "read_error" ? "stream_read_error" : "missing_sse_terminal";
    }

    struct SseInspectorState {
      rt::TextDecoder decoder{};
      std::string buffer;
      bool pendingCarriageReturn = false;
      bool droppingOversizedEvent = false;
      bool observedFrame = false;
      bool framingValid = true;
      std::optional<BodySemantic> terminal;
      std::optional<std::string> terminalBody;
      bool terminalBodyTruncated = false;

      void observeEvent(const std::string& rawEvent) {
        const SseEventFields fields = parseSseEventFields(rawEvent);
        if (fields.hasUnknownField) framingValid = false;
        if (!fields.data.empty() && rt::join(fields.data, "\n") != "[DONE]") {
          try {
            const json::value parsed = rt::jsonParse(rt::join(fields.data, "\n"));
            if (!isPlainRecord(parsed)) framingValid = false;
          } catch (...) {
            framingValid = false;
          }
        }
        std::optional<BodySemantic> observation = sseEventObservation(rawEvent);
        if (!observation) return;
        if (terminalRank(observation->terminal_type) > terminalRank(terminal ? terminal->terminal_type : std::nullopt)) {
          terminal = std::move(observation);
          // Only a terminal event's own bytes are retained, bounded, so the
          // observed downstream terminal is replayable without keeping the trace.
          rt::Bytes encoded = replay_model::TEXT_ENCODER.encode(rawEvent);
          const TerminalBody retained = boundedTerminalBody(encoded);
          terminalBody = retained.base64;
          terminalBodyTruncated = retained.truncated;
        }
      }

      // `buffer.length > MAX_SSE_EVENT_CHARS` without measuring every small buffer: a UTF-16 length
      // is at most the UTF-8 byte count and at least a third of it.
      bool bufferTooLong() const {
        if (static_cast<double>(buffer.size()) <= replay_model::MAX_SSE_EVENT_CHARS) return false;
        if (static_cast<double>(buffer.size()) > 3 * replay_model::MAX_SSE_EVENT_CHARS) return true;
        return static_cast<double>(rt::length(buffer)) > replay_model::MAX_SSE_EVENT_CHARS;
      }

      void process() {
        std::size_t start = 0;
        {
          // Consumed events leave the buffer even when observing one throws, as the JS slices do.
          auto consume = rt::scopeExit([this, &start] { buffer.erase(0, start); });
          for (;;) {
            const std::size_t boundary = buffer.find("\n\n", start);
            if (boundary == std::string::npos) break;
            const std::string rawEvent = buffer.substr(start, boundary - start);
            start = boundary + 2;
            observedFrame = true;
            if (droppingOversizedEvent) {
              droppingOversizedEvent = false;
              continue;
            }
            observeEvent(rawEvent);
          }
        }
        if (bufferTooLong()) {
          framingValid = false;
          droppingOversizedEvent = true;
          buffer.clear();
        }
      }

      void append(const std::string& text, bool final) {
        std::string normalized = pendingCarriageReturn ? "\r" + text : text;
        pendingCarriageReturn = !final && normalized.ends_with("\r");
        if (pendingCarriageReturn) normalized.pop_back();
        normalized = rt::replaceAll(rt::replaceAll(normalized, "\r\n", "\n"), "\r", "\n");
        buffer += normalized;
        process();
      }

      SentinelClientBodyObservation finish(const std::string& termination) {
        append(decoder.decode(), true);
        if (pendingCarriageReturn) {
          buffer += "\n";
          pendingCarriageReturn = false;
        }
        process();
        const bool terminalMissing = !terminal;
        if (!observedFrame || !rt::trim(buffer).empty() || droppingOversizedEvent || terminalMissing) framingValid = false;
        std::optional<std::string> failureKind = terminal ? terminal->failure_kind : std::nullopt;
        if (!failureKind) failureKind = missingTerminalFailureKind(framingValid, terminalMissing, termination);
        return {
          .stream = true,
          .completed = terminal ? terminal->completed : false,
          .terminal_type = terminal ? terminal->terminal_type : std::nullopt,
          .failure_kind = failureKind,
          .framing_valid = framingValid,
          .error_code = rt::orNull(terminal ? terminal->error_code.toOptional() : std::nullopt),
          .error_param = rt::orNull(terminal ? terminal->error_param.toOptional() : std::nullopt),
          .terminal_body_base64 = rt::orNull(terminalBody),
          .terminal_body_truncated = terminalBodyTruncated,
        };
      }
    };

    /** Terminal type used when the internal observation carries no authoritative one. */
    std::optional<std::string> fallbackTerminalType(const SentinelFailureObservation& internal, bool cancelled) {
      if (internal.terminal_type == "response.completed" || internal.terminal_type == "response.failed" || internal.terminal_type == "response.incomplete" ||
          internal.terminal_type == "error" || cancelled) {
        return internal.terminal_type;
      }
      if (internal.status >= 400) return "http.error";
      if (internal.completed) return "http.completed";
      return internal.terminal_type;
    }

    /**
     * The client-visible generic literal. It is a real observed value, but it is
     * never allowed to replace a trigger-specific transport cause (`read_error`,
     * `premature_eof`, `inactivity_timeout`, `malformed_event`, ...) in the
     * diagnostic `failure_kind` slot.
     */
    constexpr std::array<std::string_view, 4> GENERIC_CLIENT_FAILURE_KINDS{"server_error", "error", "internal_error", "api_error"};

    bool isGenericClientFailureKind(const std::optional<std::string>& value) {
      return !value || std::find(GENERIC_CLIENT_FAILURE_KINDS.begin(), GENERIC_CLIENT_FAILURE_KINDS.end(), *value) != GENERIC_CLIENT_FAILURE_KINDS.end();
    }

    /** Keep the more specific of the observed and internal causes; never the literal over a cause. */
    std::optional<std::string> preferDiagnosticFailureKind(const std::optional<std::string>& observed, const std::optional<std::string>& internal) {
      if (observed && !isGenericClientFailureKind(observed)) return observed;
      if (internal && !isGenericClientFailureKind(internal)) return internal;
      return observed ? observed : internal;
    }

    /**
     * A synthetic post-commit terminal is reported to the client as `server_error`,
     * but the transport cause that produced it must stay in the diagnostic slot.
     * The generic literal remains the fallback only when no more specific cause
     * exists at all.
     */
    std::optional<std::string> diagnosticFailureKind(bool cancelled, const std::optional<std::string>& observedKind, const std::optional<std::string>& internalKind,
                                                     const std::optional<std::string>& syntheticTerminal) {
      if (cancelled) return std::nullopt;
      std::optional<std::string> preferred = preferDiagnosticFailureKind(observedKind, internalKind);
      if (preferred) return preferred;
      if (rt::truthy(syntheticTerminal)) return "server_error";
      return internalKind;
    }

    /** The client-visible error code, or the synthetic terminal literal when the client saw one. */
    std::optional<std::string> clientErrorCode(bool cancelled, const SentinelClientBodyObservation* body, const std::optional<std::string>& syntheticTerminal) {
      if (cancelled) return std::nullopt;
      std::optional<std::string> observed = body ? body->error_code.toOptional() : std::nullopt;
      if (observed) return observed;
      return rt::truthy(syntheticTerminal) ? std::optional<std::string>("server_error") : std::nullopt;
    }

    bool isGatewayOrProviderIncompleteReason(const std::optional<std::string>& value) {
      static const rt::Regex REASON("^(?:response_incomplete:)?(?:gateway|provider|upstream|server|network|timeout|deadline)[a-z0-9_.:-]*$", "i");
      return value && REASON.test(*value);
    }

    // `isPersistableSentinelFailure` over either observation type: `framing_valid` is an own
    // member of the client observation only, `synthetic_terminal_type` of the internal one.
    struct PersistableView {
      double status;
      std::optional<bool> stream;
      bool completed;
      std::optional<std::string> terminal_type;
      std::optional<std::string> failure_kind;
      std::optional<bool> framing_valid;
      bool hasSyntheticTerminalType;
      std::optional<std::string> synthetic_terminal_type;
    };

    bool isPersistableSentinelFailure(const PersistableView& observation) {
      if (observation.terminal_type == "cancelled") return false;
      if (observation.framing_valid && observation.stream == true && !*observation.framing_valid) return true;
      if (observation.status < 400 && observation.completed && observation.terminal_type == "response.completed") {
        return false;
      }
      if (observation.status >= 400) return true;
      if (observation.terminal_type == "response.incomplete") {
        return isGatewayOrProviderIncompleteReason(observation.failure_kind) ||
               (observation.hasSyntheticTerminalType && observation.synthetic_terminal_type.has_value());
      }
      if (observation.failure_kind) return true;
      if (observation.hasSyntheticTerminalType && observation.synthetic_terminal_type) return true;
      if (observation.terminal_type == "deadline" || observation.terminal_type == "eof" || observation.terminal_type == "error" ||
          observation.terminal_type == "response.failed")
        return true;
      return observation.stream == true && !observation.completed && observation.terminal_type && observation.terminal_type != "cancelled";
    }

    PersistableView persistableView(const SentinelFailureObservation& observation) {
      return {
        .status = observation.status,
        .stream = observation.stream,
        .completed = observation.completed,
        .terminal_type = observation.terminal_type,
        .failure_kind = observation.failure_kind,
        .framing_valid = std::nullopt,
        .hasSyntheticTerminalType = true,
        .synthetic_terminal_type = observation.synthetic_terminal_type,
      };
    }

    PersistableView persistableView(const SentinelClientFailureObservation& observation) {
      return {
        .status = observation.status,
        .stream = observation.stream,
        .completed = observation.completed,
        .terminal_type = observation.terminal_type,
        .failure_kind = observation.failure_kind,
        .framing_valid = observation.framing_valid,
        .hasSyntheticTerminalType = false,
        .synthetic_terminal_type = std::nullopt,
      };
    }
  }

  bool isSentinelReplayRequestId(const rt::KvValue& value) { return value.isString() && REQUEST_ID().test(value.asString()); }

  bool isSentinelReplayCaptureStatusRow(const rt::KvValue& value) {
    if (!rt::kvIsObject(value)) return false;
    const rt::KvValue& status = value["status"];
    const rt::KvValue& reason = value["reason"];
    const rt::KvValue& capturedAtMs = value["captured_at_ms"];
    const rt::KvValue& manifestKey = value["manifest_key"];
    const rt::KvValue& fingerprint = value["fingerprint"];
    const rt::KvValue& expiresAtMs = value["expires_at_ms"];
    if (!(value["version"].isNumber() && value["version"].asNumber() == 1) || !isSentinelReplayRequestId(value["request_id"]) || !status.isString() ||
        std::find(STORED_CAPTURE_STATUSES.begin(), STORED_CAPTURE_STATUSES.end(), status.asString()) == STORED_CAPTURE_STATUSES.end() ||
        (!reason.isNull() && (!reason.isString() || !STATUS_REASON().test(reason.asString()))) || !capturedAtMs.isNumber() ||
        !rt::Number::isSafeInteger(capturedAtMs.asNumber()) || capturedAtMs.asNumber() < 0 ||
        (!manifestKey.isNull() && (!manifestKey.isArray() || manifestKey.asArray().size() != 7)) ||
        (!fingerprint.isNull() && (!fingerprint.isString() || !replay_model::HEX_DIGEST.test(fingerprint.asString()))) ||
        (!expiresAtMs.isNull() && !isSafeIntegerNumber(expiresAtMs)))
      return false;
    if (status.asString() == "ready" || status.asString() == "incomplete") {
      return !manifestKey.isNull() && !fingerprint.isNull() && !expiresAtMs.isNull();
    }
    return manifestKey.isNull() && expiresAtMs.isNull();
  }

  replay_model::SentinelCompatibilityHeaders normalizeSentinelCompatibilityHeaders(const rt::Headers& headers) {
    json::object normalized;
    for (const std::string_view name : replay_model::COMPATIBILITY_HEADER_NAMES) {
      const std::optional<std::string> raw = headers.get(name);
      if (!raw) continue;
      const std::string value = rt::trim(*raw);
      if (!value.empty()) rt::objectSet(normalized, name, json::value(value));
    }
    return normalized;
  }

  std::shared_ptr<replay_model::SentinelReplayCaptureCandidate> captureAcceptedSentinelReplayInput(const rt::Request& req, const std::string& requestId) {
    if (req.method() != "POST") return nullptr;
    const rt::URL url(req.url());
    auto candidate = std::make_shared<replay_model::SentinelReplayCaptureCandidate>(replay_model::SentinelReplayCaptureCandidate{
      .endpoint = url.pathname() + url.search(),
      .method = req.method(),
      .body = nullptr,
      .content_type = optionalContentType(req.headers().get("content-type")),
      .compatibility_headers = normalizeSentinelCompatibilityHeaders(req.headers()),
      .request_id = requestId,
      .git_sha = config::runtimeGitSha(),
      .deno_revision = config::runtimeDeploymentId(),
      .body_omitted_reason = std::nullopt,
    });
    request::observeRawBodyOnce(
      req, [candidate](std::shared_ptr<rt::Bytes> bytes) { candidate->body = std::move(bytes); },
      [candidate](const std::string& reason) {
        candidate->body_omitted_reason = reason;
        if (reason == "body_invalid" && candidate->body) {
          zero(*candidate->body);
          candidate->body = nullptr;
        }
      },
      request::RawBodyStage::received);
    return candidate;
  }

  std::optional<replay_model::AcceptedSentinelReplayInput> materializeSentinelReplayInput(const std::shared_ptr<replay_model::SentinelReplayCaptureCandidate>& candidate) {
    if (!candidate || !candidate->body) {
      if (candidate && !candidate->body_omitted_reason) candidate->body_omitted_reason = "body_unavailable";
      return std::nullopt;
    }
    std::shared_ptr<rt::Bytes> body = candidate->body;
    candidate->body = nullptr;
    return replay_model::AcceptedSentinelReplayInput{
      .endpoint = candidate->endpoint,
      .method = candidate->method,
      .body = std::move(body),
      .content_type = candidate->content_type,
      .compatibility_headers = candidate->compatibility_headers,
      .request_id = candidate->request_id,
      .git_sha = candidate->git_sha,
      .deno_revision = candidate->deno_revision,
      .bodyStatus = candidate->body_omitted_reason,
    };
  }

  void zeroSentinelReplayInput(const replay_model::AcceptedSentinelReplayInput* input) {
    if (input && input->body) zero(*input->body);
  }

  void zeroSentinelReplayInput(const std::optional<replay_model::AcceptedSentinelReplayInput>& input) { zeroSentinelReplayInput(input ? &*input : nullptr); }

  replay_model::AcceptedSentinelReplayInput snapshotSentinelReplayInput(const replay_model::AcceptedSentinelReplayInput& input) {
    const std::optional<upstream_capture::SentinelUpstreamRecorder>& recorder = input.upstreamRecorder;
    upstream_capture::SentinelUpstreamTrace upstream =
      recorder ? recorder->snapshotAndSeal() : (input.upstream ? *input.upstream : upstream_capture::emptySentinelUpstreamTrace());
    if (recorder) recorder->dispose();
    return {
      .endpoint = input.endpoint,
      .method = input.method,
      .body = std::make_shared<rt::Bytes>(replay_model::cloneBytes(*input.body)),
      .content_type = input.content_type,
      .compatibility_headers = input.compatibility_headers,
      .request_id = input.request_id,
      .git_sha = input.git_sha,
      .deno_revision = input.deno_revision,
      .upstream = std::move(upstream),
      .bodyStatus = input.bodyStatus,
    };
  }

  void disposeSentinelUpstreamRecorder(const replay_model::AcceptedSentinelReplayInput* input) {
    if (input && input->upstreamRecorder) input->upstreamRecorder->dispose();
  }

  void disposeSentinelUpstreamRecorder(const std::optional<replay_model::AcceptedSentinelReplayInput>& input) {
    disposeSentinelUpstreamRecorder(input ? &*input : nullptr);
  }

  void discardSentinelReplayCaptureCandidate(const std::shared_ptr<replay_model::SentinelReplayCaptureCandidate>& candidate) {
    if (candidate && candidate->body) zero(*candidate->body);
    if (candidate) candidate->body = nullptr;
  }

  std::string sentinelFailureSignature(const SentinelClientFailureObservation& observation) {
    json::object signature;
    signature["status"] = rt::canonicalNumbers(json::value(observation.status));
    signature["stream"] = observation.stream;
    signature["completed"] = observation.completed;
    signature["terminal_type"] = observation.terminal_type ? json::value(*observation.terminal_type) : json::value(nullptr);
    signature["failure_kind"] = observation.failure_kind ? json::value(*observation.failure_kind) : json::value(nullptr);
    signature["framing_valid"] = observation.framing_valid;
    signature["provider_route"] = observation.provider_route;
    return rt::jsonStringify(signature);
  }

  std::optional<std::string> boundedFailureKind(const json::value& value) { return boundedFailureKind(&value); }

  std::optional<std::string> boundedFailureKind(const json::value* value) {
    static const rt::Regex FAILURE_KIND("^[A-Za-z0-9_.:-]{1,128}$", "");
    const std::optional<std::string_view> text = rt::getString(value);
    return text && FAILURE_KIND.test(*text) ? std::optional<std::string>(std::string(*text)) : std::nullopt;
  }

  std::optional<std::string> boundedErrorParam(const json::value& value) { return boundedErrorParam(&value); }

  std::optional<std::string> boundedErrorParam(const json::value* value) {
    static const rt::Regex ERROR_PARAM("^[A-Za-z0-9_.:[\\]-]{1,128}$", "");
    const std::optional<std::string_view> text = rt::getString(value);
    return text && ERROR_PARAM.test(*text) ? std::optional<std::string>(std::string(*text)) : std::nullopt;
  }

  SentinelClientBodyObservation inspectSentinelBufferedResponse(double status, const std::string& contentType, std::span<const std::uint8_t> bytes) {
    std::optional<BodySemantic> semantic;
    if (rt::includes(rt::toLowerCase(contentType), "json")) {
      try {
        const json::value parsed = rt::jsonParse(replay_model::TEXT_DECODER.decode(bytes));
        if (isPlainRecord(parsed)) {
          semantic = responseSemanticObservation(status, parsed);
          // A non-2xx JSON body the semantic reader did not classify is still an
          // HTTP error: its own `error.code`/`error.param` are the client-visible
          // cause and must survive instead of being dropped as a bare status.
          if (!semantic && status >= 400 && utils::isRecord(rt::get(parsed, "error"))) {
            semantic = BodySemantic{
              .completed = false,
              .terminal_type = "http.error",
              .failure_kind = errorKind(rt::get(parsed, "error")),
              .error_code = rt::orNull(errorKind(rt::get(parsed, "error"))),
              .error_param = rt::orNull(errorParam(rt::get(parsed, "error"))),
            };
          }
        }
      } catch (...) {
        // HTTP status remains authoritative when a buffered body is not valid JSON.
      }
    }
    const auto withBody = [bytes](SentinelClientBodyObservation observation) -> SentinelClientBodyObservation {
      if (observation.completed) return observation;
      const TerminalBody body = boundedTerminalBody(bytes);
      observation.terminal_body_base64 = rt::orNull(body.base64);
      observation.terminal_body_truncated = body.truncated;
      return observation;
    };
    if (semantic) return withBody(withSemantic(false, true, *semantic));
    if (status >= 400) {
      return withBody({
        .stream = false,
        .completed = false,
        .terminal_type = "http.error",
        .failure_kind = std::nullopt,
        .framing_valid = true,
        .error_code = nullptr,
        .error_param = nullptr,
      });
    }
    if (status == 202) {
      return {
        .stream = false,
        .completed = false,
        .terminal_type = "http.accepted",
        .failure_kind = std::nullopt,
        .framing_valid = true,
      };
    }
    return {
      .stream = false,
      .completed = true,
      .terminal_type = "http.completed",
      .failure_kind = std::nullopt,
      .framing_valid = true,
    };
  }

  rt::Task<std::optional<SentinelClientBodyObservation>> inspectSentinelBufferedResponseBody(rt::Response response) {
    std::optional<rt::Bytes> bytes;
    std::optional<SentinelClientBodyObservation> observation;
    try {
      auto read = readBoundedResponseClone(response);
      bytes = co_await std::move(read);
      const std::optional<std::string> contentType = response.headers().get("content-type");
      observation = inspectSentinelBufferedResponse(response.status(), contentType ? rt::toLowerCase(*contentType) : std::string(), *bytes);
    } catch (...) {
      observation = std::nullopt;
    }
    if (bytes) zero(*bytes);
    co_return observation;
  }

  SentinelSseInspector createSentinelSseInspector() {
    auto state = std::make_shared<SseInspectorState>();
    return {
      .push = [state](std::span<const std::uint8_t> bytes) { state->append(state->decoder.decode(bytes, {.stream = true}), false); },
      .finish = [state](const std::optional<std::string>& termination) { return state->finish(termination.value_or("eof")); },
    };
  }

  SentinelClientBodyObservation inspectSentinelSse(std::span<const std::uint8_t> bytes) {
    const SentinelSseInspector inspector = createSentinelSseInspector();
    inspector.push(bytes);
    return inspector.finish(std::nullopt);
  }

  SentinelClientFailureObservation resolveSentinelClientFailureObservation(const SentinelFailureObservation& internal) {
    return resolveSentinelClientFailureObservation(internal, static_cast<const SentinelClientBodyObservation*>(nullptr));
  }

  SentinelClientFailureObservation resolveSentinelClientFailureObservation(const SentinelFailureObservation& internal, const SentinelClientBodyObservation* body) {
    const bool cancelled = internal.terminal_type == "cancelled";
    const std::optional<std::string> syntheticTerminal = cancelled ? std::nullopt : internal.synthetic_terminal_type;
    const std::optional<std::string> fallbackTerminal = syntheticTerminal ? syntheticTerminal : fallbackTerminalType(internal, cancelled);
    const std::optional<std::string> internalKind = cancelled ? std::nullopt : internal.failure_kind;
    const std::optional<std::string> observedKind = cancelled ? std::nullopt : (body ? body->failure_kind : std::nullopt);
    const std::optional<std::string> bodyTerminalType = body ? body->terminal_type : fallbackTerminal;
    return {
      .status = internal.status,
      .stream = body ? body->stream : internal.stream.value_or(false),
      .completed = body ? body->completed : internal.completed,
      .terminal_type = cancelled ? fallbackTerminal : bodyTerminalType,
      .failure_kind = diagnosticFailureKind(cancelled, observedKind, internalKind, syntheticTerminal),
      .framing_valid = body ? body->framing_valid : internal.stream != true,
      .provider_route = internal.provider_route,
      .error_code = clientErrorCode(cancelled, body, syntheticTerminal),
      .error_param = cancelled ? std::nullopt : (body ? body->error_param.toOptional() : std::nullopt),
      .terminal_body_base64 = cancelled ? std::nullopt : (body ? body->terminal_body_base64.toOptional() : std::nullopt),
      .terminal_body_truncated = cancelled ? false : (body ? body->terminal_body_truncated.value_or(false) : false),
    };
  }

  SentinelClientFailureObservation resolveSentinelClientFailureObservation(const SentinelFailureObservation& internal,
                                                                          const std::optional<SentinelClientBodyObservation>& body) {
    return resolveSentinelClientFailureObservation(internal, body ? &*body : nullptr);
  }

  bool shouldPersistSentinelReplay(const SentinelFailureObservation& observation) {
    if (observation.terminal_type == "cancelled") return false;
    return isPersistableSentinelFailure(persistableView(observation));
  }

  bool shouldPersistSentinelReplay(const SentinelFailureObservation& observation, const SentinelClientFailureObservation& clientObservation) {
    if (observation.terminal_type == "cancelled") return false;
    return isPersistableSentinelFailure(persistableView(observation)) || isPersistableSentinelFailure(persistableView(clientObservation));
  }

  bool shouldPersistFailedRequestPayload(const SentinelFailureObservation& observation,
                                         const SentinelClientFailureObservation& clientObservation, bool deliveryInterrupted) {
    return deliveryInterrupted || observation.status >= 400 || clientObservation.status >= 400 ||
           observation.terminal_type == "response.incomplete" || clientObservation.terminal_type == "response.incomplete" ||
           observation.terminal_type == "cancelled" || clientObservation.terminal_type == "cancelled" ||
           shouldPersistSentinelReplay(observation, clientObservation);
  }

  bool shouldSignalSentinelIncident(const SentinelFailureObservation& observation, const SentinelClientFailureObservation& clientObservation) {
    if (observation.terminal_type == "cancelled") return false;
    if (clientObservation.stream && !clientObservation.framing_valid) return true;
    if (observation.status < 400 && observation.completed && observation.terminal_type == "response.completed") {
      return false;
    }
    if (observation.status >= 500) return true;
    if (isGatewayOrProviderIncompleteReason(clientObservation.failure_kind)) return true;
    if (observation.terminal_type == "response.incomplete") {
      return isGatewayOrProviderIncompleteReason(observation.failure_kind) || observation.synthetic_terminal_type.has_value() ||
             (clientObservation.terminal_type == "response.incomplete" && isGatewayOrProviderIncompleteReason(clientObservation.failure_kind));
    }
    if (observation.synthetic_terminal_type || observation.failure_kind) return true;
    return observation.terminal_type == "deadline" || observation.terminal_type == "eof" || observation.terminal_type == "error";
  }
}
