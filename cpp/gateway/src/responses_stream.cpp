#include "uos/responses_stream.hpp"

#include "uos/inference_deadline.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/utils.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uos::responses_stream {
  const json::object* responsesFailurePayload(const ResponsesStreamEvent& event) {
    if (event.type == "error") {
      if (const auto* error = rt::getObject(event.value, "error")) return error;
      return &event.value;
    }
    if (event.type == "response.failed") return rt::getObject(rt::get(rt::get(event.value, "response"), "error"));
    return nullptr;
  }

  const ResponsesStreamError* responsesStreamErrorOf(const rt::Reason& reason) noexcept {
    if (!reason) return nullptr;
    try {
      std::rethrow_exception(reason);
    } catch (const ResponsesStreamError& error) {
      return &error;
    } catch (...) {
    }
    return nullptr;
  }

  namespace {
    using ReadResult = rt::ReadableStreamReadResult<rt::ByteChunk>;
    using Controller = rt::ReadableStreamDefaultController<rt::ByteChunk>;

    const rt::ByteChunk& SSE_KEEPALIVE_FRAME() {
      static const rt::ByteChunk frame = rt::encodeUtf8(": keepalive\n\n");
      return frame;
    }

    rt::Reason reasonText(std::string_view text) { return rt::thrown(json::value(json::string(text))); }

    std::string join(const std::vector<std::string>& parts, std::string_view separator) {
      std::string out;
      for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index) out += separator;
        out += parts[index];
      }
      return out;
    }

    bool aborted(const std::optional<rt::AbortSignal>& signal) { return signal && signal->aborted(); }

    std::optional<ResponsesStreamEvent> parseEventBlock(const std::string& raw) {
      static const rt::Regex LINE_BREAK("\\r\\n|\\r|\\n");
      static const rt::Regex LEADING_SPACE("^ ");
      std::vector<std::string> data;
      for (const std::string& line : rt::split(raw, LINE_BREAK)) {
        if (line.empty() || line.starts_with(':')) continue;
        if (line == "data") data.push_back("");
        else if (line.starts_with("data:")) data.push_back(rt::replace(rt::slice(line, 5), LEADING_SPACE, ""));
      }
      if (data.empty()) return std::nullopt;

      json::value value;
      try {
        value = rt::jsonParse(join(data, "\n"));
      } catch (...) {
        throw ResponsesStreamError("Upstream emitted malformed Responses SSE JSON.", {.cause = std::current_exception(), .kind = "malformed_event"});
      }
      if (!utils::isRecord(value)) {
        throw ResponsesStreamError("Upstream emitted a non-object Responses SSE event.", {.cause = nullptr, .kind = "malformed_event"});
      }
      const std::optional<std::string> typeText = utils::getString(rt::get(value, "type"));
      const std::string type = typeText ? rt::trim(*typeText) : std::string();
      if (type.empty()) {
        throw ResponsesStreamError("Upstream emitted a Responses SSE event without a type.", {.cause = nullptr, .kind = "malformed_event"});
      }
      // Only a non-array object has a string `type` member.
      const json::object& object = value.get_object();
      const bool hasNestedError = rt::hasOwn(object, "error");
      const json::value* code = rt::get(object, "code");
      const json::value* message = rt::get(object, "message");
      const json::value* param = rt::get(object, "param");
      const bool isFlatError = code && (code->is_null() || (code->is_string() && !rt::trim(code->get_string()).empty())) && message && message->is_string() &&
                               !rt::trim(message->get_string()).empty() && param && (param->is_null() || param->is_string());
      const json::value* error = rt::get(object, "error");
      const json::value* response = rt::get(object, "response");
      const bool terminal = RESPONSES_TERMINAL_EVENT_TYPES.has(type);
      if ((type == "error" && (hasNestedError ? !utils::isRecord(error) || error->is_array() : !isFlatError)) ||
          (type != "error" && terminal && (!utils::isRecord(response) || response->is_array()))) {
        throw ResponsesStreamError("Upstream emitted a Responses terminal event with an invalid payload.", {.cause = nullptr, .kind = "malformed_event"});
      }
      return ResponsesStreamEvent{.raw = raw, .value = std::move(value.get_object()), .type = type, .terminal = terminal};
    }

    /**
     * Incremental SSE framing state. Bytes accumulate into a reusable buffer that
     * grows geometrically and never exceeds the protocol's ceiling; complete
     * blocks are handed back one at a time so the reader can yield between them.
     * Unlike a string accumulator, this neither re-encodes every fragmented event
     * nor allocates an unbounded intermediate string.
     */
    class SseEventFramer {
    public:
      // The state of one `frames(chunk)` generator: raw text of every complete
      // SSE block found in `chunk`, in wire order, one per step.
      struct Frames {
        const rt::ByteChunk* chunk;
        std::size_t index = 0;
        std::size_t segmentStart = 0;
        bool done = false;
      };

      explicit SseEventFramer(rt::TextDecoder decoder_) : decoder(std::move(decoder_)) {
        buffer.reserve(static_cast<std::size_t>(std::min(4'096.0, MAX_RESPONSES_SSE_EVENT_BYTES)));
      }

      // One step of `frames(chunk)`: the next complete block, or std::nullopt once
      // the chunk is consumed (its incomplete tail is then buffered).
      std::optional<std::string> next(Frames& frames) {
        if (frames.done) return std::nullopt;
        const rt::ByteChunk& chunk = *frames.chunk;
        while (frames.index < chunk.size()) {
          const std::size_t index = frames.index++;
          const int byte = chunk[index];
          ensurePendingWithinLimit(index - frames.segmentStart + 1);
          if (!isBoundary(byte)) {
            advanceBoundaryState(byte);
            continue;
          }
          append(chunk, frames.segmentStart, index + 1);
          frames.segmentStart = index + 1;
          return take();
        }
        frames.done = true;
        if (frames.segmentStart < chunk.size()) append(chunk, frames.segmentStart, chunk.size());
        return std::nullopt;
      }
      /** Raw text of the trailing block that never received a closing boundary. */
      std::string flush() { return take(); }
      /** True while bytes belonging to an incomplete block are buffered. */
      bool hasPending() const { return !buffer.empty(); }

    private:
      static ResponsesStreamError oversized() {
        return ResponsesStreamError("Upstream emitted an oversized Responses SSE event.", {.cause = nullptr, .kind = "event_too_large"});
      }

      void append(const rt::ByteChunk& chunk, std::size_t begin, std::size_t end) {
        const double nextLength = static_cast<double>(buffer.size() + (end - begin));
        if (nextLength > MAX_RESPONSES_SSE_EVENT_BYTES) throw oversized();
        buffer.insert(buffer.end(), chunk.begin() + static_cast<std::ptrdiff_t>(begin), chunk.begin() + static_cast<std::ptrdiff_t>(end));
      }

      std::string take() {
        std::string raw = decoder.decode(std::span<const std::uint8_t>(buffer));
        buffer.clear();
        thirdPreviousByte = -1;
        secondPreviousByte = -1;
        previousByte = -1;
        return raw;
      }

      bool isBoundary(int byte) const {
        // CRLF is one line terminator. The CR in CRLFCRLF must not terminate
        // early, while mixed LF+CRLF framing (\n\r\n) must still split.
        if (byte == 10) {
          return previousByte == 10 || (previousByte == 13 && thirdPreviousByte == 13 && secondPreviousByte == 10);
        }
        if (byte != 13) return false;
        if (previousByte == 13) return true;
        return previousByte == 10 && secondPreviousByte != 13;
      }

      void advanceBoundaryState(int byte) {
        thirdPreviousByte = secondPreviousByte;
        secondPreviousByte = previousByte;
        previousByte = byte;
      }

      void ensurePendingWithinLimit(std::size_t pendingBytes) const {
        if (static_cast<double>(buffer.size() + pendingBytes) > MAX_RESPONSES_SSE_EVENT_BYTES) throw oversized();
      }

      rt::TextDecoder decoder;
      rt::ByteChunk buffer;
      int thirdPreviousByte = -1;
      int secondPreviousByte = -1;
      int previousByte = -1;
    };

    struct ResponsesStreamChunk {
      /** Bytes carried by one upstream read; empty when the read carried none. */
      rt::ByteChunk bytes;
      /** True when the upstream reader reported that the stream ended. */
      bool done = false;
    };

    // The reader and the flags its cancellation reads: the abort listener holds
    // this part only, so the signal never keeps the whole session alive.
    struct ReaderControl {
      rt::ReadableStreamDefaultReader<rt::ByteChunk> reader;
      bool readerDone = false;
      bool cancelStarted = false;

      void cancelReaderOnce(rt::Reason reason) {
        if (cancelStarted || readerDone) return;
        cancelStarted = true;
        rt::spawn(reader.cancel(std::move(reason)), rt::ignoreRejection);
      }
    };

    // ResponsesStreamSession.
    struct ResponsesStreamSession {
      std::shared_ptr<ReaderControl> control;
      SseEventFramer framer;
      std::optional<rt::AbortSignal> signal;
      ReadResponsesStreamOptions options;
      bool sawEvent = false;
      rt::EventListener abort;
      double firstEventDeadlineAtMs = 0;

      void markParsedEvent(const ResponsesStreamEvent& parsed) {
        sawEvent = true;
        if (parsed.terminal) control->cancelReaderOnce(reasonText("Responses terminal event received"));
        if (options.onEvent) options.onEvent(parsed);
      }
      /** One step of `takeEvents(bytes)`: the next complete event framed by `bytes`, in wire order. */
      std::optional<ResponsesStreamEvent> takeEvent(SseEventFramer::Frames& frames) {
        for (;;) {
          std::optional<std::string> frame = framer.next(frames);
          if (!frame) return std::nullopt;
          std::optional<ResponsesStreamEvent> parsed = parseEventBlock(*frame);
          if (!parsed) continue;
          markParsedEvent(*parsed);
          return parsed;
        }
      }
      /** Trailing event of a stream that ended without a closing boundary. */
      std::optional<ResponsesStreamEvent> takeTrailingEvent() {
        if (!framer.hasPending()) return std::nullopt;
        std::optional<ResponsesStreamEvent> parsed = parseEventBlock(framer.flush());
        if (!parsed) return std::nullopt;
        markParsedEvent(*parsed);
        return parsed;
      }
      void cancel(rt::Reason reason) { control->cancelReaderOnce(std::move(reason)); }
      /** Releases the upstream reader, cancelling it when no terminal event arrived. */
      void dispose() {
        if (signal) signal->removeEventListener("abort", abort);
        if (!control->readerDone) control->cancelReaderOnce(reasonText("Responses stream consumer stopped before a terminal event"));
        control->reader.releaseLock();
      }
    };

    std::shared_ptr<ResponsesStreamSession> createResponsesStreamSession(const rt::ByteStream& stream, std::optional<rt::AbortSignal> signal,
                                                                         ReadResponsesStreamOptions options) {
      auto control = std::make_shared<ReaderControl>(ReaderControl{.reader = stream.getReader()});
      auto session = std::shared_ptr<ResponsesStreamSession>(new ResponsesStreamSession{
        .control = control, .framer = SseEventFramer(rt::TextDecoder()), .signal = signal, .options = std::move(options), .sawEvent = false, .abort = {},
        .firstEventDeadlineAtMs = 0});
      if (signal) {
        std::weak_ptr weakSignal = signal->state();
        session->abort = rt::EventListener([control, weakSignal] {
          rt::Reason reason;
          if (auto state = weakSignal.lock()) reason = rt::AbortSignal(state).reason();
          control->cancelReaderOnce(reason);
        });
        signal->addEventListener("abort", session->abort, {.once = true});
      }
      session->firstEventDeadlineAtMs = rt::dateNow() + session->options.firstEventTimeoutMs.value_or(inference_deadline::STREAM_FIRST_EVENT_DEADLINE_MS);
      return session;
    }

    rt::Task<ReadResult> readWithDeadline(std::shared_ptr<ResponsesStreamSession> session) {
      const double timeoutMs = session->sawEvent ? session->options.inactivityTimeoutMs.value_or(inference_deadline::STREAM_INACTIVITY_DEADLINE_MS)
                                                 : rt::Math::max(0.0, session->firstEventDeadlineAtMs - rt::dateNow());
      if (!(timeoutMs >= 0) || std::isinf(timeoutMs))
        throw rt::TypeError("Value " + rt::numberToString(timeoutMs) + " is outside the range [0, 9007199254740991]");
      const auto inactive = rt::withResolvers<ReadResult>();
      auto timer = rt::detail::createExactTimer([reject = inactive.reject] {
        reject(rt::makeReason(ResponsesStreamError("Upstream Responses stream became inactive.", {.cause = nullptr, .kind = "inactivity_timeout"})));
      }, timeoutMs);
      timer.unref();
      const auto cleanup = rt::scopeExit([&] {
        rt::clearTimeout(timer);
        // Release the deadline promise's race reaction when the read wins.
        inactive.resolve(ReadResult{.done = true, .value = std::nullopt});
      });
      const rt::Promise<ReadResult> read = session->control->reader.readPromise();
      const auto race = rt::race(read, inactive.promise);
      auto result = co_await race.wait();
      co_return result;
    }

    rt::Task<ResponsesStreamChunk> nextChunk(std::shared_ptr<ResponsesStreamSession> session) {
      if (aborted(session->signal)) rt::rethrow(session->signal->reason());
      ReadResult result = co_await readWithDeadline(session);
      if (aborted(session->signal)) rt::rethrow(session->signal->reason());
      session->control->readerDone = result.done;
      ResponsesStreamChunk chunk{.bytes = result.value ? std::move(*result.value) : rt::ByteChunk(), .done = result.done};
      if (chunk.bytes.empty()) co_return chunk;
      if (session->options.onActivity) {
        rt::Promise<void> activity = session->options.onActivity();
        co_await activity.wait();
      } else {
        co_await rt::microtask();
      }
      if (aborted(session->signal)) rt::rethrow(session->signal->reason());
      co_return chunk;
    }

    rt::Task<bool> consumeResponsesStreamBody(std::shared_ptr<ResponsesStreamSession> session, rt::Yield<ResponsesStreamEvent> yield) {
      for (;;) {
        ResponsesStreamChunk chunk = co_await nextChunk(session);
        SseEventFramer::Frames frames{.chunk = &chunk.bytes};
        for (;;) {
          std::optional<ResponsesStreamEvent> parsed = session->takeEvent(frames);
          if (!parsed) break;
          const bool terminal = parsed->terminal;
          ResponsesStreamEvent event = std::move(*parsed);
          co_await yield(std::move(event));
          if (terminal) co_return true;
        }
        if (chunk.done) break;
      }
      std::optional<ResponsesStreamEvent> trailing = session->takeTrailingEvent();
      if (trailing) {
        const bool terminal = trailing->terminal;
        ResponsesStreamEvent event = std::move(*trailing);
        co_await yield(std::move(event));
        if (terminal) co_return true;
      }
      co_return false;
    }

    /**
     * Forwards upstream events until a terminal one has been yielded, and reports
     * whether that happened. `markParsedEvent` closes the reader as soon as it sees
     * a terminal event, so `false` always means the upstream stream ended early.
     */
    rt::AsyncGenerator<ResponsesStreamEvent, bool> consumeResponsesStream(std::shared_ptr<ResponsesStreamSession> session) {
      return rt::AsyncGenerator<ResponsesStreamEvent, bool>(
        [session](rt::Yield<ResponsesStreamEvent> yield) { return consumeResponsesStreamBody(session, std::move(yield)); });
    }

    rt::Task<void> readResponsesStreamBody(rt::ByteStream stream, std::optional<rt::AbortSignal> signal, ReadResponsesStreamOptions options,
                                           rt::Yield<ResponsesStreamEvent> yield) {
      const std::shared_ptr<ResponsesStreamSession> session = createResponsesStreamSession(stream, signal, std::move(options));
      rt::Reason failure;
      bool returning = false;
      try {
        rt::AsyncGenerator<ResponsesStreamEvent, bool> consumer = consumeResponsesStream(session);
        const bool terminal = co_await yield.from(consumer);
        if (!terminal) throw ResponsesStreamError("Upstream Responses stream ended before a terminal event.", {.cause = nullptr, .kind = "premature_eof"});
      } catch (const rt::GeneratorReturn&) {
        returning = true;
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) {
        rt::Reason thrown;
        try {
          session->cancel(failure);
          if (aborted(signal)) rt::rethrow(signal->reason());
          if (responsesStreamErrorOf(failure)) std::rethrow_exception(failure);
          throw ResponsesStreamError("Upstream Responses stream could not be read.", {.cause = failure, .kind = std::nullopt});
        } catch (...) {
          thrown = std::current_exception();
        }
        session->dispose();
        std::rethrow_exception(thrown);
      }
      session->dispose();
      if (returning) throw rt::GeneratorReturn{};
    }

    // `await iterator.return(reason).catch(() => {})`.
    rt::Task<void> returnQuietly(ResponsesStreamIterator iterator) {
      try {
        co_await iterator.return_();
      } catch (...) {
      }
      // The settled `.catch()` promise is awaited one turn after it.
      co_await rt::microtask();
    }

    rt::Task<void> cancelPreflight(rt::AbortController cancellation, ResponsesStreamIterator iterator, rt::Reason reason) {
      if (!cancellation.signal().aborted()) cancellation.abort(reason);
      co_await returnQuietly(iterator);
    }

    rt::ByteChunk errorEvent(std::string_view message) {
      json::object error;
      rt::objectDefine(error, "type", json::string("server_error"));
      rt::objectDefine(error, "code", json::string("upstream_stream_error"));
      rt::objectDefine(error, "message", json::string(message));
      rt::objectDefine(error, "param", nullptr);
      json::object value;
      rt::objectDefine(value, "type", json::string("error"));
      rt::objectDefine(value, "error", std::move(error));
      return rt::encodeUtf8("event: error\ndata: " + rt::jsonStringify(json::value(std::move(value))) + "\n\n");
    }

    // `void Promise.resolve(callback()).catch(() => {})` inside a try. A function
    // object rather than a function, so argument-dependent lookup never prefers
    // std::invoke (rt::Reason is a std type).
    constexpr struct {
      template <class... A, class... P> void operator()(const MaybeAsyncCallback<A...>& callback, P&&... args) const {
        if (!callback) return;
        try {
          rt::Promise<void> result = callback(std::forward<P>(args)...);
          (void)result.catch_(rt::ignoreRejection);
        } catch (...) {
          // Lifecycle callbacks must never interfere with downstream delivery.
        }
      }
    } invoke{};

    struct ProxyState {
      ResponsesStreamIterator iterator;
      ProxyResponsesStreamOptions options;
      rt::AbortController localAbort;
      std::optional<ResponsesStreamEvent> pending;
      bool closed = false;
    };

    rt::Task<void> proxyPull(std::shared_ptr<ProxyState> state, Controller controller) {
      if (state->closed) co_return;
      rt::Reason failure;
      try {
        rt::IteratorResult<ResponsesStreamEvent> next;
        if (state->pending) next = rt::IteratorResult<ResponsesStreamEvent>{false, state->pending};
        else next = co_await state->iterator.next();
        state->pending.reset();
        // `cancel()` can flip `closed` while `pull` is awaiting the upstream read.
        if (state->closed) co_return;
        if (next.done) {
          state->closed = true;
          controller.close();
          co_return;
        }
        controller.enqueue(rt::encodeUtf8(next.value->raw));
        invoke(state->options.onEvent, *next.value);
        if (next.value->terminal) {
          state->closed = true;
          controller.close();
          rt::spawn(state->iterator.return_(), rt::ignoreRejection);
        }
      } catch (...) {
        failure = std::current_exception();
      }
      if (!failure) co_return;
      if (state->closed) co_return;
      if (!state->localAbort.signal().aborted() && !aborted(state->options.signal) && !aborted(state->options.downstreamSignal)) {
        controller.enqueue(errorEvent("The upstream stream ended unexpectedly."));
      }
      state->closed = true;
      controller.close();
      invoke(state->options.onFailure, failure);
    }

    void proxyCancel(const std::shared_ptr<ProxyState>& state, rt::Reason reason) {
      if (state->closed) return;
      state->closed = true;
      invoke(state->options.onCancel, reason);
      state->localAbort.abort(reason);
      if (state->options.abortUpstream) state->options.abortUpstream(reason);
      rt::spawn(state->iterator.return_(), rt::ignoreRejection);
    }

    // The heartbeat race's outcome: `{ kind: "heartbeat" }` or `{ kind: "read", result }`.
    struct KeepaliveOutcome {
      bool heartbeat = false;
      std::optional<ReadResult> result = std::nullopt;
    };

    struct KeepaliveState {
      rt::ReadableStreamDefaultReader<rt::ByteChunk> reader;
      double intervalMs = 0;
      bool closed = false;
      // The pending timer is tracked by the call that cancels it.
      std::function<void()> clearPendingHeartbeat;
      std::optional<rt::Promise<ReadResult>> pendingRead;
      std::function<void()> resolveHeartbeat;

      rt::Promise<std::string> heartbeat(const std::shared_ptr<KeepaliveState>& self) {
        return rt::newPromise<std::string>([this, self](const rt::Resolve<std::string>& resolve, const rt::Reject&) {
          resolveHeartbeat = [resolve] { resolve(std::string("heartbeat")); };
          std::weak_ptr<KeepaliveState> weak = self;
          const rt::Timeout timer = rt::setTimeout(
            [weak, resolve] {
              if (auto state = weak.lock()) {
                state->clearPendingHeartbeat = nullptr;
                state->resolveHeartbeat = nullptr;
              }
              resolve(std::string("heartbeat"));
            },
            intervalMs);
          clearPendingHeartbeat = [timer] { rt::clearTimeout(timer); };
        });
      }

      void stopHeartbeat() {
        if (clearPendingHeartbeat) clearPendingHeartbeat();
        clearPendingHeartbeat = nullptr;
        if (resolveHeartbeat) resolveHeartbeat();
        resolveHeartbeat = nullptr;
      }
    };

    rt::Task<void> keepalivePull(std::shared_ptr<KeepaliveState> state, Controller controller) {
      if (state->closed) co_return;
      rt::Reason failure;
      try {
        if (!state->pendingRead) state->pendingRead.emplace(state->reader.readPromise());
        KeepaliveOutcome outcome;
        if (state->intervalMs > 0) {
          rt::Promise<KeepaliveOutcome> read = state->pendingRead->then([](const ReadResult& result) { return KeepaliveOutcome{false, result}; });
          rt::Promise<KeepaliveOutcome> beat = state->heartbeat(state).then([](const std::string&) { return KeepaliveOutcome{true, std::nullopt}; });
          std::vector<rt::Promise<KeepaliveOutcome>> racers{read, beat};
          rt::Promise<KeepaliveOutcome> race = rt::race(std::move(racers));
          outcome = co_await race.wait();
        } else {
          rt::Promise<ReadResult> pendingRead = *state->pendingRead;
          ReadResult result = co_await pendingRead.wait();
          outcome.result = std::move(result);
        }
        if (outcome.heartbeat) {
          // Cancellation resolves the pending heartbeat, so a cancelled
          // consumer must not receive one more frame from that race.
          if (state->closed) co_return;
          controller.enqueue(SSE_KEEPALIVE_FRAME());
          co_return;
        }
        state->stopHeartbeat();
        state->pendingRead.reset();
        ReadResult& result = *outcome.result;
        if (state->closed) co_return;
        if (result.done) {
          state->closed = true;
          controller.close();
          co_return;
        }
        controller.enqueue(std::move(*result.value));
      } catch (...) {
        failure = std::current_exception();
      }
      if (!failure) co_return;
      if (state->closed) co_return;
      state->closed = true;
      state->stopHeartbeat();
      controller.error(failure);
    }

    void keepaliveCancel(const std::shared_ptr<KeepaliveState>& state, rt::Reason reason) {
      if (state->closed) return;
      state->closed = true;
      state->stopHeartbeat();
      rt::spawn(state->reader.cancel(std::move(reason)), rt::ignoreRejection);
    }
  }

  ResponsesStreamIterator readResponsesStream(rt::ByteStream stream, std::optional<rt::AbortSignal> signal, ReadResponsesStreamOptions options) {
    return ResponsesStreamIterator([stream = std::move(stream), signal = std::move(signal), options = std::move(options)](rt::Yield<ResponsesStreamEvent> yield) {
      return readResponsesStreamBody(stream, signal, options, std::move(yield));
    });
  }

  rt::Task<PreflightedResponsesStream> preflightResponsesStream(rt::ByteStream upstream, std::optional<rt::AbortSignal> signal,
                                                                PreflightResponsesStreamOptions options) {
    const rt::AbortController cancellation;
    std::vector<rt::AbortSignal> sources;
    if (signal) sources = {*signal, cancellation.signal()};
    const rt::AbortSignal streamSignal = signal ? rt::AbortSignal::any(sources) : cancellation.signal();
    ReadResponsesStreamOptions readOptions;
    readOptions.onActivity = options.onActivity;
    const ResponsesStreamIterator iterator = readResponsesStream(upstream, streamSignal, std::move(readOptions));
    const std::function<rt::Task<void>(rt::Reason)> cancel = [cancellation, iterator](rt::Reason reason) {
      return cancelPreflight(cancellation, iterator, std::move(reason));
    };
    rt::Reason failure;
    try {
      rt::IteratorResult<ResponsesStreamEvent> next = co_await iterator.next();
      std::optional<ResponsesStreamEvent> first = next.done ? std::nullopt : std::move(next.value);
      if (!first) {
        throw ResponsesStreamError("Upstream Responses stream ended before its first event.", {.cause = nullptr, .kind = "premature_eof"});
      }
      PreflightedResponsesStream preflighted{.first = std::move(*first), .iterator = iterator, .cancel = cancel};
      co_return preflighted;
    } catch (...) {
      failure = std::current_exception();
    }
    rt::Task<void> cancelled = cancel(failure);
    co_await std::move(cancelled);
    std::rethrow_exception(failure);
  }

  rt::Task<PreflightedResponsesStream> preflightResponsesStream(rt::ByteStream upstream, std::optional<rt::AbortSignal> signal) {
    PreflightResponsesStreamOptions options;
    return preflightResponsesStream(std::move(upstream), std::move(signal), std::move(options));
  }

  rt::Task<PreflightedResponsesStream> preflightResponsesStream(rt::ByteStream upstream) {
    PreflightResponsesStreamOptions options;
    return preflightResponsesStream(std::move(upstream), std::nullopt, std::move(options));
  }

  rt::ByteStream proxyResponsesStreamIterator(ResponsesStreamIterator iterator, ProxyResponsesStreamOptions options, std::optional<ResponsesStreamEvent> initialEvent) {
    auto state = std::make_shared<ProxyState>(ProxyState{.iterator = std::move(iterator), .options = std::move(options), .localAbort = rt::AbortController(),
                                                          .pending = std::move(initialEvent), .closed = false});
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.pull = [state](Controller controller) { return proxyPull(state, std::move(controller)); };
    source.cancel = [state](rt::Reason reason) { proxyCancel(state, std::move(reason)); };
    return rt::ByteStream(std::move(source));
  }

  rt::ByteStream withSseKeepalive(rt::ByteStream source, WithSseKeepaliveOptions options) {
    const rt::ReadableStreamDefaultReader<rt::ByteChunk> reader = source.getReader();
    const double configuredIntervalMs = options.intervalMs.value_or(SSE_KEEPALIVE_INTERVAL_MS);
    const double intervalMs = rt::Number::isFinite(configuredIntervalMs) && configuredIntervalMs > 0 ? configuredIntervalMs : 0;
    auto state = std::make_shared<KeepaliveState>(KeepaliveState{.reader = reader, .intervalMs = intervalMs, .closed = false, .clearPendingHeartbeat = nullptr,
                                                                  .pendingRead = std::nullopt, .resolveHeartbeat = nullptr});
    rt::UnderlyingSource<rt::ByteChunk> underlying;
    underlying.pull = [state](Controller controller) { return keepalivePull(state, std::move(controller)); };
    underlying.cancel = [state](rt::Reason reason) { keepaliveCancel(state, std::move(reason)); };
    return rt::ByteStream(std::move(underlying));
  }

  rt::ByteStream proxyResponsesStream(rt::ByteStream upstream, ProxyResponsesStreamOptions options) {
    const rt::AbortController cancellation;
    std::vector<rt::AbortSignal> sources;
    if (options.signal) sources = {*options.signal, cancellation.signal()};
    const rt::AbortSignal signal = options.signal ? rt::AbortSignal::any(sources) : cancellation.signal();
    ProxyResponsesStreamOptions proxied = options;
    proxied.abortUpstream = [cancellation, upstream, abortUpstream = options.abortUpstream](rt::Reason reason) {
      if (!cancellation.signal().aborted()) cancellation.abort(reason);
      // A consumer can cancel before the iterator's first read, so the session
      // never owns the reader; release the untouched source here instead.
      if (!upstream.locked()) rt::spawn(upstream.cancel(reason), rt::ignoreRejection);
      if (abortUpstream) abortUpstream(reason);
    };
    return proxyResponsesStreamIterator(readResponsesStream(upstream, signal), std::move(proxied));
  }
}
