// Provider stream relay, extracted from src/openai.ts.
#include "uos/provider/stream_relay.hpp"
#include "uos/chat/stream_translation.hpp"
#include "uos/deepseek/responses_stream.hpp"
#include "uos/upstream_wire.hpp"
#include "uos/utils.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/weak.hpp"

#include <boost/asio/execution_context.hpp>
#include <boost/asio/io_context.hpp>
#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace uos::provider::stream_relay {
  namespace {
    namespace telemetry = openai_telemetry;
    using Controller = rt::ReadableStreamDefaultController<rt::ByteChunk>;
    using Chunk = deepseek::stream::DeepSeekStreamFrameChunk;
    using Comment = deepseek::stream::DeepSeekStreamFrameComment;
    using Done = deepseek::stream::DeepSeekStreamFrameDone;
    constexpr std::string_view UPSTREAM_STREAM_REFUSAL_MESSAGE = "Upstream Chat Completions stream failed.";

    json::object chatStreamErrorValue(const std::string& code, const std::string& message = std::string(UPSTREAM_STREAM_REFUSAL_MESSAGE)) {
      return {{"error", json::object{{"message", message}, {"type", "server_error"}, {"code", code}, {"param", nullptr}}}};
    }

    struct UpstreamStreamRefusal { int status; std::optional<std::string> code; std::optional<std::string> message; };

    rt::Task<UpstreamStreamRefusal> readUpstreamStreamRefusal(rt::Response response) {
      const UpstreamStreamRefusal fallback{response.status(), std::nullopt, std::nullopt};
      try {
        auto reading = response.json();
        const json::value payload = co_await std::move(reading);
        const json::value* error = utils::isRecord(payload) ? rt::get(payload, "error") : nullptr;
        if (!utils::isRecord(error)) co_return fallback;
        co_return UpstreamStreamRefusal{response.status(), utils::getString(rt::get(*error, "code")), utils::getString(rt::get(*error, "message"))};
      } catch (...) {
      }
      co_return fallback;
    }

    /** Closing is best effort: the client may already have cancelled the stream. */
    void closeController(const Controller& controller) {
      try { controller.close(); } catch (...) { /* Already closed or errored by the consumer. */ }
    }

    struct PendingCancellation {
      CancelPending callback;
      bool cancelled = false;
      explicit PendingCancellation(CancelPending callback_) : callback(std::move(callback_)) {}
      void cancel(rt::Reason reason) {
        if (cancelled) return;
        cancelled = true;
        if (callback) callback(reason);
      }
    };

    // Abort callbacks remain callable after completion. Release their signal cycles only
    // when the loop can no longer dispatch an abort, without invoking the callbacks.
    class PendingCancellationRegistry : public boost::asio::execution_context::service {
    public:
      static boost::asio::execution_context::id id;
      explicit PendingCancellationRegistry(boost::asio::execution_context& context) : service(context) {}
      void add(const std::shared_ptr<PendingCancellation>& pending) {
        std::erase_if(pending_, [](const auto& weak) { return weak.expired(); });
        pending_.push_back(pending);
      }
    private:
      void shutdown() override {
        auto pending = std::move(pending_);
        pending_.clear();
        for (const auto& weak : pending) {
          if (const auto live = weak.lock()) live->callback = {};
        }
      }
      std::vector<std::weak_ptr<PendingCancellation>> pending_;
    };
    boost::asio::execution_context::id PendingCancellationRegistry::id;

    struct RelayState {
      ProviderChatStreamAdapter adapter;
      std::optional<rt::Response> upstream;
      std::optional<rt::Promise<rt::Response>> pendingUpstream;
      std::optional<std::string> providerRequestId;
      std::optional<telemetry::UsageContext> usageContext;
      rt::AbortSignal downstreamSignal;
      rt::AbortSignal requestSignal;
      std::string upstreamModel;
      std::shared_ptr<PendingCancellation> pendingCancellation;
      rt::AbortController cancellation;
      rt::AbortSignal readSignal;
      std::optional<ProviderStreamIterator> iterator;
      rt::Reason cancellationReason;
      rt::WeakSet<rt::Response> cancelledResponses;

      RelayState(ProviderChatStreamAdapter adapter_, ProviderStreamSource source, std::optional<std::string> providerRequestId_,
                 const telemetry::UsageContext* context_, rt::AbortSignal downstreamSignal_, rt::AbortSignal requestSignal_,
                 std::string upstreamModel_, CancelPending cancelPending_)
        : adapter(std::move(adapter_)), providerRequestId(std::move(providerRequestId_)),
          usageContext(context_ ? std::optional<telemetry::UsageContext>(*context_) : std::nullopt),
          downstreamSignal(std::move(downstreamSignal_)), requestSignal(std::move(requestSignal_)), upstreamModel(std::move(upstreamModel_)),
          pendingCancellation(std::make_shared<PendingCancellation>(std::move(cancelPending_))), readSignal(rt::AbortSignal::any({requestSignal, cancellation.signal()})) {
        if (const auto* response = std::get_if<rt::Response>(&source)) upstream = *response;
        else pendingUpstream = std::get<rt::Promise<rt::Response>>(source);
      }

      const telemetry::UsageContext* context() const { return usageContext ? &*usageContext : nullptr; }
      void cancelPendingOnce(rt::Reason reason = {}) {
        pendingCancellation->cancel(reason);
      }
      void cancelResponse(const rt::Response& response) {
        if (cancelledResponses.has(response)) return;
        cancelledResponses.add(response);
        const auto body = response.body();
        if (!body || body->locked()) return;
        const auto reason = cancellationReason ? cancellationReason : downstreamSignal.reason();
        (void)body->cancelPromise(reason).catch_([](rt::Reason) {});
      }
    };

    template <class State> void ownPending(const std::shared_ptr<State>& state) {
      if (state->pendingCancellation->callback) {
        auto& registry = boost::asio::use_service<PendingCancellationRegistry>(rt::loop().context());
        registry.add(state->pendingCancellation);
      }
      const std::weak_ptr<rt::detail::AbortSignalState> weakSignal = state->downstreamSignal.state();
      rt::EventListener cancelPendingOnDownstreamAbort([pending = state->pendingCancellation, weakSignal] {
        if (const auto signal = weakSignal.lock()) pending->cancel(rt::AbortSignal(signal).reason());
      });
      if (state->downstreamSignal.aborted()) cancelPendingOnDownstreamAbort();
      else state->downstreamSignal.addEventListener("abort", cancelPendingOnDownstreamAbort, {.once = true});
      // Keep ownership of a deferred response even before the first downstream pull.
      if (state->pendingUpstream) {
        (void)state->pendingUpstream->then([state](rt::Response source) {
          if (!state->upstream) state->upstream = source;
          if (state->streamEnded()) state->cancelResponse(source);
        }).catch_([](rt::Reason) {});
      }
    }

    struct ChatRelayState : RelayState, std::enable_shared_from_this<ChatRelayState> {
      using RelayState::RelayState;
      bool closed = false;
      bool terminalSettled = false;
      bool semantic = false;
      std::optional<telemetry::UsageTokens> usage;

      bool streamEnded() const { return closed || downstreamSignal.aborted(); }
      void settleTerminal(const std::string& terminalType) {
        if (terminalSettled) return;
        terminalSettled = true;
        telemetry::recordStreamTerminalType(context(), terminalType);
      }
      rt::Task<void> finishStream(Controller controller) {
        if (closed) co_return;
        closed = true;
        co_await telemetry::recordCompletionUsage(context(), usage).wait();
        settleTerminal("response.completed");
        telemetry::recordStreamTerminal(context());
        adapter.recordResponseHealth(upstream ? upstream->status() : 200, providerRequestId);
        controller.enqueue(rt::encodeUtf8("data: [DONE]\n\n"));
        controller.close();
      }
      rt::Task<void> failStream(Controller controller, rt::Reason error) {
        if (closed) co_return;
        closed = true;
        const auto terminalType = adapter.terminalTypeForError(error, downstreamSignal);
        settleTerminal(terminalType);
        adapter.recordTransportFailure(context(), error, terminalType);
        if (usage) telemetry::recordTerminalUsage(context(), usage, false);
        if (terminalType != "cancelled") {
          adapter.recordProviderError(std::nullopt, providerRequestId);
          co_await telemetry::recordErrorUsage(context()).wait();
          controller.enqueue(rt::encodeUtf8("data: " + rt::jsonStringify(chatStreamErrorValue(adapter.streamErrorCode)) + "\n\n"));
        }
        controller.close();
      }
      rt::Task<void> refuseStream(Controller controller, rt::Response refusal) {
        if (closed) co_return;
        closed = true;
        // Snapshot provenance before the bounded error-body await.
        const bool gatewayDeadline = upstream_wire::isGatewayDeadlineResponse(refusal, requestSignal);
        const rt::Promise<UpstreamStreamRefusal> reading(readUpstreamStreamRefusal(refusal));
        const auto detail = co_await reading.wait();
        if (detail.status == 499 || gatewayDeadline) {
          settleTerminal(detail.status == 499 ? "cancelled" : "deadline");
          if (detail.status == 499) adapter.recordCancellation(context());
          else adapter.recordTransportFailure(context(), rt::makeReason(rt::JsError("The gateway deadline passed while waiting for the provider retry window.")), "deadline");
          telemetry::recordStreamTerminal(context());
          closeController(controller);
          co_return;
        }
        settleTerminal("response.failed");
        adapter.recordResponseHealth(detail.status, providerRequestId);
        adapter.recordProviderError(detail.status, providerRequestId);
        co_await telemetry::recordErrorUsage(context()).wait();
        controller.enqueue(rt::encodeUtf8("data: " + rt::jsonStringify(chatStreamErrorValue(detail.code.value_or(adapter.streamErrorCode), detail.message.value_or(std::string(UPSTREAM_STREAM_REFUSAL_MESSAGE)))) + "\n\n"));
        closeController(controller);
      }
      rt::Task<std::optional<ProviderStreamIterator>> ensureIterator(Controller controller) {
        if (iterator) co_return iterator;
        if (streamEnded()) co_return std::nullopt;
        auto source = upstream;
        if (!source && pendingUpstream) source = co_await pendingUpstream->wait();
        if (!source) co_return std::nullopt;
        if (streamEnded()) { cancelResponse(*source); co_return std::nullopt; }
        upstream = source;
        if (!source->ok()) {
          const rt::Promise<void> refusing(refuseStream(controller, *source));
          co_await refusing.wait();
          co_return std::nullopt;
        }
        iterator = adapter.frames(*source, upstreamModel, ProviderStreamFrameOptions{readSignal});
        co_return iterator;
      }
      rt::Task<void> pull(Controller controller) {
        if (closed) co_return;
        rt::Reason failure;
        try {
          const rt::Promise<std::optional<ProviderStreamIterator>> ensuring(ensureIterator(controller));
          const auto frames = co_await ensuring.wait();
          if (!frames) co_return;
          const auto next = co_await frames->next();
          if (next.done) {
            const rt::Promise<void> finishing(finishStream(controller));
            co_await finishing.wait();
            co_return;
          }
          if (const auto* comment = std::get_if<Comment>(&*next.value)) {
            controller.enqueue(rt::encodeUtf8(comment->text + "\n\n"));
            co_return;
          }
          if (std::holds_alternative<Done>(*next.value)) {
            iterator.reset();
            co_await frames->return_();
            const rt::Promise<void> finishing(finishStream(controller));
            co_await finishing.wait();
            co_return;
          }
          const auto& frame = std::get<Chunk>(*next.value);
          telemetry::recordFirstUpstreamSseEvent(context());
          if (!semantic && upstream_wire::chatChunkHasAnswerBearingOutput(frame.value)) {
            semantic = true;
            chat::stream_translation::markChatSemanticOutput(context());
            telemetry::recordFirstSemanticCommitment(context());
          }
          if (auto observed = telemetry::extractChatUsageTokens(rt::get(frame.value, "usage"))) usage = observed;
          controller.enqueue(rt::encodeUtf8("data: " + rt::jsonStringify(frame.value) + "\n\n"));
        } catch (...) { failure = std::current_exception(); }
        if (failure) {
          const rt::Promise<void> failing(failStream(controller, failure));
          co_await failing.wait();
        }
      }
      void cancel(rt::Reason reason) {
        if (closed) return;
        closed = true;
        cancellationReason = reason;
        settleTerminal("cancelled");
        adapter.recordCancellation(context());
        if (usage) telemetry::recordTerminalUsage(context(), usage, false);
        // Interrupt a parked read before queuing the generator return.
        if (!cancellation.signal().aborted()) cancellation.abort(reason);
        cancelPendingOnce(reason);
        if (upstream) cancelResponse(*upstream);
        if (iterator) rt::spawn(iterator->return_(), rt::ignoreRejection);
      }
    };

    struct ResponsesRelayState : RelayState, std::enable_shared_from_this<ResponsesRelayState> {
      std::string responseId;
      deepseek::responses_stream::DeepSeekResponsesStreamTranslator translator;
      bool settled = false;
      bool cancelled = false;
      bool semantic = false;
      std::optional<telemetry::UsageTokens> usage;
      double sequenceNumber = 0;

      ResponsesRelayState(ProviderStreamAdapter adapter_, const RelayResponsesStreamOptions& options)
        : RelayState(adapter_, options.upstream, options.providerRequestId, options.usageContext, options.downstreamSignal,
                     options.requestSignal, options.upstreamModel, options.cancelPending), responseId(options.responseId),
          translator(deepseek::responses_stream::createDeepSeekResponsesStreamTranslator(options.requestedModel, options.responseId, options.echo,
            options.createdAtSeconds, options.toolNames, options.customToolNames, adapter_.responsesProfile)) {}

      bool streamEnded() const { return settled || cancelled || downstreamSignal.aborted(); }
      void settleTerminal(const std::string& terminalType) {
        if (settled) return;
        settled = true;
        telemetry::recordStreamTerminalType(context(), terminalType);
      }
      void emit(const Controller& controller, const std::vector<json::object>& events) {
        for (auto event : events) {
          rt::objectDefine(event, "sequence_number", rt::makeNumber(sequenceNumber));
          controller.enqueue(rt::encodeUtf8(deepseek::responses_stream::encodeResponsesEvent(event)));
          sequenceNumber += 1;
        }
      }
      json::object failureEvent(const std::string& code, const std::string& message) const {
        return {{"type", "response.failed"}, {"response", json::object{{"id", responseId}, {"object", "response"}, {"status", "failed"},
          {"error", json::object{{"code", code}, {"message", message}}}}}};
      }
      bool emitEmptyCompletionFailure(const Controller& controller) {
        const auto output = translator.answerBearingOutput();
        const upstream_wire::CompletionAnswerBearingOutput answer{output.text, output.refusal, output.toolCallCount};
        if (translator.terminalKind() != "completed" || upstream_wire::isAnswerBearingCompletion(answer)) return false;
        if (usageContext && usageContext->responseTelemetry) {
          usageContext->responseTelemetry->failureKind = "empty_upstream_completion";
          usageContext->responseTelemetry->semanticOutputObserved = false;
        }
        telemetry::recordTerminalUsage(context(), usage, false);
        settleTerminal("response.failed");
        telemetry::recordStreamTerminal(context());
        auto events = translator.open();
        events.push_back(failureEvent("empty_upstream_completion", std::string(chat::stream_translation::EMPTY_UPSTREAM_COMPLETION_MESSAGE)));
        emit(controller, events);
        adapter.recordResponseHealth(upstream ? upstream->status() : 200, providerRequestId);
        return true;
      }
      rt::Task<void> finishStream(Controller controller) {
        if (settled) co_return;
        const auto cleanup = rt::scopeExit([controller] { closeController(controller); });
        if (emitEmptyCompletionFailure(controller)) co_return;
        const auto terminalKind = translator.terminalKind();
        emit(controller, translator.finish());
        if (terminalKind == "completed") {
          co_await telemetry::recordCompletionUsage(context(), usage).wait();
          settleTerminal("response.completed");
        } else {
          telemetry::recordTerminalUsage(context(), usage, false);
          if (terminalKind == "incomplete") adapter.recordIncompleteResponse(context());
          else {
            adapter.recordFinishFailureKind(context(), translator.upstreamFinishReason());
            adapter.recordProviderError(upstream ? upstream->status() : 200, providerRequestId);
          }
          settleTerminal(terminalKind == "incomplete" ? "response.incomplete" : "response.failed");
        }
        telemetry::recordStreamTerminal(context());
        adapter.recordResponseHealth(upstream ? upstream->status() : 200, providerRequestId);
      }
      rt::Task<void> refuseStream(Controller controller, rt::Response refusal) {
        if (settled) { closeController(controller); co_return; }
        const bool gatewayDeadline = upstream_wire::isGatewayDeadlineResponse(refusal, requestSignal);
        const rt::Promise<UpstreamStreamRefusal> reading(readUpstreamStreamRefusal(refusal));
        const auto detail = co_await reading.wait();
        if (detail.status == 499 || gatewayDeadline) {
          settleTerminal(detail.status == 499 ? "cancelled" : "deadline");
          if (detail.status == 499) adapter.recordCancellation(context());
          else adapter.recordTransportFailure(context(), rt::makeReason(rt::JsError("The gateway deadline passed while waiting for the provider retry window.")), "deadline");
          telemetry::recordStreamTerminal(context());
          closeController(controller);
          co_return;
        }
        settleTerminal("response.failed");
        adapter.recordResponseHealth(detail.status, providerRequestId);
        adapter.recordProviderError(detail.status, providerRequestId);
        co_await telemetry::recordErrorUsage(context()).wait();
        telemetry::recordStreamTerminal(context());
        emit(controller, {failureEvent(detail.code.value_or(adapter.streamErrorCode), detail.message.value_or(std::string(UPSTREAM_STREAM_REFUSAL_MESSAGE)))});
        closeController(controller);
      }
      rt::Task<std::optional<ProviderStreamIterator>> ensureIterator(Controller controller) {
        if (iterator) co_return iterator;
        if (streamEnded()) co_return std::nullopt;
        auto source = upstream;
        if (!source && pendingUpstream) source = co_await pendingUpstream->wait();
        if (!source) co_return std::nullopt;
        if (streamEnded()) { cancelResponse(*source); co_return std::nullopt; }
        upstream = source;
        if (!source->ok()) {
          const rt::Promise<void> refusing(refuseStream(controller, *source));
          co_await refusing.wait();
          co_return std::nullopt;
        }
        iterator = adapter.frames(*source, upstreamModel, ProviderStreamFrameOptions{readSignal});
        co_return iterator;
      }
      rt::Task<void> failStream(Controller controller, rt::Reason error) {
        if (settled) { closeController(controller); co_return; }
        const auto terminalType = adapter.terminalTypeForError(error, downstreamSignal);
        settleTerminal(terminalType);
        adapter.recordTransportFailure(context(), error, terminalType);
        if (usage) telemetry::recordTerminalUsage(context(), usage, false);
        const auto cleanup = rt::scopeExit([controller] { closeController(controller); });
        if (terminalType != "cancelled") {
          adapter.recordProviderError(std::nullopt, providerRequestId);
          co_await telemetry::recordErrorUsage(context()).wait();
          emit(controller, {failureEvent(adapter.streamErrorCode, "Upstream Chat Completions stream failed.")});
        }
      }
      std::vector<json::object> handleChunk(const json::object& chunk) {
        if (auto observed = telemetry::extractChatUsageTokens(rt::get(chunk, "usage"))) usage = observed;
        if (!semantic && upstream_wire::chatChunkHasAnswerBearingOutput(chunk)) {
          semantic = true;
          chat::stream_translation::markChatSemanticOutput(context());
          telemetry::recordFirstSemanticCommitment(context());
        }
        telemetry::recordFirstUpstreamSseEvent(context());
        return translator.push(chunk);
      }
      rt::Task<void> pull(Controller controller) {
        if (settled) co_return;
        rt::Reason failure;
        try {
          const rt::Promise<std::optional<ProviderStreamIterator>> ensuring(ensureIterator(controller));
          const auto frames = co_await ensuring.wait();
          if (!frames) co_return;
          for (;;) {
            const auto next = co_await frames->next();
            if (next.done) {
              const rt::Promise<void> finishing(finishStream(controller));
              co_await finishing.wait();
              co_return;
            }
            if (std::holds_alternative<Done>(*next.value)) {
              iterator.reset();
              co_await frames->return_();
              const rt::Promise<void> finishing(finishStream(controller));
              co_await finishing.wait();
              co_return;
            }
            if (std::holds_alternative<Comment>(*next.value)) continue;
            const auto events = handleChunk(std::get<Chunk>(*next.value).value);
            if (events.empty()) continue;
            emit(controller, events);
            co_return;
          }
        } catch (...) { failure = std::current_exception(); }
        if (failure) {
          const rt::Promise<void> failing(failStream(controller, failure));
          co_await failing.wait();
        }
      }
      void cancel(rt::Reason reason) {
        if (cancelled) return;
        cancelled = true;
        cancellationReason = reason;
        settleTerminal("cancelled");
        adapter.recordCancellation(context());
        if (usage) telemetry::recordTerminalUsage(context(), usage, false);
        if (!cancellation.signal().aborted()) cancellation.abort(reason);
        cancelPendingOnce(reason);
        if (upstream) cancelResponse(*upstream);
        if (iterator) rt::spawn(iterator->return_(), rt::ignoreRejection);
      }
    };

    template <class State> rt::Response responseFor(const std::shared_ptr<State>& state) {
      rt::Headers headers(state->adapter.responseHeaders(state->providerRequestId));
      headers.set("Content-Type", "text/event-stream");
      headers.set("Cache-Control", "no-cache");
      ownPending(state);
      rt::UnderlyingSource<rt::ByteChunk> source;
      source.pull = [state](Controller controller) { return state->pull(std::move(controller)); };
      source.cancel = [state](rt::Reason reason) { state->cancel(reason); };
      const rt::ByteStream body(std::move(source));
      const rt::ResponseInit init{.status = 200, .headers = headers};
      return rt::Response(body, init);
    }
  }

  rt::Response relayChatCompletionStream(ProviderChatStreamAdapter adapter, ProviderStreamSource upstreamSource,
    std::optional<std::string> providerRequestId, const telemetry::UsageContext* usageContext,
    rt::AbortSignal downstreamSignal, rt::AbortSignal requestSignal, std::string upstreamModel, CancelPending cancelPending) {
    const auto state = std::make_shared<ChatRelayState>(std::move(adapter), std::move(upstreamSource), std::move(providerRequestId), usageContext,
      std::move(downstreamSignal), std::move(requestSignal), std::move(upstreamModel), std::move(cancelPending));
    return responseFor(state);
  }

  rt::Response relayResponsesStream(ProviderStreamAdapter adapter, const RelayResponsesStreamOptions& options) {
    return responseFor(std::make_shared<ResponsesRelayState>(std::move(adapter), options));
  }
}
