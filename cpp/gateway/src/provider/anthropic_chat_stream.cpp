#include "uos/provider/anthropic_chat_stream.hpp"

#include "uos/inference_deadline.hpp"
#include "uos/responses_stream.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uos::provider::anthropic_chat_stream {
  namespace {
    using responses_stream::MAX_RESPONSES_SSE_EVENT_BYTES;
    using responses_stream::ResponsesStreamError;
    using ReadResult = rt::ReadableStreamReadResult<rt::ByteChunk>;

    bool record(const json::value* value) { return rt::isRecord(value); }

    ResponsesStreamError malformed(std::string message) { return ResponsesStreamError(std::move(message), {.cause = nullptr, .kind = "malformed_event"}); }

    std::string join(const std::vector<std::string>& parts, std::string_view separator) {
      std::string out;
      for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index) out += separator;
        out += parts[index];
      }
      return out;
    }

    std::optional<ChatFrame> parse(const std::string& raw) {
      static const rt::Regex LINE_BREAK("\\r\\n|\\r|\\n");
      static const rt::Regex LEADING_SPACE("^ ");
      std::vector<std::string> data;
      std::vector<std::string> comments;
      for (const std::string& line : rt::split(raw, LINE_BREAK)) {
        if (line.starts_with(':')) comments.push_back(line);
        if (line.starts_with("data:")) data.push_back(rt::replace(rt::slice(line, 5), LEADING_SPACE, ""));
      }
      if (data.empty()) return comments.empty() ? std::nullopt : std::optional<ChatFrame>(ChatFrameComment{.text = join(comments, "\n")});
      const std::string joined = join(data, "\n");
      if (rt::trim(joined) == "[DONE]") return ChatFrame(ChatFrameDone{});
      json::value value;
      try {
        value = rt::jsonParse(joined);
      } catch (...) {
        throw ResponsesStreamError("Claude Chat emitted malformed SSE JSON.", {.cause = std::current_exception(), .kind = "malformed_event"});
      }
      if (!record(&value)) throw malformed("Claude Chat emitted a non-object stream chunk.");
      if (const json::value* error = rt::get(value, "error")) {
        throw ResponsesStreamError("Claude Chat reported an upstream stream error.", {.cause = rt::thrown(*error), .kind = "read_error"});
      }
      const json::value* choices = rt::get(value, "choices");
      if (!choices || !choices->is_array()) throw malformed("Claude Chat stream chunk has no choices.");
      return ChatFrame(ChatFrameChunk{.value = std::move(value.get_object())});
    }

    // frames(): the incremental framing state; each `push(incoming)` generator is a Push cursor.
    class Frames {
    public:
      struct Push {
        // `incoming`: the chunk, or nullptr when the stream ended (flush).
        const rt::ByteChunk* incoming;
        bool started = false;
        bool done = false;
      };

      std::optional<ChatFrame> next(Push& push) {
        if (push.done) return std::nullopt;
        if (!push.started) {
          push.started = true;
          buffer += push.incoming ? decoder.decode(std::span<const std::uint8_t>(*push.incoming), {.stream = true}) : decoder.decode();
        }
        static const rt::Regex BOUNDARY("\\r\\n\\r\\n|\\n\\n|\\r\\r");
        for (;;) {
          const std::optional<rt::RegexMatch> match = BOUNDARY.exec(buffer);
          if (!match && push.incoming) {
            bounded(buffer);
            push.done = true;
            return std::nullopt;
          }
          const std::size_t end = match ? match->byteIndex : buffer.size();
          const std::string raw = buffer.substr(0, end);
          bounded(raw);
          buffer = match ? buffer.substr(match->byteEndIndex()) : std::string();
          std::optional<ChatFrame> event = parse(raw);
          if (!match) push.done = true;
          if (event) return event;
          if (!match) return std::nullopt;
        }
      }

    private:
      static void bounded(std::string_view value) {
        if (static_cast<double>(rt::utf8ByteLength(value)) > MAX_RESPONSES_SSE_EVENT_BYTES)
          throw ResponsesStreamError("Claude Chat stream frame exceeded its bound.", {.cause = nullptr, .kind = "event_too_large"});
      }

      std::string buffer;
      rt::TextDecoder decoder;
    };

    rt::Reason abortedError(const rt::AbortSignal& signal) {
      return rt::isError(signal.reason()) ? signal.reason() : rt::makeReason(rt::DOMException("Claude Chat was aborted.", "AbortError"));
    }

    rt::Task<ReadResult> read(rt::ReadableStreamDefaultReader<rt::ByteChunk> reader, rt::AbortSignal signal) {
      rt::EventListener abort = [] {};
      const rt::Promise<ReadResult> rejected = rt::newPromise<ReadResult>([&abort, &signal](const rt::Resolve<ReadResult>&, const rt::Reject& reject) {
        std::weak_ptr weakSignal = signal.state();
        abort = rt::EventListener([reject, weakSignal] {
          if (auto state = weakSignal.lock()) reject(abortedError(rt::AbortSignal(state)));
        });
        signal.addEventListener("abort", abort, {.once = true});
        if (signal.aborted()) abort();
      });
      std::optional<ReadResult> result;
      rt::Reason failure;
      try {
        const rt::Promise<ReadResult> pending = reader.readPromise();
        std::vector<rt::Promise<ReadResult>> racers{pending, rejected};
        const rt::Promise<ReadResult> race = rt::race(std::move(racers));
        result = co_await race.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      signal.removeEventListener("abort", abort);
      if (failure) {
        std::rethrow_exception(failure);
      }
      co_return std::move(*result);
    }

    rt::Reason readFailure(const rt::Reason& error, const rt::AbortSignal& downstream, const rt::AbortSignal& deadline) {
      if (downstream.aborted()) return abortedError(downstream);
      if (deadline.aborted())
        return rt::makeReason(ResponsesStreamError("Claude Chat stream became inactive.", {.cause = error, .kind = "inactivity_timeout"}));
      if (responses_stream::responsesStreamErrorOf(error)) return error;
      return rt::makeReason(ResponsesStreamError("Claude Chat stream could not be read.", {.cause = error, .kind = std::nullopt}));
    }

    // The mutable state the abort listener and the deadline timer share with the body.
    struct ReadState {
      std::optional<rt::Timeout> timeout;
      bool sawFrame = false;
      bool cancelled = false;
    };

    rt::Task<void> readAnthropicChatStreamBody(rt::Response response, rt::AbortSignal downstream, rt::Yield<ChatFrame> yield) {
      const std::optional<rt::ByteStream> body = response.body();
      if (!body) throw ResponsesStreamError("Claude Chat returned no stream body.", {.cause = nullptr, .kind = "premature_eof"});
      const rt::ReadableStreamDefaultReader<rt::ByteChunk> reader = body->getReader();
      Frames framer;
      const rt::AbortController deadline;
      std::vector<rt::AbortSignal> sources{downstream, deadline.signal()};
      const rt::AbortSignal signal = rt::AbortSignal::any(sources);
      auto state = std::make_shared<ReadState>();
      std::weak_ptr weakSignal = signal.state();
      const rt::EventListener cancel([state, reader, weakSignal] {
        if (state->cancelled) return;
        state->cancelled = true;
        rt::Reason reason;
        if (auto signalState = weakSignal.lock()) reason = rt::AbortSignal(signalState).reason();
        rt::spawn(reader.cancel(reason), rt::ignoreRejection);
      });
      const auto arm = [&state, &deadline] {
        rt::clearTimeout(state->timeout);
        state->timeout = rt::setTimeout([deadline] { deadline.abort(rt::makeReason(rt::DOMException("Claude Chat stream stalled.", "TimeoutError"))); },
                                        state->sawFrame ? inference_deadline::STREAM_INACTIVITY_DEADLINE_MS : inference_deadline::STREAM_FIRST_EVENT_DEADLINE_MS);
      };
      signal.addEventListener("abort", cancel, {.once = true});
      arm();
      rt::Reason failure;
      bool returning = false;
      try {
        for (;;) {
          const ReadResult next = co_await read(reader, signal);
          Frames::Push push{.incoming = next.done ? nullptr : &*next.value};
          bool finished = false;
          for (;;) {
            std::optional<ChatFrame> frame = framer.next(push);
            if (!frame) break;
            state->sawFrame = true;
            arm();
            const bool isDone = std::holds_alternative<ChatFrameDone>(*frame);
            ChatFrame yielded = std::move(*frame);
            co_await yield(std::move(yielded));
            if (isDone) {
              finished = true;
              break;
            }
          }
          if (finished) break;
          if (next.done) throw ResponsesStreamError("Claude Chat stream ended before [DONE].", {.cause = nullptr, .kind = "premature_eof"});
        }
      } catch (const rt::GeneratorReturn&) {
        returning = true;
      } catch (...) {
        failure = std::current_exception();
      }
      const rt::Reason thrown = failure ? readFailure(failure, downstream, deadline.signal()) : nullptr;
      rt::clearTimeout(state->timeout);
      signal.removeEventListener("abort", cancel);
      cancel();
      reader.releaseLock();
      if (thrown) std::rethrow_exception(thrown);
      if (returning) throw rt::GeneratorReturn{};
    }
  }

  rt::AsyncGenerator<ChatFrame> readAnthropicChatStream(rt::Response response, rt::AbortSignal downstream) {
    return rt::AsyncGenerator<ChatFrame>([response, downstream](rt::Yield<ChatFrame> yield) { return readAnthropicChatStreamBody(response, downstream, std::move(yield)); });
  }
}
