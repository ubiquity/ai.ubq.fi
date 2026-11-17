// OpenRouter stream plumbing: validated SSE frames for both OpenAI wires.
#include "uos/provider/openrouter_streams.hpp"
#include "uos/chat/stream_translation.hpp"
#include "uos/http.hpp"
#include "uos/inference_deadline.hpp"
#include "uos/provider/health.hpp"
#include "uos/responses_stream.hpp"
#include "uos/upstream_wire.hpp"
#include "uos/utils.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace uos::provider::openrouter_streams {
  namespace {
    namespace telemetry = openai_telemetry;
    using stream_relay::ProviderStreamFrame;
    using Comment = deepseek::stream::DeepSeekStreamFrameComment;
    using Chunk = deepseek::stream::DeepSeekStreamFrameChunk;
    using Done = deepseek::stream::DeepSeekStreamFrameDone;
    using Reader = rt::ReadableStreamDefaultReader<rt::ByteChunk>;
    using ReadResult = rt::ReadableStreamReadResult<rt::ByteChunk>;
    const rt::Regex SSE_FRAME_BOUNDARY("\\r?\\n\\r?\\n");
    const rt::Regex SSE_LINE_BOUNDARY("\\r?\\n");

    OpenRouterStreamError streamError(std::string message, std::string kind, std::optional<rt::Reason> cause = std::nullopt) {
      const OpenRouterStreamErrorOptions options{cause, std::move(kind)};
      return OpenRouterStreamError(std::move(message), options);
    }
    OpenRouterStreamFailureKind failureKindForStreamError(const rt::Reason& error) {
      try { if (error) std::rethrow_exception(error); }
      catch (const OpenRouterStreamError& failure) { return failure.kind; }
      catch (const responses_stream::ResponsesStreamError& failure) {
        if (failure.kind == "malformed_event" || failure.kind == "premature_eof" || failure.kind == "read_error" || failure.kind == "inactivity_timeout") return failure.kind;
        return "read_error";
      } catch (...) {}
      return "read_error";
    }
    OpenRouterStreamFailureKind openRouterFinishReasonFailureKind(const std::optional<std::string>& finishReason) {
      static const rt::Regex valid("^[A-Za-z0-9_.:-]{1,64}$");
      return "openrouter_finish_reason:" + (finishReason && valid.test(*finishReason) ? *finishReason : "unrecognized");
    }
    telemetry::ResponseStreamTerminalType terminalTypeForStreamError(const rt::Reason& error, const rt::AbortSignal& downstreamSignal) {
      if (downstreamSignal.aborted()) return "cancelled";
      try { if (error) std::rethrow_exception(error); }
      catch (const OpenRouterStreamError& failure) { if (failure.kind == "inactivity_timeout") return "deadline"; }
      catch (const responses_stream::ResponsesStreamError& failure) {
        if (failure.kind == "inactivity_timeout") return "deadline";
        if (failure.kind == "premature_eof") return "eof";
      } catch (...) {}
      if (rt::isError(error) && rt::errorName(error) == "TimeoutError") return "deadline";
      if (rt::isError(error) && rt::errorName(error) == "AbortError") return "cancelled";
      return "error";
    }
    ProviderStreamFrame chatFrameFromPayload(const std::string& payload) {
      if (rt::trim(payload) == "[DONE]") return Done{};
      json::value parsed;
      try { parsed = rt::jsonParse(payload); }
      catch (...) { throw streamError("OpenRouter emitted malformed Chat Completions SSE JSON.", "malformed_event", std::current_exception()); }
      if (!utils::isRecord(parsed)) throw streamError("OpenRouter emitted a non-object Chat Completions chunk.", "invalid_chunk");
      if (const auto* error = rt::get(parsed, "error")) {
        const auto* message = utils::isRecord(error) ? rt::get(*error, "message") : error;
        const std::string text = message && message->is_string() ? std::string(message->as_string()) : "";
        throw streamError(!text.empty() ? text : "OpenRouter reported an upstream stream error.", "upstream_error");
      }
      if (!rt::getArray(parsed, "choices")) throw streamError("OpenRouter emitted a Chat Completions chunk without choices.", "invalid_chunk");
      return Chunk{parsed.as_object()};
    }
    std::optional<ProviderStreamFrame> parseOpenRouterSseEventBlock(const std::string& raw) {
      std::vector<std::string> data;
      std::vector<std::string> comments;
      for (const auto& line : rt::split(raw, SSE_LINE_BOUNDARY)) {
        if (line.empty()) continue;
        if (rt::startsWith(line, ":")) comments.push_back(line);
        else if (line == "data") data.emplace_back();
        else if (rt::startsWith(line, "data:")) {
          auto value = rt::slice(line, 5);
          if (rt::startsWith(value, " ")) value = rt::slice(value, 1);
          data.push_back(std::move(value));
        }
      }
      if (data.empty()) return comments.empty() ? std::nullopt : std::optional<ProviderStreamFrame>(Comment{rt::join(comments, "\n")});
      return chatFrameFromPayload(rt::join(data, "\n"));
    }
    class OpenRouterFrameReader {
    public:
      void push(const rt::ByteChunk* incoming) {
        buffered = rt::concat(buffered, incoming ? decoder.decode(*incoming, {.stream = true}) : decoder.decode());
        if (static_cast<double>(rt::length(buffered)) > OPENROUTER_SSE_FRAME_MAX_BYTES)
          throw streamError("OpenRouter SSE frame exceeded the gateway bound.", "frame_too_large");
      }
      std::optional<ProviderStreamFrame> shift() {
        for (;;) {
          const auto match = SSE_FRAME_BOUNDARY.exec(buffered);
          if (!match) return std::nullopt;
          const auto raw = rt::slice(buffered, 0, static_cast<double>(match->index));
          buffered = rt::slice(buffered, static_cast<double>(match->endIndex()));
          auto frame = parseOpenRouterSseEventBlock(raw);
          if (frame) return frame;
        }
      }
    private:
      rt::TextDecoder decoder;
      std::string buffered;
    };
    rt::Task<ReadResult> raceReaderRead(Reader reader, rt::AbortSignal signal) {
      rt::EventListener onAbort = [] {};
      const rt::Promise<ReadResult> aborted = rt::newPromise<ReadResult>([&onAbort, &signal](const rt::Resolve<ReadResult>&, const rt::Reject& reject) {
        const std::weak_ptr<rt::detail::AbortSignalState> weakSignal = signal.state();
        onAbort = rt::EventListener([reject, weakSignal] {
          if (const auto state = weakSignal.lock()) {
            const rt::AbortSignal active(state);
            const auto reason = active.reason();
            reject(rt::isError(reason) ? reason : rt::makeReason(rt::DOMException("The stream was aborted.", "AbortError")));
          }
        });
        signal.addEventListener("abort", onAbort, {.once = true});
        if (signal.aborted()) onAbort();
      });
      std::optional<ReadResult> result;
      rt::Reason failure;
      try {
        std::vector<rt::Promise<ReadResult>> racers{reader.readPromise(), aborted};
        const auto raced = rt::race(std::move(racers));
        result = co_await raced.wait();
      } catch (...) { failure = std::current_exception(); }
      signal.removeEventListener("abort", onAbort);
      if (failure) std::rethrow_exception(failure);
      co_return std::move(*result);
    }
    class OpenRouterStreamSession : public std::enable_shared_from_this<OpenRouterStreamSession> {
    public:
      OpenRouterStreamSession(Reader reader_, rt::AbortController deadline_, const OpenRouterStreamOptions& options)
        : reader(std::move(reader_)), deadline(std::move(deadline_)),
          signal(options.signal ? rt::AbortSignal::any({*options.signal, deadline.signal()}) : deadline.signal()),
          firstEventTimeoutMs(options.firstEventTimeoutMs), inactivityTimeoutMs(options.inactivityTimeoutMs) { resetWatchdog(); }
      rt::Task<std::optional<ProviderStreamFrame>> next() { return nextFrame(shared_from_this()); }
      void finish() {
        stopWatchdog();
        if (released) return;
        released = true;
        try {
          const auto cancellation = reader.cancelPromise(rt::thrown(json::value("OpenRouter Chat Completions stream finished")));
          releaseReaderLock();
          (void)cancellation.catch_([](rt::Reason) {});
        } catch (...) { releaseReaderLock(); }
      }
    private:
      void stopWatchdog() { if (watchdog) rt::clearTimeout(*watchdog); }
      void resetWatchdog() {
        stopWatchdog();
        const double timeoutMs = sawFrame ? inactivityTimeoutMs.value_or(inference_deadline::STREAM_INACTIVITY_DEADLINE_MS)
          : firstEventTimeoutMs.value_or(inference_deadline::STREAM_FIRST_EVENT_DEADLINE_MS);
        const auto controller = deadline;
        watchdog = rt::setTimeout([controller] { controller.abort(rt::makeReason(rt::DOMException("OpenRouter Chat Completions stream stalled.", "TimeoutError"))); }, timeoutMs);
      }
      void releaseReaderLock() { try { reader.releaseLock(); } catch (...) {} }
      static rt::Task<std::optional<ProviderStreamFrame>> nextFrame(std::shared_ptr<OpenRouterStreamSession> session) {
        for (;;) {
          auto frame = session->frames.shift();
          if (frame) { session->sawFrame = true; session->resetWatchdog(); co_return frame; }
          if (session->eof) co_return std::nullopt;
          const rt::Promise<ReadResult> reading(raceReaderRead(session->reader, session->signal));
          const auto result = co_await reading.wait();
          session->eof = result.done;
          session->frames.push(result.done ? nullptr : &*result.value);
        }
      }
      Reader reader;
      rt::AbortController deadline;
      rt::AbortSignal signal;
      std::optional<double> firstEventTimeoutMs;
      std::optional<double> inactivityTimeoutMs;
      OpenRouterFrameReader frames;
      bool eof = false;
      bool released = false;
      bool sawFrame = false;
      std::optional<rt::Timeout> watchdog;
    };
    rt::Reason streamFailure(const rt::Reason& error, const rt::AbortController& deadline) {
      try { if (error) std::rethrow_exception(error); }
      catch (const OpenRouterStreamError&) { return error; }
      catch (...) {}
      if (deadline.signal().aborted()) return rt::makeReason(streamError("OpenRouter Chat Completions stream stalled.", "inactivity_timeout", error));
      if (rt::isError(error) && (rt::errorName(error) == "TimeoutError" || rt::errorName(error) == "AbortError")) return error;
      return rt::makeReason(streamError("OpenRouter Chat Completions stream failed.", "read_error", error));
    }
    rt::Task<void> iterateOpenRouterChatCompletionStreamBody(rt::Response response, OpenRouterStreamOptions options, rt::Yield<ProviderStreamFrame> yield) {
      const auto body = response.body();
      if (!body) throw streamError("OpenRouter returned no Chat Completions stream body.", "premature_eof");
      const auto reader = body->getReader();
      const rt::AbortController deadline;
      const auto session = std::make_shared<OpenRouterStreamSession>(reader, deadline, options);
      rt::Reason failure;
      try {
        for (;;) {
          const rt::Promise<std::optional<ProviderStreamFrame>> reading(session->next());
          auto frame = co_await reading.wait();
          if (!frame) throw streamError("OpenRouter Chat Completions stream ended before [DONE].", "premature_eof");
          const bool done = std::holds_alternative<Done>(*frame);
          co_await yield(std::move(*frame));
          if (done) break;
        }
      } catch (const rt::GeneratorReturn&) { failure = std::current_exception(); }
      catch (...) { failure = streamFailure(std::current_exception(), deadline); }
      session->finish();
      if (failure) std::rethrow_exception(failure);
    }
    void recordFailureKind(const telemetry::UsageContext* context, const std::string& kind) {
      if (context && context->responseTelemetry) context->responseTelemetry->failureKind = kind;
    }
    stream_relay::ProviderChatStreamAdapter openRouterStreamAdapter() {
      stream_relay::ProviderChatStreamAdapter adapter;
      adapter.responseHeaders = upstream_wire::openRouterResponseHeaders;
      adapter.frames = [](rt::Response upstream, std::string, stream_relay::ProviderStreamFrameOptions options) -> stream_relay::ProviderStreamIterator {
        OpenRouterStreamOptions settings; settings.signal = options.signal;
        return iterateOpenRouterChatCompletionStream(upstream, settings);
      };
      adapter.recordResponseHealth = recordOpenRouterResponseHealth;
      adapter.recordProviderError = [](std::optional<double> status, std::optional<std::string> providerRequestId) {
        (void)health::recordOpenRouterProviderHealth("upstream_error", status, rt::dateNow, providerRequestId);
      };
      adapter.recordCancellation = [](const telemetry::UsageContext* context) { recordFailureKind(context, "cancellation"); };
      adapter.recordIncompleteResponse = [](const telemetry::UsageContext* context) { recordFailureKind(context, "incomplete_response"); };
      adapter.recordFinishFailureKind = [](const telemetry::UsageContext* context, std::optional<std::string> reason) { recordFailureKind(context, openRouterFinishReasonFailureKind(reason)); };
      adapter.recordTransportFailure = [](const telemetry::UsageContext* context, rt::Reason error, std::string terminalType) {
        recordFailureKind(context, terminalType == "cancelled" ? "cancellation" : failureKindForStreamError(error));
      };
      adapter.terminalTypeForError = terminalTypeForStreamError;
      adapter.streamErrorCode = "openrouter_upstream_stream_error";
      return adapter;
    }
    rt::Response withOpenRouterSseKeepalive(rt::Response response) {
      const auto body = response.body();
      if (!body) return response;
      const rt::ResponseInit init{.status = response.status(), .headers = response.headers()};
      return rt::Response(responses_stream::withSseKeepalive(*body), init);
    }
    bool openRouterResponsesEventIsSemantic(const responses_stream::ResponsesStreamEvent& event) {
      static const rt::OrderedSet<std::string> types{"response.output_text.delta", "response.refusal.delta", "response.function_call_arguments.delta",
        "response.reasoning_text.delta", "response.reasoning_summary_text.delta"};
      const auto delta = rt::getString(event.value, "delta");
      return types.has(event.type) && delta && !delta->empty();
    }
  }

  void recordOpenRouterResponseHealth(double status, std::optional<std::string> providerRequestId) {
    const std::string event = status == 401 || status == 403 ? "auth_invalid" : status == 402 || status == 429 ? "quota_exhausted"
      : status >= 500 ? "upstream_error" : status >= 400 ? "reachable" : "success";
    (void)health::recordOpenRouterProviderHealth(event, status, rt::dateNow, providerRequestId);
  }
  rt::AsyncGenerator<ProviderStreamFrame> iterateOpenRouterChatCompletionStream(rt::Response response) {
    return iterateOpenRouterChatCompletionStream(std::move(response), OpenRouterStreamOptions{});
  }
  rt::AsyncGenerator<ProviderStreamFrame> iterateOpenRouterChatCompletionStream(rt::Response response, const OpenRouterStreamOptions& options) {
    return rt::AsyncGenerator<ProviderStreamFrame>([response, options](rt::Yield<ProviderStreamFrame> yield) {
      return iterateOpenRouterChatCompletionStreamBody(response, options, std::move(yield));
    });
  }
  rt::Response streamOpenRouterChatCompletion(stream_relay::ProviderStreamSource upstream, std::optional<std::string> providerRequestId,
    const telemetry::UsageContext* usageContext, rt::AbortSignal downstreamSignal, rt::AbortSignal requestSignal, std::string upstreamModel) {
    return withOpenRouterSseKeepalive(stream_relay::relayChatCompletionStream(openRouterStreamAdapter(), std::move(upstream), std::move(providerRequestId),
      usageContext, std::move(downstreamSignal), std::move(requestSignal), std::move(upstreamModel)));
  }
  rt::Response streamOpenRouterResponses(rt::Response upstream, std::optional<std::string> providerRequestId,
    const telemetry::UsageContext* usageContext, rt::AbortSignal downstreamSignal, rt::AbortSignal requestSignal) {
    const auto body = upstream.body();
    if (!body) {
      telemetry::recordStreamTerminalType(usageContext, "error");
      return http::openaiError(502, "OpenRouter returned no Responses stream body.", "openrouter_upstream_invalid_response");
    }
    rt::Headers headers(upstream_wire::openRouterResponseHeaders(providerRequestId));
    headers.set("Content-Type", "text/event-stream"); headers.set("Cache-Control", "no-cache");
    headers.delete_("Content-Encoding"); headers.delete_("Content-Length");
    const auto context = usageContext ? std::make_shared<telemetry::UsageContext>(*usageContext) : nullptr;
    responses_stream::ProxyResponsesStreamOptions options;
    options.signal = requestSignal; options.downstreamSignal = downstreamSignal;
    options.onEvent = [context, providerRequestId](responses_stream::ResponsesStreamEvent event) {
      telemetry::recordFirstUpstreamSseEvent(context.get());
      chat::stream_translation::recordResponsesTerminal(event, context.get());
      if (event.terminal) {
        telemetry::recordStreamTerminal(context.get());
        const bool explicitProviderFault = event.type == "error" || event.type == "response.failed";
        const bool incompleteProviderFault = event.type == "response.incomplete" && telemetry::responseIncompleteFailureKind(event).has_value();
        if (explicitProviderFault || incompleteProviderFault) (void)health::recordOpenRouterProviderHealth("upstream_error", 200, rt::dateNow, providerRequestId);
        else if (event.type == "response.completed" || event.type == "response.incomplete") recordOpenRouterResponseHealth(200, providerRequestId);
        return;
      }
      if (openRouterResponsesEventIsSemantic(event)) {
        chat::stream_translation::markChatSemanticOutput(context.get()); telemetry::recordFirstSemanticCommitment(context.get());
      }
    };
    options.onFailure = [context, providerRequestId, downstreamSignal](rt::Reason error) {
      const auto terminalType = terminalTypeForStreamError(error, downstreamSignal);
      telemetry::recordStreamTerminalType(context.get(), terminalType);
      if (terminalType == "cancelled") return;
      recordFailureKind(context.get(), failureKindForStreamError(error));
      (void)health::recordOpenRouterProviderHealth("upstream_error", std::nullopt, rt::dateNow, providerRequestId);
      (void)telemetry::recordErrorUsage(context.get());
    };
    options.onCancel = [context](rt::Reason) {
      telemetry::recordStreamTerminalType(context.get(), "cancelled"); recordFailureKind(context.get(), "cancellation");
    };
    const auto stream = responses_stream::proxyResponsesStream(*body, options);
    const rt::ResponseInit init{.status = 200, .headers = headers};
    return rt::Response(responses_stream::withSseKeepalive(stream), init);
  }
}
