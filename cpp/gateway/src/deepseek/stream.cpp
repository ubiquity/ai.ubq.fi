#include "uos/deepseek/stream.hpp"

#include "uos/deepseek/index.hpp"
#include "uos/inference_deadline.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uos::deepseek::stream {
  namespace {
    using ReadResult = rt::ReadableStreamReadResult<rt::ByteChunk>;

    DeepSeekStreamError streamError(std::string message, std::string kind) {
      return DeepSeekStreamError(std::move(message), DeepSeekStreamErrorOptions{.cause = std::nullopt, .kind = std::move(kind)});
    }

    DeepSeekStreamError streamError(std::string message, rt::Reason cause, std::string kind) {
      return DeepSeekStreamError(std::move(message), DeepSeekStreamErrorOptions{.cause = std::move(cause), .kind = std::move(kind)});
    }

    /**
     * Parses one complete SSE event block. `data:` payloads are the OpenAI Chat
     * Completions stream contract; DeepSeek's `: keep-alive` comments are relayed
     * verbatim because they are what keeps a long thinking turn from looking like
     * an idle connection to an edge proxy. Any other SSE field (`event:`, `id:`,
     * `retry:`) is not part of the Chat Completions contract and is not relayed.
     */
    // std::nullopt is null.
    std::optional<DeepSeekStreamFrame> parseSseEventBlock(std::string_view raw, const std::string& requestedModel) {
      static const rt::Regex LEADING_SPACE("^ ");
      std::vector<std::string> data;
      std::vector<std::string> comments;
      for (const std::string& line : rt::split(raw, index::SSE_LINE_BOUNDARY)) {
        if (line.empty()) continue;
        if (line.starts_with(":")) comments.push_back(line);
        else if (line == "data") data.push_back("");
        else if (line.starts_with("data:")) data.push_back(rt::replace(rt::slice(line, 5), LEADING_SPACE, ""));
      }
      if (data.empty()) {
        if (comments.empty()) return std::nullopt;
        return DeepSeekStreamFrameComment{.text = rt::join(comments, "\n")};
      }
      const std::string payload = rt::join(data, "\n");
      if (rt::trim(payload) == "[DONE]") return DeepSeekStreamFrameDone{};
      json::value parsed;
      rt::Reason cause;
      try {
        parsed = rt::jsonParse(payload);
      } catch (...) {
        cause = std::current_exception();
      }
      if (cause) throw streamError("Upstream emitted malformed Chat Completions SSE JSON.", cause, "malformed_event");
      index::NormalizationResult<json::object> normalized = index::normalizeDeepSeekChatCompletionChunk(parsed, requestedModel);
      if (const auto* failure = std::get_if<index::NormalizationResultNotOk>(&normalized)) {
        throw streamError("Upstream emitted an invalid Chat Completions chunk: " + failure->message, "invalid_chunk");
      }
      return DeepSeekStreamFrameChunk{.value = std::move(std::get<index::NormalizationResultOk<json::object>>(normalized).value)};
    }

    /**
     * Splits an incremental SSE byte stream into relayable frames, retaining the
     * unterminated tail between reads. Frames that carry neither `data:` nor a
     * comment are skipped, and the byte bound is enforced on the retained tail.
     */
    // The TS keeps `buffered.text` and re-slices it; here the retained tail is `text` from `start`
    // on (consumed frames are dropped on the next push) and its JS `.length` is kept in `length`,
    // which is what the bound compares.
    class DeepSeekFrameReader {
    public:
      explicit DeepSeekFrameReader(std::string requestedModel_) : requestedModel(std::move(requestedModel_)) {}

      // `push(incoming)`: nullptr is the end of the stream (`decoder.decode()`).
      void push(const rt::ByteChunk* incoming) {
        const std::string decoded = incoming ? decoder.decode(*incoming, {.stream = true}) : decoder.decode();
        if (start) {
          text.erase(0, start);
          start = 0;
        }
        // TextDecoder output is well formed, so plain appending is JS concatenation.
        text += decoded;
        length += static_cast<std::int64_t>(rt::length(decoded));
        if (static_cast<double>(length) > index::MAX_DEEPSEEK_SSE_FRAME_BYTES) {
          throw streamError("Upstream SSE frame exceeded the gateway bound.", "frame_too_large");
        }
      }

      // std::nullopt is null.
      std::optional<DeepSeekStreamFrame> shift() {
        for (;;) {
          const std::string_view buffered = std::string_view(text).substr(start);
          const std::optional<rt::RegexMatch> match = index::SSE_FRAME_BOUNDARY.exec(buffered);
          if (!match) return std::nullopt;
          const std::string raw(buffered.substr(0, match->byteIndex));
          length -= static_cast<std::int64_t>(match->endIndex());
          start += match->byteEndIndex();
          std::optional<DeepSeekStreamFrame> frame = parseSseEventBlock(raw, requestedModel);
          if (frame) return frame;
        }
      }

    private:
      std::string requestedModel;
      rt::TextDecoder decoder;
      std::string text;
      std::size_t start = 0;
      std::int64_t length = 0;
    };

    // `signal.reason instanceof Error ? signal.reason : new DOMException("The stream was aborted.", "AbortError")`
    rt::Reason streamAbortReason(const rt::AbortSignal& signal) {
      return rt::isError(signal.reason()) ? signal.reason() : rt::makeReason(rt::DOMException("The stream was aborted.", "AbortError"));
    }

    /** Races one upstream read against the composed cancellation/deadline signal. */
    rt::Task<ReadResult> raceReaderRead(rt::ReadableStreamDefaultReader<rt::ByteChunk> reader, rt::AbortSignal signal) {
      rt::EventListener onAbort = [] {};
      const rt::Promise<ReadResult> aborted = rt::newPromise<ReadResult>([&onAbort, &signal](const rt::Resolve<ReadResult>&, const rt::Reject& reject) {
        // The listener lives in the signal, so it reaches the signal weakly.
        const std::weak_ptr<rt::detail::AbortSignalState> weakSignal = signal.state();
        onAbort = rt::EventListener([reject, weakSignal] {
          if (const std::shared_ptr<rt::detail::AbortSignalState> state = weakSignal.lock()) reject(streamAbortReason(rt::AbortSignal(state)));
        });
        signal.addEventListener("abort", onAbort, {.once = true});
        if (signal.aborted()) onAbort();
      });
      std::optional<ReadResult> result;
      rt::Reason failure;
      try {
        std::vector<rt::Promise<ReadResult>> racers{reader.readPromise(), aborted};
        const rt::Promise<ReadResult> race = rt::race(std::move(racers));
        result = co_await race.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      signal.removeEventListener("abort", onAbort);
      if (failure) std::rethrow_exception(failure);
      co_return std::move(*result);
    }

    /**
     * Owns one upstream SSE read loop: the reader lock, the first-event and
     * inactivity watchdogs, and the frame queue. Kept outside the generator so
     * each concern is independently readable.
     */
    class DeepSeekStreamSession : public std::enable_shared_from_this<DeepSeekStreamSession> {
    public:
      DeepSeekStreamSession(rt::ReadableStreamDefaultReader<rt::ByteChunk> reader_, std::string requestedModel, rt::AbortController deadline_,
                            const DeepSeekStreamOptions& options)
        : reader(std::move(reader_)),
          deadline(std::move(deadline_)),
          signal(options.signal ? rt::AbortSignal::any({*options.signal, deadline.signal()}) : deadline.signal()),
          firstEventTimeoutMs(options.firstEventTimeoutMs),
          inactivityTimeoutMs(options.inactivityTimeoutMs),
          frames(std::move(requestedModel)) {
        resetWatchdog();
      }

      void finish() {
        stopWatchdog();
        if (released) return;
        released = true;
        try {
          const rt::Promise<void> cancellation = reader.cancelPromise(rt::thrown(json::value("DeepSeek Chat Completions stream finished")));
          releaseReaderLock();
          (void)cancellation.catch_([](const rt::Reason&) {});
        } catch (...) {
          releaseReaderLock();
        }
      }

      rt::Task<std::optional<DeepSeekStreamFrame>> next() { return nextFrame(shared_from_this()); }

    private:
      void stopWatchdog() {
        if (watchdog) rt::clearTimeout(*watchdog);
      }

      void resetWatchdog() {
        stopWatchdog();
        const double timeoutMs = sawFrame ? inactivityTimeoutMs.value_or(inference_deadline::STREAM_INACTIVITY_DEADLINE_MS)
                                          : firstEventTimeoutMs.value_or(inference_deadline::STREAM_FIRST_EVENT_DEADLINE_MS);
        const rt::AbortController controller = deadline;
        watchdog = rt::setTimeout(
          [controller] { controller.abort(rt::makeReason(rt::DOMException("DeepSeek Chat Completions stream stalled.", "TimeoutError"))); }, timeoutMs);
      }

      void releaseReaderLock() {
        try {
          reader.releaseLock();
        } catch (...) {
          // The reader may already be released by a failing upstream stream.
        }
      }

      static rt::Task<std::optional<DeepSeekStreamFrame>> nextFrame(std::shared_ptr<DeepSeekStreamSession> session) {
        for (;;) {
          std::optional<DeepSeekStreamFrame> frame = session->frames.shift();
          if (frame) {
            session->sawFrame = true;
            session->resetWatchdog();
            co_return frame;
          }
          if (session->eof) co_return std::nullopt;
          const rt::Promise<ReadResult> reading(raceReaderRead(session->reader, session->signal));
          ReadResult result = co_await reading.wait();
          session->eof = result.done;
          session->frames.push(result.done ? nullptr : &*result.value);
        }
      }

      rt::ReadableStreamDefaultReader<rt::ByteChunk> reader;
      rt::AbortController deadline;
      rt::AbortSignal signal;
      std::optional<double> firstEventTimeoutMs;
      std::optional<double> inactivityTimeoutMs;
      DeepSeekFrameReader frames;
      bool eof = false;
      bool released = false;
      bool sawFrame = false;
      std::optional<rt::Timeout> watchdog;
    };

    // The generator's `catch (error)`: the error it rethrows.
    rt::Reason streamFailure(const rt::Reason& error, const rt::AbortController& deadline) {
      try {
        std::rethrow_exception(error);
      } catch (const DeepSeekStreamError&) {
        return error;
      } catch (...) {
      }
      if (deadline.signal().aborted()) return rt::makeReason(streamError("DeepSeek Chat Completions stream stalled.", error, "inactivity_timeout"));
      if (index::isTimeoutError(error) || (rt::isError(error) && rt::errorName(error) == "AbortError")) return error;
      return rt::makeReason(streamError("DeepSeek Chat Completions stream failed.", error, "read_error"));
    }

    rt::Task<void> iterateDeepSeekChatCompletionStreamBody(rt::Response response, std::string requestedModel, DeepSeekStreamOptions options,
                                                           rt::Yield<DeepSeekStreamFrame> yield) {
      const std::optional<rt::ByteStream> body = response.body();
      if (!body) throw streamError("Upstream returned no Chat Completions stream body.", "premature_eof");
      const rt::ReadableStreamDefaultReader<rt::ByteChunk> reader = body->getReader();

      const rt::AbortController deadline;
      const auto session = std::make_shared<DeepSeekStreamSession>(reader, requestedModel, deadline, options);
      // `try { ... } catch (error) { ... } finally { session.finish(); }`: a return() at the yield
      // unwinds through here as rt::GeneratorReturn, which only the `finally` sees.
      rt::Reason failure;
      try {
        for (;;) {
          const rt::Promise<std::optional<DeepSeekStreamFrame>> pending(session->next());
          std::optional<DeepSeekStreamFrame> frame = co_await pending.wait();
          if (!frame) throw streamError("Upstream Chat Completions stream ended before [DONE].", "premature_eof");
          const bool done = std::holds_alternative<DeepSeekStreamFrameDone>(*frame);
          co_await yield(std::move(*frame));
          if (done) break;
        }
      } catch (const rt::GeneratorReturn&) {
        failure = std::current_exception();
      } catch (...) {
        failure = streamFailure(std::current_exception(), deadline);
      }
      session->finish();
      if (failure) std::rethrow_exception(failure);
    }
  }

  rt::AsyncGenerator<DeepSeekStreamFrame> iterateDeepSeekChatCompletionStream(rt::Response response, std::string requestedModel) {
    return iterateDeepSeekChatCompletionStream(std::move(response), std::move(requestedModel), DeepSeekStreamOptions{});
  }

  rt::AsyncGenerator<DeepSeekStreamFrame> iterateDeepSeekChatCompletionStream(rt::Response response, std::string requestedModel, const DeepSeekStreamOptions& options) {
    return rt::AsyncGenerator<DeepSeekStreamFrame>([response, requestedModel, options](rt::Yield<DeepSeekStreamFrame> yield) {
      return iterateDeepSeekChatCompletionStreamBody(response, requestedModel, options, std::move(yield));
    });
  }
}
